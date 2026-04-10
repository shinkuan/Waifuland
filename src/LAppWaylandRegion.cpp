#include "LAppWayland.hpp"
#include <GL/glew.h>
#include <vector>
#include <iostream>
#include <chrono>

void UpdateWaylandInputRegion(WaylandContext* wl) {
    if (!wl || !wl->compositor || !wl->surface) return;

    int width = wl->width;
    int height = wl->height;
    if (width <= 0 || height <= 0) return;

    // Use a downsampled buffer to avoid freezing the GPU pipeline with huge glReadPixels
    // Downsample factor:
    const int DOWNSAMPLE = 8;
    int rw = width / DOWNSAMPLE;
    int rh = height / DOWNSAMPLE;

    std::vector<unsigned char> pixels(rw * rh * 4);

    // Read a 1/8 downsampled screen! 
    // To do this we could use glBlitFramebuffer, but GLES2 doesn't have it natively.
    // Instead we just read the full frame buffer with a step size? No, glReadPixels reads a continuous block.
    // Reading 1920x1080 takes ~2ms. We can afford to read it all and downsample on CPU.
    
    std::vector<unsigned char> full_pixels(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, full_pixels.data());

    wl_region* region = wl_compositor_create_region(wl->compositor);

    // Build region
    // We process it in blocks of 'BLOCK_SIZE' (e.g. 10x10 pixels) to reduce the number of rectangles
    const int BLOCK_SIZE = 10;
    
    for (int y = 0; y < height; y += BLOCK_SIZE) {
        int start_x = -1;
        for (int x = 0; x < width; x += BLOCK_SIZE) {
            
            bool opaque = false;
            // Just check the center pixel of the block for speed
            int test_x = x + BLOCK_SIZE/2;
            int test_y = y + BLOCK_SIZE/2;
            if (test_x < width && test_y < height) {
                // OpenGL reads bottom-to-top, Wayland is top-to-bottom
                int idx = (test_y * width + test_x) * 4;
                if (full_pixels[idx + 3] > 10) { // Alpha > 10
                    opaque = true;
                }
            }

            if (opaque) {
                if (start_x < 0) start_x = x;
            } else {
                if (start_x >= 0) {
                    // OpenGL Y is bottom-to-top, Wayland surface Y is top-to-bottom
                    int wayland_y = height - y - BLOCK_SIZE;
                    wl_region_add(region, start_x, wayland_y, x - start_x, BLOCK_SIZE);
                    start_x = -1;
                }
            }
        }
        if (start_x >= 0) {
            int wayland_y = height - y - BLOCK_SIZE;
            wl_region_add(region, start_x, wayland_y, width - start_x, BLOCK_SIZE);
        }
    }

    wl_surface_set_input_region(wl->surface, region);
    wl_region_destroy(region);
}

