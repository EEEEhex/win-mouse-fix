#pragma once

#include "Common.h"
#include <chrono>

namespace WinMouseFix {

/**
 * @brief 按钮状态类 - 跟踪鼠标按钮的状态
 */
class ButtonState {
public:
    ButtonState();

    /**
     * @brief 设置按钮按下状态
     */
    void SetPressed(MouseButton button, const Point& position);

    /**
     * @brief 设置按钮释放状态
     */
    void SetReleased(MouseButton button);

    /**
     * @brief 检查按钮是否被按下
     */
    bool IsPressed(MouseButton button) const;

    /**
     * @brief 获取按钮按下时的位置
     */
    Point GetPressPosition(MouseButton button) const;

    /**
     * @brief 获取按钮按下的时长（毫秒）
     */
    long long GetPressDuration(MouseButton button) const;

    /**
     * @brief 重置所有按钮状态
     */
    void Reset();

private:
    struct ButtonInfo {
        bool pressed;
        Point pressPosition;
        std::chrono::steady_clock::time_point pressTime;
        
        ButtonInfo() : pressed(false), pressPosition(0, 0) {}
    };

    std::map<MouseButton, ButtonInfo> buttonStates_;
};

} // namespace WinMouseFix

