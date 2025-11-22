#include "MouseHook.h"
#include "GestureRecognizer.h"
#include "WindowsActions.h"
#include "ConfigManager.h"
#include "MainWindow.h"
#include "TrayIcon.h"
#include <windows.h>

#ifdef _UNICODE
#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#endif

using namespace WinMouseFix;

// 全局变量
MouseHook* g_mouseHook = nullptr;
TrayIcon* g_trayIcon = nullptr;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 检查是否已有实例在运行
    HANDLE hMutex = CreateMutex(nullptr, TRUE, L"WinMouseFix_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBox(
            nullptr,
            L"Win Mouse Fix 已经在运行中！\n请检查系统托盘图标。",
            L"提示",
            MB_OK | MB_ICONINFORMATION
        );
        if (hMutex) {
            CloseHandle(hMutex);
        }
        return 0;
    }

    // 设置工作目录
    char path[MAX_PATH] = { 0 }; // 获取当前进程模块完整路径（含exe文件名）
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len != 0) {
        std::string fullPath(path);
        size_t pos = fullPath.find_last_of("\\/");
        if (pos != std::string::npos) {
            std::string dir = fullPath.substr(0, pos);
            if (!dir.empty()) {
                SetCurrentDirectoryA(dir.c_str());
            }
        }
    }
    
    // 创建核心组件
    WindowsActions actions;
    GestureRecognizer gestureRecognizer(&actions);
    MouseHook mouseHook;
    g_mouseHook = &mouseHook;
    
    ConfigManager configManager;
    std::string configPath = "config.json";
    
    if (!configManager.LoadFromFile(configPath)) {
        configManager.CreateDefaultConfig();
        configManager.SaveToFile(configPath);
    }
    
    gestureRecognizer.LoadConfig(configManager.GetGestureConfigs());
    mouseHook.SetGestureRecognizer(&gestureRecognizer);
    
    // 创建主窗口
    MainWindow mainWindow;
    if (!mainWindow.Create(hInstance)) {
        return 1;
    }
    
    mainWindow.SetConfigManager(&configManager);
    mainWindow.SetMouseHook(&mouseHook);
    
    // 创建托盘图标
    TrayIcon trayIcon;
    g_trayIcon = &trayIcon;
    trayIcon.Create(mainWindow.GetHWND(), hInstance);
    trayIcon.SetMainWindow(&mainWindow);
    mainWindow.SetTrayIcon(&trayIcon);
    
    // 安装钩子
    if (!mouseHook.Install()) {
        MessageBox(nullptr, L"Failed to install mouse hook!", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // 显示主窗口
    mainWindow.Show();
    
    // 消息循环
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // 清理
    mouseHook.Uninstall();
    trayIcon.Remove();
    
    // 释放互斥锁
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return (int)msg.wParam;
}

