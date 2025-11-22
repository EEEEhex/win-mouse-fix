#include "MainWindow.h"
#include "ConfigManager.h"
#include "MouseHook.h"
#include "TrayIcon.h"
#include <windowsx.h>
#include <sstream>

#include "resource.h"

namespace WinMouseFix {

MainWindow::MainWindow()
    : hwnd_(nullptr)
    , hInstance_(nullptr)
    , hFont_(nullptr)
    , statusLabel_(nullptr)
    , configListBox_(nullptr)
    , enableCheckBox_(nullptr)
    , autoStartCheckBox_(nullptr)
    , editButton_(nullptr)
    , aboutButton_(nullptr)
    , configManager_(nullptr)
    , mouseHook_(nullptr)
    , trayIcon_(nullptr) {
}

MainWindow::~MainWindow() {
    if (hFont_) {
        DeleteObject(hFont_);
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

bool MainWindow::Create(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    // 注册窗口类
    const wchar_t CLASS_NAME[] = L"WinMouseFixWindow";
    
    // 先尝试取消注册（如果已存在）
    UnregisterClass(CLASS_NAME, hInstance);
    
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = 0;
    wc.lpfnWndProc = WindowProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = CLASS_NAME;
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));

    ATOM atom = RegisterClassEx(&wc);
    if (!atom) {
        DWORD error = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"Failed to register window class! Error: %lu", error);
        MessageBox(nullptr, buf, L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // 计算居中位置
    int windowWidth = 500;
    int windowHeight = 400;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenWidth - windowWidth) / 2;
    int y = (screenHeight - windowHeight) / 2;

    // 创建窗口（禁用最大化和调整大小）
    hwnd_ = CreateWindowEx(
        0,                      // Optional window styles
        CLASS_NAME,             // Window class
        L"Win Mouse Fix",       // Window text
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,    // Window style (无最大化按钮和调整大小)
        x, y, windowWidth, windowHeight,
        nullptr,                // Parent window    
        nullptr,                // Menu
        hInstance,              // Instance handle
        this                    // Additional application data
    );

    if (!hwnd_) {
        DWORD error = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"Failed to create window! Error: %lu\nAtom: %u", error, atom);
        MessageBox(nullptr, buf, L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    CreateControls();
    return true;
}

void MainWindow::CreateControls() {
    // 创建微软雅黑字体
    hFont_ = CreateFont(
        16,                        // 字体高度
        0,                         // 字体宽度（0表示自动）
        0,                         // 文本倾斜角度
        0,                         // 字体倾斜角度
        FW_NORMAL,                 // 字体粗细
        FALSE,                     // 斜体
        FALSE,                     // 下划线
        FALSE,                     // 删除线
        DEFAULT_CHARSET,           // 字符集
        OUT_DEFAULT_PRECIS,        // 输出精度
        CLIP_DEFAULT_PRECIS,       // 裁剪精度
        CLEARTYPE_QUALITY,         // 输出质量（使用ClearType）
        DEFAULT_PITCH | FF_DONTCARE, // 字体间距和族
        L"Microsoft YaHei"         // 字体名称
    );

    // 状态标签
    statusLabel_ = CreateWindowEx(
        0, L"STATIC",
        L"Win Mouse Fix - 鼠标手势增强工具",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, 10, 460, 30,
        hwnd_, (HMENU)NULL, hInstance_, nullptr
    );
    if (statusLabel_) {
        SendMessage(statusLabel_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    }

    // 启用复选框
    enableCheckBox_ = CreateWindowEx(
        0, L"BUTTON",
        L"启用手势识别",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        10, 50, 220, 25,
        hwnd_, (HMENU)(UINT_PTR)ID_ENABLE_CHECKBOX, hInstance_, nullptr
    );
    if (enableCheckBox_) {
        SendMessage(enableCheckBox_, WM_SETFONT, (WPARAM)hFont_, TRUE);
        Button_SetCheck(enableCheckBox_, BST_CHECKED);
    }

    // 开机自启复选框
    autoStartCheckBox_ = CreateWindowEx(
        0, L"BUTTON",
        L"开机自启",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        250, 50, 220, 25,
        hwnd_, (HMENU)(UINT_PTR)ID_AUTOSTART_CHECKBOX, hInstance_, nullptr
    );
    if (autoStartCheckBox_) {
        SendMessage(autoStartCheckBox_, WM_SETFONT, (WPARAM)hFont_, TRUE);
        // 检查当前是否已设置开机自启
        Button_SetCheck(autoStartCheckBox_, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    }

    // 配置列表标签
    HWND hLabel = CreateWindowEx(
        0, L"STATIC",
        L"当前配置:",
        WS_CHILD | WS_VISIBLE,
        10, 85, 460, 20,
        hwnd_, (HMENU)NULL, hInstance_, nullptr
    );
    if (hLabel) {
        SendMessage(hLabel, WM_SETFONT, (WPARAM)hFont_, TRUE);
    }

    configListBox_ = CreateWindowEx(
        WS_EX_CLIENTEDGE, L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
        10, 110, 460, 180,
        hwnd_, (HMENU)(UINT_PTR)ID_CONFIG_LISTBOX, hInstance_, nullptr
    );
    if (configListBox_) {
        SendMessage(configListBox_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    }

    // 编辑按钮
    editButton_ = CreateWindowEx(
        0, L"BUTTON",
        L"编辑配置文件",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        10, 300, 120, 30,
        hwnd_, (HMENU)(UINT_PTR)ID_EDIT_BUTTON, hInstance_, nullptr
    );
    if (editButton_) {
        SendMessage(editButton_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    }

    // 关于按钮
    aboutButton_ = CreateWindowEx(
        0, L"BUTTON",
        L"关于",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        350, 300, 120, 30,
        hwnd_, (HMENU)(UINT_PTR)ID_ABOUT_BUTTON, hInstance_, nullptr
    );
    if (aboutButton_) {
        SendMessage(aboutButton_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    }

    LoadConfigToUI();
}

void MainWindow::LoadConfigToUI() {
    if (!configManager_ || !configListBox_) return;

    // 清空列表
    SendMessage(configListBox_, LB_RESETCONTENT, 0, 0);

    // 添加配置项
    const auto& configs = configManager_->GetGestureConfigs();
    for (const auto& config : configs) {
        std::wstring buttonStr = L"按钮";
        if (config.triggerButton == MouseButton::BUTTON_4) {
            buttonStr += L"4";
        } else if (config.triggerButton == MouseButton::BUTTON_5) {
            buttonStr += L"5";
        }

        std::wstring gestureStr;
        switch (config.gestureType) {
            case GestureType::SWIPE_UP: gestureStr = L"向上滑动"; break;
            case GestureType::SWIPE_DOWN: gestureStr = L"向下滑动"; break;
            case GestureType::SWIPE_LEFT: gestureStr = L"向左滑动"; break;
            case GestureType::SWIPE_RIGHT: gestureStr = L"向右滑动"; break;
            case GestureType::TWO_FINGER_SCROLL: gestureStr = L"移动"; break;
            default: gestureStr = L"未知"; break;
        }

        std::wstring actionStr;
        switch (config.actionType) {
            case ActionType::TASK_VIEW: actionStr = L"任务视图"; break;
            case ActionType::SHOW_DESKTOP: actionStr = L"显示桌面"; break;
            case ActionType::SWITCH_DESKTOP_LEFT: actionStr = L"切换到左边桌面"; break;
            case ActionType::SWITCH_DESKTOP_RIGHT: actionStr = L"切换到右边桌面"; break;
            case ActionType::SCROLL_SIMULATION: actionStr = L"滚动模拟"; break;
            default: actionStr = L"未知"; break;
        }

        std::wstring item = buttonStr + L" + " + gestureStr + L" = " + actionStr;
        SendMessage(configListBox_, LB_ADDSTRING, 0, (LPARAM)item.c_str());
    }
}

void MainWindow::Show() {
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    
    // 显示后刷新配置列表
    LoadConfigToUI();
}

void MainWindow::RefreshConfigList() {
    LoadConfigToUI();
}

void MainWindow::Hide() {
    ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    MainWindow* pThis = nullptr;

    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        pThis = (MainWindow*)pCreate->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
        pThis->hwnd_ = hwnd;
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    } else {
        pThis = (MainWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    if (pThis) {
        return pThis->HandleMessage(uMsg, wParam, lParam);
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            return 0;

        case WM_COMMAND:
            OnCommand(wParam);
            return 0;
        
        case WM_TRAYICON:
            // 转发托盘消息到托盘图标处理
            if (trayIcon_) {
                trayIcon_->HandleTrayMessage(wParam, lParam);
            }
            return 0;

        case WM_CLOSE:
            OnClose();
            return 0;

        case WM_DESTROY:
            OnDestroy();
            return 0;

        default:
            return DefWindowProc(hwnd_, uMsg, wParam, lParam);
    }
}

void MainWindow::OnCommand(WPARAM wParam) {
    switch (LOWORD(wParam)) {
        case ID_ENABLE_CHECKBOX: {
            BOOL checked = Button_GetCheck(enableCheckBox_) == BST_CHECKED;
            if (mouseHook_) {
                if (checked) {
                    mouseHook_->Install();
                } else {
                    mouseHook_->Uninstall();
                }
            }
            break;
        }

        case ID_AUTOSTART_CHECKBOX: {
            BOOL checked = Button_GetCheck(autoStartCheckBox_) == BST_CHECKED;
            SetAutoStart(checked == BST_CHECKED);
            break;
        }

        case ID_EDIT_BUTTON: {
            // 打开配置文件
            ShellExecute(hwnd_, L"open", L"config.json", nullptr, nullptr, SW_SHOW);
            break;
        }

        case ID_ABOUT_BUTTON: {
            MessageBox(
                hwnd_,
                L"Win Mouse Fix v1.0.1\n\n"
                L"Windows 鼠标手势增强工具\n"
                L"模仿 Mac Mouse Fix 功能\n\n"
                L"使用方法:\n"
                L"• 按住侧键4并移动鼠标执行手势\n"
                L"• 按住侧键5并移动鼠标进行滚动\n\n"
                L"GitHub:https://github.com/EEEEhex/win-mouse-fix",
                L"关于 Win Mouse Fix",
                MB_OK | MB_ICONINFORMATION
            );
            break;
        }
    }
}

void MainWindow::OnClose() {
    // 最小化到托盘而不是关闭
    Hide();
}

void MainWindow::OnDestroy() {
    PostQuitMessage(0);
}

bool MainWindow::IsAutoStartEnabled() {
    HKEY hKey;
    LONG result = RegOpenKeyEx(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        KEY_READ,
        &hKey
    );
    
    if (result != ERROR_SUCCESS) {
        return false;
    }
    
    wchar_t value[MAX_PATH];
    DWORD valueSize = sizeof(value);
    result = RegQueryValueEx(
        hKey,
        L"WinMouseFix",
        nullptr,
        nullptr,
        (LPBYTE)value,
        &valueSize
    );
    
    RegCloseKey(hKey);
    return result == ERROR_SUCCESS;
}

void MainWindow::SetAutoStart(bool enable) {
    HKEY hKey;
    LONG result = RegOpenKeyEx(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        KEY_WRITE,
        &hKey
    );
    
    if (result != ERROR_SUCCESS) {
        MessageBox(
            hwnd_,
            L"无法打开注册表项",
            L"错误",
            MB_OK | MB_ICONERROR
        );
        return;
    }
    
    if (enable) {
        // 获取程序路径
        wchar_t path[MAX_PATH];
        GetModuleFileName(nullptr, path, MAX_PATH);
        
        // 添加注册表项
        result = RegSetValueEx(
            hKey,
            L"WinMouseFix",
            0,
            REG_SZ,
            (LPBYTE)path,
            (wcslen(path) + 1) * sizeof(wchar_t)
        );
        
        if (result == ERROR_SUCCESS) {
            MessageBox(
                hwnd_,
                L"已设置开机自启",
                L"成功",
                MB_OK | MB_ICONINFORMATION
            );
        } else {
            MessageBox(
                hwnd_,
                L"设置开机自启失败",
                L"错误",
                MB_OK | MB_ICONERROR
            );
        }
    } else {
        // 删除注册表项
        result = RegDeleteValue(hKey, L"WinMouseFix");
        
        if (result == ERROR_SUCCESS) {
            MessageBox(
                hwnd_,
                L"已取消开机自启",
                L"成功",
                MB_OK | MB_ICONINFORMATION
            );
        } else if (result == ERROR_FILE_NOT_FOUND) {
            // 本来就没有设置，不算错误
        } else {
            MessageBox(
                hwnd_,
                L"取消开机自启失败",
                L"错误",
                MB_OK | MB_ICONERROR
            );
        }
    }
    
    RegCloseKey(hKey);
}

} // namespace WinMouseFix

