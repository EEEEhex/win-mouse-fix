#pragma once

#include "Common.h"
#include <functional>

namespace WinMouseFix {

class GestureRecognizer;

/**
 * @brief 鼠标钩子类 - 负责拦截和处理鼠标事件
 * 
 * 使用 Windows 低级鼠标钩子 (WH_MOUSE_LL) 来监听系统范围内的鼠标事件
 */
class MouseHook {
public:
    MouseHook();
    ~MouseHook();

    // 禁止拷贝
    MouseHook(const MouseHook&) = delete;
    MouseHook& operator=(const MouseHook&) = delete;

    /**
     * @brief 安装鼠标钩子
     * @return 成功返回 true
     */
    bool Install();

    /**
     * @brief 卸载鼠标钩子
     */
    void Uninstall();

    /**
     * @brief 检查钩子是否已安装
     */
    bool IsInstalled() const { return hook_ != nullptr; }

    /**
     * @brief 设置手势识别器
     */
    void SetGestureRecognizer(GestureRecognizer* recognizer) {
        gestureRecognizer_ = recognizer;
    }

    /**
     * @brief 获取当前鼠标位置
     */
    Point GetCurrentMousePosition() const;

private:
    /**
     * @brief 静态钩子过程回调
     */
    static LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam);

    /**
     * @brief 实例钩子处理方法
     */
    LRESULT HandleHook(int nCode, WPARAM wParam, LPARAM lParam);

    /**
     * @brief 处理鼠标移动事件
     */
    void HandleMouseMove(const MSLLHOOKSTRUCT* info);

    /**
     * @brief 处理鼠标按钮按下事件
     */
    bool HandleMouseButtonDown(MouseButton button, const MSLLHOOKSTRUCT* info);

    /**
     * @brief 处理鼠标按钮释放事件
     */
    bool HandleMouseButtonUp(MouseButton button, const MSLLHOOKSTRUCT* info);

    /**
     * @brief 将 Windows 鼠标消息转换为 MouseButton 枚举
     */
    MouseButton GetMouseButtonFromMessage(WPARAM wParam);

private:
    HHOOK hook_;                          // 钩子句柄
    GestureRecognizer* gestureRecognizer_; // 手势识别器
    
    static MouseHook* instance_;          // 单例实例（用于静态回调）
};

} // namespace WinMouseFix

