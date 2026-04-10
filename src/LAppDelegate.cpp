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
#include <GLFW/glfw3.h>
#include "LAppView.hpp"
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppTextureManager.hpp"

using namespace Csm;
using namespace std;
using namespace LAppDefine;

namespace {
    LAppDelegate* s_instance = NULL;
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

    // GLFWの初期化
    if (glfwInit() == GL_FALSE)
    {
        if (DebugLogEnable)
        {
            LAppPal::PrintLogLn("Can't initilize GLFW");
        }
        return GL_FALSE;
    }

    // Set Transparent, Borderless Window Hints for Wayland Desktop Pet
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);

    // Windowの生成_
    _window = glfwCreateWindow(RenderTargetWidth, RenderTargetHeight, "waifuland", NULL, NULL);
    if (_window == NULL)
    {
        if (DebugLogEnable)
        {
            LAppPal::PrintLogLn("Can't create GLFW window.");
        }
        glfwTerminate();
        return GL_FALSE;
    }

    // Windowのコンテキストをカレントに設定
    glfwMakeContextCurrent(_window);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        if (DebugLogEnable)
        {
            LAppPal::PrintLogLn("Can't initilize glew. Error: %s", glewGetErrorString(err));
        }
        glfwTerminate();
        return GL_FALSE;
    }

    //テクスチャサンプリング設定
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    //透過設定
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    //コールバック関数の登録
    glfwSetMouseButtonCallback(_window, EventHandler::OnMouseCallBack);
    glfwSetCursorPosCallback(_window, EventHandler::OnMouseCallBack);
    glfwSetScrollCallback(_window, EventHandler::OnScrollCallBack);

    // ウィンドウサイズ記憶
    int width, height;
    glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &width, &height);
    _windowWidth = width;
    _windowHeight = height;
    glViewport(0, 0, _windowWidth, _windowHeight);

    // Cubism3の初期化
    InitializeCubism();

    SetExecuteAbsolutePath();

    //load model
    LAppLive2DManager::GetInstance();

    //AppViewの初期化
    _view->Initialize(width, height);
    _view->InitializeSprite();

    return GL_TRUE;
}

void LAppDelegate::Release()
{
    // Windowの削除
    glfwDestroyWindow(_window);

    glfwTerminate();

    delete _textureManager;
    delete _view;

    // リソースを解放
    LAppLive2DManager::ReleaseInstance();

    //Cubism3の解放
    CubismFramework::Dispose();
}

void LAppDelegate::Run()
{
    //メインループ
    while (glfwWindowShouldClose(_window) == GL_FALSE && !_isEnd)
    {
        int width, height;
        glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &width, &height);
        if((_windowWidth!=width || _windowHeight!=height) && width>0 && height>0)
        {
            _view->Initialize(width, height);
            _view->ResizeSprite();
            // レンダーターゲットを破棄する（次フレームで新サイズで再作成される）
            _view->DestroySpriteRenderTarget();
            // モデルのレンダーターゲットのサイズ変更
            LAppLive2DManager::GetInstance()->SetRenderTargetSize(width, height);
            _windowWidth = width;
            _windowHeight = height;
        }
        glViewport(0, 0, _windowWidth, _windowHeight);

        // 時間更新
        LAppPal::UpdateTime();

        // 画面の初期化
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearDepth(1.0);

        //描画更新
        _view->Render();

        // バッファの入れ替え
        glfwSwapBuffers(_window);

        // Poll for and process events
        glfwPollEvents();
    }

    Release();

    LAppDelegate::ReleaseInstance();
}

LAppDelegate::LAppDelegate():
    _cubismOption(),
    _window(NULL),
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
    _lookCenterY(0.5f)
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

    LAppPal::UpdateTime();
}

void LAppDelegate::OnMouseCallBack(GLFWwindow* window, int button, int action, int modify)
{
    if (_view == NULL)
    {
        return;
    }
    
    if (button == GLFW_MOUSE_BUTTON_LEFT)
    {
        if (GLFW_PRESS == action)
        {
            _captured = true;
            _view->OnTouchesBegan(_mouseX, _mouseY);

            // Start drag
            _isDraggingWindow = true;
            double curX, curY;
            glfwGetCursorPos(window, &curX, &curY);
            _dragStartX = static_cast<int>(curX);
            _dragStartY = static_cast<int>(curY);
            glfwGetWindowPos(window, &_windowStartX, &_windowStartY);

        }
        else if (GLFW_RELEASE == action)
        {
            if (_captured)
            {
                _captured = false;
                _isDraggingWindow = false;

                double curX, curY;
                glfwGetCursorPos(window, &curX, &curY);
                if (abs(static_cast<int>(curX) - _dragStartX) < 10 && abs(static_cast<int>(curY) - _dragStartY) < 10)
                {
                    _view->OnTouchesEnded(_mouseX, _mouseY); // Trigger Tap
                }
                else 
                {
                    LAppLive2DManager::GetInstance()->OnDrag(0.0f, 0.0f); // End look
                }
            }
        }
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_RELEASE)
    {
        // Switch Models
        LAppLive2DManager::GetInstance()->NextScene();
    }
    else if (button == GLFW_MOUSE_BUTTON_MIDDLE && action == GLFW_PRESS)
    {
        // Set Look Center!
        int width, height;
        glfwGetWindowSize(window, &width, &height);
        _lookCenterX = _mouseX / (float)width;
        _lookCenterY = _mouseY / (float)height;
    }
}

void LAppDelegate::OnMouseCallBack(GLFWwindow* window, double x, double y)
{
    _mouseX = static_cast<float>(x);
    _mouseY = static_cast<float>(y);

    if (_view == NULL)
    {
        return;
    }

    // Calculate viewX / viewY based on look center
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    
    float viewX = (_mouseX / (float)width) - _lookCenterX;
    float viewY = (_mouseY / (float)height) - _lookCenterY;
    viewX *= 2.0f; // Scale to -1 to 1 roughly
    viewY *= -2.0f; // Flip Y

    LAppLive2DManager::GetInstance()->OnDrag(viewX, viewY);

    if (_captured && _isDraggingWindow)
    {
        int currentX, currentY;
        glfwGetWindowPos(window, &currentX, &currentY);
        
        int globalMouseX = currentX + static_cast<int>(x);
        int globalMouseY = currentY + static_cast<int>(y);
        
        int globalDragStartX = _windowStartX + _dragStartX;
        int globalDragStartY = _windowStartY + _dragStartY;
        
        int deltaX = globalMouseX - globalDragStartX;
        int deltaY = globalMouseY - globalDragStartY;
        
        if (deltaX != 0 || deltaY != 0) {
            glfwSetWindowPos(window, _windowStartX + deltaX, _windowStartY + deltaY);
        }
    }
}

void LAppDelegate::OnScrollCallBack(GLFWwindow* window, double xoffset, double yoffset)
{
    int width, height;
    glfwGetWindowSize(window, &width, &height);

    float scale = 1.0f + (yoffset * 0.1f);
    int newWidth = static_cast<int>(width * scale);
    int newHeight = static_cast<int>(height * scale);

    // Limit size
    if (newWidth > 300 && newHeight > 300 && newWidth < 4000)
    {
        glfwSetWindowSize(window, newWidth, newHeight);
    }
}


void LAppDelegate::GetClientSize(int& rWidth, int& rHeight)
{
    glfwGetWindowSize(LAppDelegate::GetInstance()->GetWindow(), &rWidth, &rHeight);
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
