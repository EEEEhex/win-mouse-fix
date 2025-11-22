#include "ButtonState.h"

namespace WinMouseFix {

ButtonState::ButtonState() {
    // 初始化所有按钮状态
    buttonStates_[MouseButton::BUTTON_4] = ButtonInfo();
    buttonStates_[MouseButton::BUTTON_5] = ButtonInfo();
    buttonStates_[MouseButton::BUTTON_MIDDLE] = ButtonInfo();
    buttonStates_[MouseButton::BUTTON_LEFT] = ButtonInfo();
    buttonStates_[MouseButton::BUTTON_RIGHT] = ButtonInfo();
}

void ButtonState::SetPressed(MouseButton button, const Point& position) {
    auto it = buttonStates_.find(button);
    if (it != buttonStates_.end()) {
        it->second.pressed = true;
        it->second.pressPosition = position;
        it->second.pressTime = std::chrono::steady_clock::now();
    }
}

void ButtonState::SetReleased(MouseButton button) {
    auto it = buttonStates_.find(button);
    if (it != buttonStates_.end()) {
        it->second.pressed = false;
    }
}

bool ButtonState::IsPressed(MouseButton button) const {
    auto it = buttonStates_.find(button);
    if (it != buttonStates_.end()) {
        return it->second.pressed;
    }
    return false;
}

Point ButtonState::GetPressPosition(MouseButton button) const {
    auto it = buttonStates_.find(button);
    if (it != buttonStates_.end()) {
        return it->second.pressPosition;
    }
    return Point(0, 0);
}

long long ButtonState::GetPressDuration(MouseButton button) const {
    auto it = buttonStates_.find(button);
    if (it != buttonStates_.end() && it->second.pressed) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.pressTime);
        return duration.count();
    }
    return 0;
}

void ButtonState::Reset() {
    for (auto& pair : buttonStates_) {
        pair.second.pressed = false;
    }
}

} // namespace WinMouseFix

