#pragma once

#include "Common.h"

namespace WinMouseFix {

/**
 * @brief Windows 系统动作执行类
 * 
 * 负责执行各种 Windows 系统操作，如切换虚拟桌面、显示任务视图等
 */
class WindowsActions {
public:
    WindowsActions();
    ~WindowsActions();

    /**
     * @brief 显示任务视图 (Win+Tab)
     */
    void ShowTaskView();

    /**
     * @brief 显示/隐藏桌面 (Win+D)
     */
    void ShowDesktop();

    /**
     * @brief 切换到左边的虚拟桌面 (Ctrl+Win+Left)
     */
    void SwitchDesktopLeft();

    /**
     * @brief 切换到右边的虚拟桌面 (Ctrl+Win+Right)
     */
    void SwitchDesktopRight();

    /**
     * @brief 模拟鼠标滚轮滚动
     * @param deltaX 水平滚动量
     * @param deltaY 垂直滚动量
     */
    void SimulateScroll(int deltaX, int deltaY);

    /**
     * @brief 执行自定义热键
     * @param modifiers 修饰键 (VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN)
     * @param key 主键
     */
    void ExecuteHotkey(DWORD modifiers, DWORD key);

    /**
     * @brief 执行指定的动作类型
     */
    void ExecuteAction(ActionType action);

private:
    /**
     * @brief 发送键盘输入
     */
    void SendKeyCombo(const std::vector<WORD>& keys, bool keyDown);

    /**
     * @brief 按下键
     */
    void PressKey(WORD key);

    /**
     * @brief 释放键
     */
    void ReleaseKey(WORD key);

    /**
     * @brief 发送完整的按键序列（按下+释放）
     */
    void SendKeySequence(const std::vector<WORD>& keys);

private:
    bool initialized_;
};

} // namespace WinMouseFix

