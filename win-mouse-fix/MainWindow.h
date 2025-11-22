#pragma once

#include "Common.h"
#include <string>
#include <vector>

namespace WinMouseFix {

class ConfigManager;
class MouseHook;

/**
 * @brief 主窗口类
 */
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    /**
     * @brief 创建并显示窗口
     */
    bool Create(HINSTANCE hInstance);

    /**
     * @brief 显示窗口
     */
    void Show();

    /**
     * @brief 隐藏窗口
     */
    void Hide();

    /**
     * @brief 获取窗口句柄
     */
    HWND GetHWND() const { return hwnd_; }

    /**
     * @brief 设置配置管理器
     */
    void SetConfigManager(ConfigManager* configManager) {
        configManager_ = configManager;
    }

    /**
     * @brief 设置鼠标钩子
     */
    void SetMouseHook(MouseHook* mouseHook) {
        mouseHook_ = mouseHook;
    }
    
    /**
     * @brief 设置托盘图标
     */
    void SetTrayIcon(class TrayIcon* trayIcon) {
        trayIcon_ = trayIcon;
    }

    /**
     * @brief 刷新配置列表
     */
    void RefreshConfigList();
    
    /**
     * @brief 托盘消息 ID (必须与 TrayIcon 一致)
     */
    static const UINT WM_TRAYICON = WM_USER + 1;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void OnCommand(WPARAM wParam);
    void OnClose();
    void OnDestroy();
    void LoadConfigToUI();
    void SaveConfigFromUI();
    
    // 开机自启相关
    bool IsAutoStartEnabled();
    void SetAutoStart(bool enable);

    HWND hwnd_;
    HINSTANCE hInstance_;
    HFONT hFont_;  // 微软雅黑字体
    
    // 控件
    HWND statusLabel_;
    HWND configListBox_;
    HWND enableCheckBox_;
    HWND autoStartCheckBox_;
    HWND editButton_;
    HWND aboutButton_;
    
    ConfigManager* configManager_;
    MouseHook* mouseHook_;
    class TrayIcon* trayIcon_;
    
    static const int ID_ENABLE_CHECKBOX = 1001;
    static const int ID_AUTOSTART_CHECKBOX = 1002;
    static const int ID_EDIT_BUTTON = 1003;
    static const int ID_ABOUT_BUTTON = 1004;
    static const int ID_CONFIG_LISTBOX = 1005;
};

} // namespace WinMouseFix

