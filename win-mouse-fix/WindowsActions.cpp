#include "WindowsActions.h"
#include <iostream>

namespace WinMouseFix {

WindowsActions::WindowsActions() : initialized_(false) {
    // 初始化 COM（某些操作可能需要）
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    initialized_ = true;
}

WindowsActions::~WindowsActions() {
    if (initialized_) {
        CoUninitialize();
    }
}

void WindowsActions::ShowTaskView() {
    // 移除日志输出以提高性能
    SendKeySequence({VK_LWIN, VK_TAB});
}

void WindowsActions::ShowDesktop() {
    SendKeySequence({VK_LWIN, 'D'});
}

void WindowsActions::SwitchDesktopLeft() {
    SendKeySequence({VK_CONTROL, VK_LWIN, VK_LEFT});
}

void WindowsActions::SwitchDesktopRight() {
    SendKeySequence({VK_CONTROL, VK_LWIN, VK_RIGHT});
}

void WindowsActions::SimulateScroll(int deltaX, int deltaY) {
    // 自然滚动：鼠标向上移动，页面向下滚动
    
    // 垂直滚动
    if (deltaY != 0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        // 自然滚动：鼠标向上（deltaY < 0），页面向下滚（正值）
        input.mi.mouseData = deltaY * 30;
        SendInput(1, &input, sizeof(INPUT));
    }
    
    // 水平滚动
    if (deltaX != 0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        // 自然滚动：鼠标向左（deltaX < 0），页面向右滚（正值）
        input.mi.mouseData = -deltaX * 30;
        SendInput(1, &input, sizeof(INPUT));
    }
}

void WindowsActions::ExecuteHotkey(DWORD modifiers, DWORD key) {
    std::vector<WORD> keys;
    
    // 添加修饰键
    if (modifiers & MOD_CONTROL) keys.push_back(VK_CONTROL);
    if (modifiers & MOD_SHIFT) keys.push_back(VK_SHIFT);
    if (modifiers & MOD_ALT) keys.push_back(VK_MENU);
    if (modifiers & MOD_WIN) keys.push_back(VK_LWIN);
    
    // 添加主键
    keys.push_back(static_cast<WORD>(key));
    
    SendKeySequence(keys);
}

void WindowsActions::ExecuteAction(ActionType action) {
    switch (action) {
        case ActionType::TASK_VIEW:
            ShowTaskView();
            break;
        
        case ActionType::SHOW_DESKTOP:
            ShowDesktop();
            break;
        
        case ActionType::SWITCH_DESKTOP_LEFT:
            SwitchDesktopLeft();
            break;
        
        case ActionType::SWITCH_DESKTOP_RIGHT:
            SwitchDesktopRight();
            break;
        
        case ActionType::SCROLL_SIMULATION:
            // 滚动由 GestureRecognizer 直接处理
            break;
        
        default:
            break;
    }
}

void WindowsActions::SendKeyCombo(const std::vector<WORD>& keys, bool keyDown) {
    std::vector<INPUT> inputs;
    
    for (WORD key : keys) {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = keyDown ? 0 : KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    
    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

void WindowsActions::PressKey(WORD key) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = 0; // Key down
    SendInput(1, &input, sizeof(INPUT));
}

void WindowsActions::ReleaseKey(WORD key) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void WindowsActions::SendKeySequence(const std::vector<WORD>& keys) {
    // 按下所有键
    for (WORD key : keys) {
        PressKey(key);
        Sleep(20); // 增加延迟确保按键被识别
    }
    
    Sleep(50); // 所有键按下后等待
    
    // 释放所有键（逆序）
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        ReleaseKey(*it);
        Sleep(20);
    }
}

} // namespace WinMouseFix

