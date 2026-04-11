#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include "LAppWayland.hpp"
#include "LAppDelegate.hpp"
#include "LAppPal.hpp"
#include <linux/input-event-codes.h>
#include <iostream>

// --- Wayland Pointer Handlers ---
static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {}
static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {}
static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    double x = wl_fixed_to_double(surface_x);
    double y = wl_fixed_to_double(surface_y);
    LAppDelegate::GetInstance()->OnMouseCallBack(nullptr, x, y);
}
static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    int glfw_action = (state == 1) ? 1 : 0; // PRESS=1, RELEASE=0 (matches GLFW definitions)
    int glfw_btn = -1;
    if (button == BTN_LEFT) glfw_btn = 0; // MOUSE_BUTTON_LEFT
    else if (button == BTN_RIGHT) glfw_btn = 1; // MOUSE_BUTTON_RIGHT
    else if (button == BTN_MIDDLE) glfw_btn = 2; // MOUSE_BUTTON_MIDDLE
    
    std::cout << "[Wayland] Button " << button << " state " << state << " mapped to " << glfw_btn << " action " << glfw_action << std::endl;

    if (glfw_btn != -1) {
        LAppDelegate::GetInstance()->OnMouseCallBack(nullptr, glfw_btn, glfw_action, 0);
    }
}
static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    double offset = wl_fixed_to_double(value);
    LAppDelegate::GetInstance()->OnScrollCallBack(nullptr, 0, offset > 0 ? -1 : 1);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

// --- Wayland Seat Handlers ---
static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    WaylandContext* wl = static_cast<WaylandContext*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->pointer) {
        wl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl->pointer, &pointer_listener, wl);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl->pointer) {
        wl_pointer_release(wl->pointer);
        wl->pointer = nullptr;
    }
}
static void seat_handle_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

// --- Layer Surface Handlers ---
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t width, uint32_t height) {
    WaylandContext* wl = static_cast<WaylandContext*>(data);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    wl->width = width;
    wl->height = height;
    
    // Dynamically resize EGL buffer when configure passes the new size
    if (wl->egl_window) {
        wl_egl_window_resize(wl->egl_window, wl->width, wl->height, 0, 0);
    }
    
    wl->configured = true;
    std::cout << "[Wayland] Configured! width: " << wl->width << ", height: " << wl->height << std::endl;
}
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    LAppDelegate::GetInstance()->AppEnd();
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    layer_surface_configure,
    layer_surface_closed,
};

// --- Registry Handlers ---
static void output_handle_geometry(void* data, struct wl_output* wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform) {
    WaylandContext::OutputInfo* info = static_cast<WaylandContext::OutputInfo*>(data);
    info->x = x;
    info->y = y;
}
static void output_handle_mode(void* data, struct wl_output* wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    WaylandContext::OutputInfo* info = static_cast<WaylandContext::OutputInfo*>(data);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        info->width = width;
        info->height = height;
    }
}
static void output_handle_done(void* data, struct wl_output* wl_output) {}
static void output_handle_scale(void* data, struct wl_output* wl_output, int32_t factor) {}
static void output_handle_name(void* data, struct wl_output* wl_output, const char* name) {
    WaylandContext::OutputInfo* info = static_cast<WaylandContext::OutputInfo*>(data);
    strncpy(info->name, name, sizeof(info->name) - 1);
}
static void output_handle_description(void* data, struct wl_output* wl_output, const char* description) {}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale,
    output_handle_name,
    output_handle_description
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    WaylandContext* wl = static_cast<WaylandContext*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wl->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        wl_seat_add_listener(wl->seat, &seat_listener, wl);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wl->layer_shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1));
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        wl_output* out = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 4));
        WaylandContext::OutputInfo* info = new WaylandContext::OutputInfo();
        info->output = out;
        info->x = 0; info->y = 0;
        info->width = 1920; info->height = 1080;
        memset(info->name, 0, sizeof(info->name));
        wl_output_add_listener(out, &output_listener, info);
        wl->outputs.push_back(info);
    }
}
static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

WaylandContext* g_wl = nullptr;

// --- Compositor Detection ---
static CompositorType s_compositorType = COMPOSITOR_GENERIC;
static bool s_compositorDetected = false;

CompositorType DetectCompositor() {
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        s_compositorType = COMPOSITOR_HYPRLAND;
        LAppPal::PrintLogLn("[Wayland] Detected compositor: Hyprland");
        LAppPal::PrintLogLn("[Wayland]   Global cursor tracking: enabled");
        LAppPal::PrintLogLn("[Wayland]   Cross-monitor drag: enabled");
    } else if (getenv("SWAYSOCK") || getenv("I3SOCK")) {
        s_compositorType = COMPOSITOR_SWAY;
        LAppPal::PrintLogLn("[Wayland] Detected compositor: Sway");
        LAppPal::PrintLogLn("[Wayland]   Global cursor tracking: not available (model eyes follow cursor only when over model)");
        LAppPal::PrintLogLn("[Wayland]   Cross-monitor drag: not available");
    } else {
        s_compositorType = COMPOSITOR_GENERIC;
        LAppPal::PrintLogLn("[Wayland] Detected compositor: generic wlroots-compatible");
        LAppPal::PrintLogLn("[Wayland]   Global cursor tracking: not available (model eyes follow cursor only when over model)");
        LAppPal::PrintLogLn("[Wayland]   Cross-monitor drag: not available");
    }
    s_compositorDetected = true;
    return s_compositorType;
}

CompositorType GetCompositorType() {
    if (!s_compositorDetected) {
        DetectCompositor();
    }
    return s_compositorType;
}

// --- Global Cursor Position (compositor-specific) ---
static bool GetHyprlandCursor(int& x, int& y) {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) return false;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return false;
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) {
        sprintf(addr.sun_path, "%s/hypr/%s/.socket.sock", xdg, sig);
    } else {
        sprintf(addr.sun_path, "/tmp/hypr/%s/.socket.sock", sig);
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return false; }
    const char* cmd = "-j/cursorpos";
    if (write(sock, cmd, strlen(cmd)) < 0) { close(sock); return false; }
    char buf[256] = {0};
    int n = read(sock, buf, 255);
    close(sock);
    if (n <= 0) return false;
    char* x_str = strstr(buf, "\"x\":");
    char* y_str = strstr(buf, "\"y\":");
    if (x_str && y_str) {
        x = atoi(x_str + 4);
        y = atoi(y_str + 4);
        return true;
    }
    return false;
}

bool GetGlobalCursorPosition(int& x, int& y) {
    switch (GetCompositorType()) {
    case COMPOSITOR_HYPRLAND:
        return GetHyprlandCursor(x, y);
    case COMPOSITOR_SWAY:
    case COMPOSITOR_GENERIC:
    default:
        return false;
    }
}

bool SetupWaylandContext(WaylandContext* wl, int width, int height) {
    g_wl = wl;
    wl->display = wl_display_connect(nullptr);
    if (!wl->display) {
        LAppPal::PrintLogLn("Failed to connect to Wayland display\n");
        return false;
    }

    wl->registry = wl_display_get_registry(wl->display);
    wl_registry_add_listener(wl->registry, &registry_listener, wl);
    wl_display_roundtrip(wl->display);

    if (!wl->compositor || !wl->layer_shell || !wl->shm) {
        LAppPal::PrintLogLn("Missing required Wayland protocols (layer_shell, compositor or shm)\n");
        return false;
    }

    // Set up EGL
    wl->egl_display = eglGetDisplay((EGLNativeDisplayType)wl->display);
    if (wl->egl_display == EGL_NO_DISPLAY) {
        LAppPal::PrintLogLn("Failed to get EGL display");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(wl->egl_display, &major, &minor)) {
        LAppPal::PrintLogLn("Failed to initialize EGL");
        return false;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLint num_config;
    if (!eglChooseConfig(wl->egl_display, config_attribs, &wl->egl_config, 1, &num_config) || num_config == 0) {
        LAppPal::PrintLogLn("Failed to choose EGL config");
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        LAppPal::PrintLogLn("Failed to bind OpenGL API");
        return false;
    }

    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 2,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    wl->egl_context = eglCreateContext(wl->egl_display, wl->egl_config, EGL_NO_CONTEXT, context_attribs);
    if (wl->egl_context == EGL_NO_CONTEXT) {
        LAppPal::PrintLogLn("Failed to create EGL context");
        return false;
    }

    // Create Wayland & Layer Surface
    wl->surface = wl_compositor_create_surface(wl->compositor);
    
    // Choose OVERLAY or TOP for mascot. TOP gives desktop layer. OVERLAY gives full screen layer (above panels).
    wl->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wl->layer_shell, wl->surface, nullptr, 
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "waifuland");
    
    zwlr_layer_surface_v1_add_listener(wl->layer_surface, &layer_surface_listener, wl);

    // Set interactive, anchor to all edges to get a full-screen transparent overlay
    zwlr_layer_surface_v1_set_size(wl->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(wl->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(wl->layer_surface, 0); // No keyboard focus
    wl_surface_commit(wl->surface);
    wl_display_roundtrip(wl->display); // Wait for configure event

    // Now we have the configured width/height
    wl->egl_window = wl_egl_window_create(wl->surface, wl->width, wl->height);
    wl->egl_surface = eglCreateWindowSurface(wl->egl_display, wl->egl_config, (EGLNativeWindowType)wl->egl_window, nullptr);
    if (wl->egl_surface == EGL_NO_SURFACE) {
        LAppPal::PrintLogLn("Failed to create EGL surface");
        return false;
    }

    eglMakeCurrent(wl->egl_display, wl->egl_surface, wl->egl_surface, wl->egl_context);
    eglSwapInterval(wl->egl_display, 1);

    return true;
}

void CleanWaylandContext(WaylandContext* wl) {
    if (wl->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(wl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (wl->egl_surface != EGL_NO_SURFACE) eglDestroySurface(wl->egl_display, wl->egl_surface);
        if (wl->egl_context != EGL_NO_CONTEXT) eglDestroyContext(wl->egl_display, wl->egl_context);
        eglTerminate(wl->egl_display);
    }
    if (wl->egl_window) wl_egl_window_destroy(wl->egl_window);
    if (wl->layer_surface) zwlr_layer_surface_v1_destroy(wl->layer_surface);
    if (wl->surface) wl_surface_destroy(wl->surface);
    if (wl->layer_shell) zwlr_layer_shell_v1_destroy(wl->layer_shell);
    if (wl->pointer) wl_pointer_destroy(wl->pointer);
    if (wl->seat) wl_seat_destroy(wl->seat);
    if (wl->shm) wl_shm_destroy(wl->shm);
    if (wl->compositor) wl_compositor_destroy(wl->compositor);
    if (wl->registry) wl_registry_destroy(wl->registry);
    if (wl->display) wl_display_disconnect(wl->display);
}

static void UpdateMonitorCoordinatesHyprland() {
    if (!g_wl) return;
    FILE* fp = popen("hyprctl monitors", "r");
    if (!fp) return;

    char line[512];
    char current_monitor[128] = {0};

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Monitor ", 8) == 0) {
            char* space = strchr(line + 8, ' ');
            if (space) {
                int len = space - (line + 8);
                if (len < (int)sizeof(current_monitor)) {
                    strncpy(current_monitor, line + 8, len);
                    current_monitor[len] = '\0';
                }
            }
        } else if (strstr(line, " at ")) {
            char* at_pos = strstr(line, " at ");
            if (at_pos) {
                int x = 0, y = 0;
                if (sscanf(at_pos + 4, "%dx%d", &x, &y) == 2) {
                    for (size_t i = 0; i < g_wl->outputs.size(); i++) {
                        WaylandContext::OutputInfo* out = g_wl->outputs[i];
                        if (strcmp(out->name, current_monitor) == 0) {
                            out->x = x;
                            out->y = y;
                        }
                    }
                }
            }
        }
    }
    pclose(fp);
}

void UpdateMonitorCoordinates() {
    if (!g_wl) return;
    switch (GetCompositorType()) {
    case COMPOSITOR_HYPRLAND:
        UpdateMonitorCoordinatesHyprland();
        break;
    case COMPOSITOR_SWAY:
    case COMPOSITOR_GENERIC:
    default:
        // On non-Hyprland compositors, output coordinates are already
        // populated by Wayland's wl_output geometry events — no extra work needed.
        break;
    }
}

void SwitchWaylandOutputToMonitor(int hx, int hy) {
    if (!g_wl || g_wl->outputs.empty()) return;
    UpdateMonitorCoordinates();
    
    int target_idx = g_wl->current_output_index;
    for (int i = 0; i < g_wl->outputs.size(); i++) {
        WaylandContext::OutputInfo* out = g_wl->outputs[i];
        LAppPal::PrintLogLn("[Wayland] Checking output %d (x:%d, y:%d, w:%d, h:%d) against hx:%d, hy:%d", i, out->x, out->y, out->width, out->height, hx, hy);
        if (hx >= out->x && hx < out->x + out->width && hy >= out->y && hy < out->y + out->height) {
            target_idx = i;
            break;
        }
    }
    
    if (target_idx == g_wl->current_output_index) {
        LAppPal::PrintLogLn("[Wayland] Target output same as current (%d), skipping switch", target_idx);
        return;
    }
    LAppPal::PrintLogLn("[Wayland] Switching output from %d to %d", g_wl->current_output_index, target_idx);
    g_wl->current_output_index = target_idx;
    
    if (g_wl->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_wl->egl_display, g_wl->egl_surface);
        g_wl->egl_surface = EGL_NO_SURFACE;
    }
    if (g_wl->egl_window) {
        wl_egl_window_destroy(g_wl->egl_window);
        g_wl->egl_window = nullptr;
    }
    if (g_wl->layer_surface) {
        zwlr_layer_surface_v1_destroy(g_wl->layer_surface);
        g_wl->layer_surface = nullptr;
    }
    if (g_wl->surface) {
        wl_surface_destroy(g_wl->surface);
        g_wl->surface = nullptr;
    }
    
    // Switch to new output
    g_wl->surface = wl_compositor_create_surface(g_wl->compositor);
    wl_output* output = g_wl->outputs[g_wl->current_output_index]->output;
    
    g_wl->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        g_wl->layer_shell, g_wl->surface, output, 
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "waifuland");
    
    zwlr_layer_surface_v1_add_listener(g_wl->layer_surface, &layer_surface_listener, g_wl);
    zwlr_layer_surface_v1_set_size(g_wl->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(g_wl->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(g_wl->layer_surface, 0);
    
    wl_surface_commit(g_wl->surface);
    wl_display_roundtrip(g_wl->display);
    
    g_wl->egl_window = wl_egl_window_create(g_wl->surface, g_wl->width, g_wl->height);
    g_wl->egl_surface = eglCreateWindowSurface(g_wl->egl_display, g_wl->egl_config, (EGLNativeWindowType)g_wl->egl_window, nullptr);
    eglMakeCurrent(g_wl->egl_display, g_wl->egl_surface, g_wl->egl_surface, g_wl->egl_context);
    eglSwapInterval(g_wl->egl_display, 1);
}
