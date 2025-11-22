#include "TrayIcon.h"
#include "MainWindow.h"

#include "resource.h"

namespace WinMouseFix {

TrayIcon::TrayIcon()
    : hwnd_(nullptr)
    , hIcon_(nullptr)
    , mainWindow_(nullptr) {
    ZeroMemory(&nid_, sizeof(NOTIFYICONDATA));
}

TrayIcon::~TrayIcon() {
    Remove();
}

bool TrayIcon::Create(HWND hwnd, HINSTANCE hInstance) {
    hwnd_ = hwnd;

    // 创建默认图标
    hIcon_ = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    if (!hIcon_) {
        hIcon_ = CreateDefaultIcon(hInstance);
    }

    // 初始化托盘图标数据
    nid_.cbSize = sizeof(NOTIFYICONDATA);
    nid_.hWnd = hwnd;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = hIcon_;
    wcscpy_s(nid_.szTip, L"Win Mouse Fix - 运行中");

    return Shell_NotifyIcon(NIM_ADD, &nid_);
}

void TrayIcon::Remove() {
    if (nid_.hWnd) {
        Shell_NotifyIcon(NIM_DELETE, &nid_);
    }

    if (hIcon_) {
        DestroyIcon(hIcon_);
        hIcon_ = nullptr;
    }
}

void TrayIcon::Update(const std::wstring& tooltip) {
    wcscpy_s(nid_.szTip, tooltip.c_str());
    Shell_NotifyIcon(NIM_MODIFY, &nid_);
}

void TrayIcon::HandleTrayMessage(WPARAM wParam, LPARAM lParam) {
    if (wParam != 1) return;

    switch (lParam) {
        case WM_LBUTTONUP:
            // 左键单击显示主窗口
            if (mainWindow_) {
                mainWindow_->Show();
            }
            break;

        case WM_LBUTTONDBLCLK:
            // 双击显示主窗口
            if (mainWindow_) {
                mainWindow_->Show();
            }
            break;

        case WM_RBUTTONUP:
            // 右键显示菜单
            ShowContextMenu();
            break;
    }
}

void TrayIcon::ShowContextMenu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    
    AppendMenu(hMenu, MF_STRING, ID_TRAY_SHOW, L"显示主窗口");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");

    // 设置前台窗口（Win32 要求）
    SetForegroundWindow(hwnd_);

    // 显示菜单
    UINT cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y,
        0,
        hwnd_,
        nullptr
    );

    // 必须调用这个来关闭菜单
    PostMessage(hwnd_, WM_NULL, 0, 0);
    
    DestroyMenu(hMenu);

    // 处理命令
    if (cmd == ID_TRAY_SHOW) {
        if (mainWindow_) {
            mainWindow_->Show();
        }
    } else if (cmd == ID_TRAY_EXIT) {
        DestroyWindow(hwnd_);
    }
}

HICON TrayIcon::CreateDefaultIcon(HINSTANCE hInstance) {
    // 创建一个简单的图标（16x16）
    int iconSize = 16;
    HDC hdc = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdc);
    
    HBITMAP hBitmap = CreateCompatibleBitmap(hdc, iconSize, iconSize);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // 绘制一个简单的鼠标图标
    HBRUSH hBrush = CreateSolidBrush(RGB(100, 150, 255));
    RECT rect = {0, 0, iconSize, iconSize};
    FillRect(hdcMem, &rect, hBrush);
    DeleteObject(hBrush);

    // 创建掩码
    HBITMAP hMask = CreateCompatibleBitmap(hdc, iconSize, iconSize);

    SelectObject(hdcMem, hOldBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdc);

    // 创建图标
    ICONINFO iconInfo;
    iconInfo.fIcon = TRUE;
    iconInfo.xHotspot = 0;
    iconInfo.yHotspot = 0;
    iconInfo.hbmMask = hMask;
    iconInfo.hbmColor = hBitmap;

    HICON hIcon = CreateIconIndirect(&iconInfo);

    DeleteObject(hBitmap);
    DeleteObject(hMask);

    return hIcon;
}

} // namespace WinMouseFix

