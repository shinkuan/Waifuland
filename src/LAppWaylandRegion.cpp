#include "LAppWayland.hpp"
#include <GL/glew.h>
#include <vector>
#include <iostream>
#include <chrono>

// Reads one alpha sample per BLOCK_SIZE x BLOCK_SIZE logical block. When
// framebuffer blits are available, the frame is first downsampled on the GPU
// (nearest filtering picks roughly the block center), so only a tiny image is
// read back instead of the whole (possibly HiDPI) buffer every frame.
static bool ReadBlockAlphaDownsampled(int bw, int bh, int cols, int rows, std::vector<unsigned char>& out) {
    static GLuint s_fbo = 0, s_rb = 0;
    static int s_cols = 0, s_rows = 0;
    if (!glBlitFramebuffer || !glGenFramebuffers) return false;

    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

    if (!s_fbo) {
        glGenFramebuffers(1, &s_fbo);
        glGenRenderbuffers(1, &s_rb);
    }
    if (s_cols != cols || s_rows != rows) {
        glBindRenderbuffer(GL_RENDERBUFFER, s_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, cols, rows);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_rb);
        s_cols = cols;
        s_rows = rows;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    bool ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glBlitFramebuffer(0, 0, bw, bh, 0, 0, cols, rows, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
        out.resize((size_t)cols * rows * 4);
        glReadPixels(0, 0, cols, rows, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    return ok;
}

void UpdateWaylandInputRegion(WaylandContext* wl) {
    if (!wl || !wl->compositor || !wl->surface) return;

    // The region is in logical surface coordinates; the GL buffer may be
    // larger (buffer = logical * scale on fractionally scaled outputs).
    const int width = wl->width;
    const int height = wl->height;
    const int bw = wl->buffer_width;
    const int bh = wl->buffer_height;
    if (width <= 0 || height <= 0 || bw <= 0 || bh <= 0) return;

    const int BLOCK_SIZE = 10;
    const int cols = (width + BLOCK_SIZE - 1) / BLOCK_SIZE;
    const int rows = (height + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // alpha(col, row) with row 0 at the top
    std::vector<unsigned char> samples;
    std::vector<unsigned char> alpha((size_t)cols * rows, 0);
    if (ReadBlockAlphaDownsampled(bw, bh, cols, rows, samples)) {
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                alpha[(size_t)r * cols + c] = samples[((size_t)(rows - 1 - r) * cols + c) * 4 + 3];
    } else {
        // Fallback: read the full buffer and sample block centers on the CPU
        samples.resize((size_t)bw * bh * 4);
        glReadPixels(0, 0, bw, bh, GL_RGBA, GL_UNSIGNED_BYTE, samples.data());
        const double sx = (double)bw / width;
        const double sy = (double)bh / height;
        for (int r = 0; r < rows; r++) {
            int by = bh - 1 - (int)((r * BLOCK_SIZE + BLOCK_SIZE / 2) * sy);
            if (by < 0) by = 0;
            for (int c = 0; c < cols; c++) {
                int bx = (int)((c * BLOCK_SIZE + BLOCK_SIZE / 2) * sx);
                if (bx >= bw) bx = bw - 1;
                alpha[(size_t)r * cols + c] = samples[((size_t)by * bw + bx) * 4 + 3];
            }
        }
    }

    // Merge opaque runs per row to keep the rectangle count low.
    wl_region* region = wl_compositor_create_region(wl->compositor);
    for (int r = 0; r < rows; r++) {
        int y = r * BLOCK_SIZE;
        int start_x = -1;
        for (int c = 0; c < cols; c++) {
            bool opaque = alpha[(size_t)r * cols + c] > 10; // Alpha > 10
            int x = c * BLOCK_SIZE;
            if (opaque) {
                if (start_x < 0) start_x = x;
            } else if (start_x >= 0) {
                wl_region_add(region, start_x, y, x - start_x, BLOCK_SIZE);
                start_x = -1;
            }
        }
        if (start_x >= 0) {
            wl_region_add(region, start_x, y, width - start_x, BLOCK_SIZE);
        }
    }

    wl_surface_set_input_region(wl->surface, region);
    wl_region_destroy(region);
}
