#include <vector>
#pragma once

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define namespace wayland_namespace
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-shell-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

struct WaylandContext {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    zxdg_output_manager_v1* xdg_output_manager = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractional_scale_manager = nullptr;

    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layer_surface = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractional_scale = nullptr;
    // Surface scale (e.g. 1.5). width/height below are logical surface size;
    // buffer_width/buffer_height are the GL buffer size in pixels.
    double scale = 1.0;
    int buffer_width = 0;
    int buffer_height = 0;
    wl_egl_window* egl_window = nullptr;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig egl_config = nullptr;

    // Geometry is in compositor-global logical coordinates (the same space as
    // cursor positions and layer-surface sizes). With fractional scaling the
    // wl_output mode is in physical pixels, so xdg-output is preferred.
    struct OutputInfo {
    wl_output* output;
    zxdg_output_v1* xdg_output;
    bool has_logical_size;
    int x;
    int y;
    int width;
    int height;
    char name[32];
};
    std::vector<OutputInfo*> outputs;
    int current_output_index = 0;
    bool configured = false;
    int width = 0;
    int height = 0;
    int margin_left = 0;
    int margin_top = 0;
};

enum CompositorType {
    COMPOSITOR_HYPRLAND,
    COMPOSITOR_SWAY,
    COMPOSITOR_GENERIC
};

CompositorType DetectCompositor();
CompositorType GetCompositorType();

bool GetGlobalCursorPosition(int& x, int& y);

bool SetupWaylandContext(WaylandContext* wl, int width, int height);
void CleanWaylandContext(WaylandContext* wl);

void SwitchWaylandOutput(int direction);
void MoveToFocusedMonitor();
