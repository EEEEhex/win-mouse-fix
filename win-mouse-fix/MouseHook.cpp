#include "MouseHook.h"
#include "GestureRecognizer.h"
#include <iostream>

namespace WinMouseFix {

// 静态成员初始化
MouseHook* MouseHook::instance_ = nullptr;

MouseHook::MouseHook() 
    : hook_(nullptr)
    , gestureRecognizer_(nullptr) {
    instance_ = this;
}

MouseHook::~MouseHook() {
    Uninstall();
    instance_ = nullptr;
}

bool MouseHook::Install() {
    if (hook_) {
        return true;
    }

    hook_ = SetWindowsHookEx(
        WH_MOUSE_LL,
        HookProc,
        GetModuleHandle(nullptr),
        0
    );

    return hook_ != nullptr;
}

void MouseHook::Uninstall() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
        
        // 重要：给系统时间处理最后的事件
        Sleep(100);
    }
}

Point MouseHook::GetCurrentMousePosition() const {
    POINT pt;
    GetCursorPos(&pt);
    return Point(pt.x, pt.y);
}

LRESULT CALLBACK MouseHook::HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (instance_) {
        return instance_->HandleHook(nCode, wParam, lParam);
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT MouseHook::HandleHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0 || !gestureRecognizer_) {
        return CallNextHookEx(hook_, nCode, wParam, lParam);
    }

    MSLLHOOKSTRUCT* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    bool blockEvent = false;

    switch (wParam) {
        case WM_MOUSEMOVE:
            // 重要：鼠标移动事件总是传递，不阻止
            // 只在后台处理手势识别
            HandleMouseMove(info);
            break;

        case WM_XBUTTONDOWN: {
            // 侧键按下 (BUTTON_4 或 BUTTON_5)
            WORD button = HIWORD(info->mouseData);
            MouseButton mouseBtn = (button == XBUTTON1) ? MouseButton::BUTTON_4 : MouseButton::BUTTON_5;
            Point pos(info->pt.x, info->pt.y);
            blockEvent = HandleMouseButtonDown(mouseBtn, info);
            break;
        }

        case WM_XBUTTONUP: {
            // 侧键释放
            WORD button = HIWORD(info->mouseData);
            MouseButton mouseBtn = (button == XBUTTON1) ? MouseButton::BUTTON_4 : MouseButton::BUTTON_5;
            Point pos(info->pt.x, info->pt.y);
            blockEvent = HandleMouseButtonUp(mouseBtn, info);
            break;
        }

        case WM_MBUTTONDOWN: {
            // 中键按下
            Point pos(info->pt.x, info->pt.y);
            blockEvent = HandleMouseButtonDown(MouseButton::BUTTON_MIDDLE, info);
            break;
        }

        case WM_MBUTTONUP: {
            // 中键释放
            Point pos(info->pt.x, info->pt.y);
            blockEvent = HandleMouseButtonUp(MouseButton::BUTTON_MIDDLE, info);
            break;
        }

        default:
            break;
    }

    // 只有明确需要阻止的事件才阻止
    // 鼠标移动始终不阻止
    if (blockEvent) {
        return 1;
    }

    return CallNextHookEx(hook_, nCode, wParam, lParam);
}

void MouseHook::HandleMouseMove(const MSLLHOOKSTRUCT* info) {
    Point currentPos(info->pt.x, info->pt.y);
    gestureRecognizer_->OnMouseMove(currentPos);
}

bool MouseHook::HandleMouseButtonDown(MouseButton button, const MSLLHOOKSTRUCT* info) {
    Point pos(info->pt.x, info->pt.y);
    bool handled = gestureRecognizer_->OnButtonDown(button, pos);
    
    // 如果手势识别器接管了这个按钮，阻止默认行为
    return handled;
}

bool MouseHook::HandleMouseButtonUp(MouseButton button, const MSLLHOOKSTRUCT* info) {
    Point pos(info->pt.x, info->pt.y);
    bool handled = gestureRecognizer_->OnButtonUp(button, pos);
    
    // 如果发生了手势，阻止默认行为（前进/后退）
    return handled;
}

MouseButton MouseHook::GetMouseButtonFromMessage(WPARAM wParam) {
    switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return MouseButton::BUTTON_LEFT;

        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return MouseButton::BUTTON_RIGHT;

        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return MouseButton::BUTTON_MIDDLE;

        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            // 需要从 mouseData 中提取具体是哪个侧键
            return MouseButton::UNKNOWN;

        default:
            return MouseButton::UNKNOWN;
    }
}

} // namespace WinMouseFix

