#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
#include <cmath>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <libgen.h>
#include <GL/glew.h>

#include "LAppView.hpp"
#include "LAppPal.hpp"
#include "LAppWaylandRegion.hpp"
#include "LAppDefine.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppTextureManager.hpp"

using namespace Csm;
using namespace std;
using namespace LAppDefine;

namespace {
    LAppDelegate* s_instance = NULL;
}

static void HandleToggleSignal(int) {
    if (s_instance) {
        s_instance->ToggleHidden();
    }
}

bool GetHyprlandCursor(int& x, int& y) {
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) return false;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if(sock < 0) return false;
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg) {
        sprintf(addr.sun_path, "%s/hypr/%s/.socket.sock", xdg, sig);
    } else {
        sprintf(addr.sun_path, "/tmp/hypr/%s/.socket.sock", sig);
    }
    if(connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return false; }
    const char* cmd = "-j/cursorpos";
    if(write(sock, cmd, strlen(cmd)) < 0) { close(sock); return false; }
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

LAppDelegate* LAppDelegate::GetInstance()
{
    if (s_instance == NULL)
    {
        s_instance = new LAppDelegate();
    }

    return s_instance;
}

void LAppDelegate::ReleaseInstance()
{
    if (s_instance != NULL)
    {
        delete s_instance;
    }

    s_instance = NULL;
}

bool LAppDelegate::Initialize()
{
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("START");
    }

    signal(SIGUSR1, HandleToggleSignal);

    _windowWidth = RenderTargetWidth;
    _windowHeight = RenderTargetHeight;

    if (!SetupWaylandContext(&_wlContext, RenderTargetWidth, RenderTargetHeight)) {
        return false;
    }

    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        if (DebugLogEnable)
        {
            LAppPal::PrintLogLn("Can't initilize glew. Error: %s", glewGetErrorString(err));
        }
        return false;
    }

    //テクスチャサンプリング設定
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    //透過設定
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, _windowWidth, _windowHeight);

    // Cubism3の初期化
    InitializeCubism();
    SetExecuteAbsolutePath();
    LAppLive2DManager::GetInstance();

    _view->Initialize(_windowWidth, _windowHeight);
    _view->InitializeSprite();

    return true;
}
void LAppDelegate::Release()
{
    CleanWaylandContext(&_wlContext);

    delete _textureManager;
    delete _view;

    LAppLive2DManager::ReleaseInstance();
    CubismFramework::Dispose();
}
void LAppDelegate::Run()
{
    while (!_isEnd) 
    {
        if (wl_display_dispatch_pending(_wlContext.display) == -1) {
            break;
        }
        wl_display_flush(_wlContext.display);
        
        int width = _wlContext.width;
        int height = _wlContext.height;

        if((_windowWidth!=width || _windowHeight!=height) && width>0 && height>0) {
            _view->Initialize(width, height);
            _view->ResizeSprite();
            _view->DestroySpriteRenderTarget();
            LAppLive2DManager::GetInstance()->SetRenderTargetSize(width, height);
            _windowWidth = width;
            _windowHeight = height;
        }

        glViewport(0, 0, _windowWidth, _windowHeight);

        
        int hx, hy;
        if (GetHyprlandCursor(hx, hy) && !_wlContext.outputs.empty()) {
            int current_idx = _wlContext.current_output_index;
            WaylandContext::OutputInfo* out = _wlContext.outputs[current_idx];
            
            int local_x = hx - out->x;
            int local_y = hy - out->y;
            OnMouseCallBack(nullptr, (double)local_x, (double)local_y);
        }

        LAppPal::UpdateTime();

        // 画面の初期化 -> Transparent!
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearDepth(1.0);

        if (!_isHidden) {
            _view->Render();
        }

        UpdateWaylandInputRegion(&_wlContext);
        eglSwapBuffers(_wlContext.egl_display, _wlContext.egl_surface);
        
        if (_isHidden) {
            usleep(33000); // Reduce CPU usage when hidden
        }
    }
    Release();
    LAppDelegate::ReleaseInstance();
}
LAppDelegate::LAppDelegate():
    _cubismOption(),
    
    _captured(false),
    _mouseX(0.0f),
    _mouseY(0.0f),
    _isEnd(false),
    _windowWidth(0),
    _windowHeight(0),
    _isDraggingWindow(false),
    _dragStartX(0),
    _dragStartY(0),
    _windowStartX(0),
    _windowStartY(0),
    _lookCenterX(0.5f),
    _lookCenterY(0.5f),
    _modelScale(1.0f),
    _modelX(0.0f),
    _modelY(0.0f)
{
    _executeAbsolutePath = "";
    _view = new LAppView();
    _textureManager = new LAppTextureManager();
}

LAppDelegate::~LAppDelegate()
{

}

void LAppDelegate::InitializeCubism()
{
    //setup cubism
    _cubismOption.LogFunction = LAppPal::PrintMessage;
    _cubismOption.LoggingLevel = LAppDefine::CubismLoggingLevel;
    _cubismOption.LoadFileFunction = LAppPal::LoadFileAsBytes;
    _cubismOption.ReleaseBytesFunction = LAppPal::ReleaseBytes;
    Csm::CubismFramework::StartUp(&_cubismAllocator, &_cubismOption);

    //Initialize cubism
    CubismFramework::Initialize();

    //default proj
    CubismMatrix44 projection;

    
        int hx, hy;
        if (GetHyprlandCursor(hx, hy) && !_wlContext.outputs.empty()) {
            int current_idx = _wlContext.current_output_index;
            WaylandContext::OutputInfo* out = _wlContext.outputs[current_idx];
            
            int local_x = hx - out->x;
            int local_y = hy - out->y;
            OnMouseCallBack(nullptr, (double)local_x, (double)local_y);
        }

        LAppPal::UpdateTime();
}

void LAppDelegate::OnMouseCallBack(void* window, int button, int action, int modify)
{
    if (_view == NULL)
    {
        return;
    }
    
    if (button == 0)
    {
        if (1 == action)
        {
            _captured = true;
            _view->OnTouchesBegan(_mouseX, _mouseY);

            // Start drag
            _isDraggingWindow = true;
            double curX, curY;
            curX = _mouseX; curY = _mouseY;
            _dragStartX = static_cast<int>(curX);
            _dragStartY = static_cast<int>(curY);
            _windowStartX = static_cast<int>(curX); _windowStartY = static_cast<int>(curY);

        }
        else if (0 == action)
        {
            if (_captured)
            {
                _captured = false;

                // Execute screen switch on release
                int hx, hy;
                bool is_drag = _isDraggingWindow;
                bool got_cursor = GetHyprlandCursor(hx, hy);
                bool has_outputs = !_wlContext.outputs.empty();
                LAppPal::PrintLogLn("[Debug] Release: is_drag=%d, got_cursor=%d, has_outputs=%d, hx=%d, hy=%d", 
                    (int)is_drag, (int)got_cursor, (int)has_outputs, hx, hy);
                
                if (is_drag && got_cursor && has_outputs) {
                    int old_idx = _wlContext.current_output_index;
                    extern void SwitchWaylandOutputToMonitor(int, int);
                    SwitchWaylandOutputToMonitor(hx, hy);
                    int new_idx = _wlContext.current_output_index;
                    
                    if (old_idx != new_idx) {
                        WaylandContext::OutputInfo* out = _wlContext.outputs[old_idx];
                        WaylandContext::OutputInfo* new_out = _wlContext.outputs[new_idx];
                        
                        // Keep physical height identical across different resolution monitors
                        _modelScale *= (float)out->height / (float)new_out->height;

                        float local_x = hx - new_out->x;
                        float local_y = hy - new_out->y;
                        
                        _modelX = (local_x - new_out->width * 0.5f) / (new_out->height * 0.5f) / _modelScale;
                        _modelY = -(local_y - new_out->height * 0.5f) / (new_out->height * 0.5f) / _modelScale;
                    }
                }

                _isDraggingWindow = false;

                double curX, curY;
                curX = _mouseX; curY = _mouseY;
                int dx = abs(static_cast<int>(curX) - _windowStartX);
                int dy = abs(static_cast<int>(curY) - _windowStartY);
                LAppPal::PrintLogLn("[Debug] Tap check: cur(%d,%d) start(%d,%d) dx=%d dy=%d", 
                    static_cast<int>(curX), static_cast<int>(curY), _windowStartX, _windowStartY, dx, dy);
                if (dx < 10 && dy < 10)
                {
                    LAppPal::PrintLogLn("[Event] Model Tapped: Cursor (%d, %d)", static_cast<int>(curX), static_cast<int>(curY));
                      _view->OnTouchesEnded(_mouseX, _mouseY); // Trigger Tap
                }
                else 
                {
                    LAppLive2DManager::GetInstance()->OnDrag(0.0f, 0.0f); // End look
                }
            }
        }
    }
    else if (button == 1 && action == 0)
    {
        // Switch Models
        LAppPal::PrintLogLn("[Event] Switch Model Triggered");
          LAppPal::PrintLogLn("[Event] Switch Model Triggered");
          LAppLive2DManager::GetInstance()->NextScene();
    }
    else if (button == 2 && action == 1)
    {
        // Switch Skin
        LAppPal::PrintLogLn("[Event] Switch Skin Triggered");
        LAppLive2DManager::GetInstance()->SwitchSkin();
    }
}

void LAppDelegate::OnMouseCallBack(void* window, double x, double y)
{
    _mouseX = static_cast<float>(x);
    _mouseY = static_cast<float>(y);

    if (_view == NULL)
    {
        return;
    }

    // Calculate viewX / viewY based on look center
    int width, height;
    width = _windowWidth; height = _windowHeight;
    
    float faceCenterX = ((float)width * _lookCenterX) + (_modelX * _modelScale * ((float)height / 2.0f));
    float faceCenterY = ((float)height * _lookCenterY) - (_modelY * _modelScale * ((float)height / 2.0f));

    float viewX = (_mouseX - faceCenterX) / ((float)width / 2.0f);
    float viewY = -(_mouseY - faceCenterY) / ((float)height / 2.0f);

    LAppLive2DManager::GetInstance()->OnDrag(viewX, viewY);

    if (_captured && _isDraggingWindow)
    {
        double curX = x;
        double curY = y;
        int deltaX = static_cast<int>(curX) - _dragStartX;
        int deltaY = static_cast<int>(curY) - _dragStartY;
        
        if (deltaX != 0 || deltaY != 0) {
            float dx_logical = (float)deltaX / (float)_windowHeight * 2.0f;
            float dy_logical = -(float)deltaY / (float)_windowHeight * 2.0f; // Y axis is flipped in OpenGL
            
            _modelX += dx_logical / _modelScale;
            _modelY += dy_logical / _modelScale;
            
            _dragStartX = static_cast<int>(curX);
            _dragStartY = static_cast<int>(curY);
        }
    }
}

void LAppDelegate::OnScrollCallBack(void* window, double xoffset, double yoffset)
{
    float scale = 1.0f + (yoffset * 0.1f);
    _modelScale *= scale;
    if (_modelScale < 0.1f) _modelScale = 0.1f;
    if (_modelScale > 10.0f) _modelScale = 10.0f;
}

void LAppDelegate::GetClientSize(int& rWidth, int& rHeight)
{
    rWidth = GetInstance()->_windowWidth;
    rHeight = GetInstance()->_windowHeight;
}

void LAppDelegate::SetExecuteAbsolutePath()
{
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, 1024 - 1);
    if (len != -1)
    {
        path[len] = '\0';
    }
    this->_executeAbsolutePath = dirname(path);
    this->_executeAbsolutePath += "/";
}
