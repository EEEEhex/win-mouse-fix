#pragma once

#include "Common.h"

namespace WinMouseFix {

class MainWindow;

/**
 * @brief 系统托盘图标类
 */
class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    /**
     * @brief 创建托盘图标
     */
    bool Create(HWND hwnd, HINSTANCE hInstance);

    /**
     * @brief 删除托盘图标
     */
    void Remove();

    /**
     * @brief 更新托盘图标
     */
    void Update(const std::wstring& tooltip);

    /**
     * @brief 处理托盘消息
     */
    void HandleTrayMessage(WPARAM wParam, LPARAM lParam);

    /**
     * @brief 设置主窗口
     */
    void SetMainWindow(MainWindow* mainWindow) {
        mainWindow_ = mainWindow;
    }

    /**
     * @brief 托盘消息 ID
     */
    static const UINT WM_TRAYICON = WM_USER + 1;

private:
    void ShowContextMenu();
    HICON CreateDefaultIcon(HINSTANCE hInstance);

    NOTIFYICONDATA nid_;
    HWND hwnd_;
    HICON hIcon_;
    MainWindow* mainWindow_;
    
    static const int ID_TRAY_SHOW = 2001;
    static const int ID_TRAY_EXIT = 2002;
};

} // namespace WinMouseFix

