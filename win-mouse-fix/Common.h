#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <map>

namespace WinMouseFix {

// Mouse button definitions
enum class MouseButton {
    BUTTON_4 = 0,  // Side button (back)
    BUTTON_5 = 1,  // Side button (forward)
    BUTTON_MIDDLE = 2,
    BUTTON_LEFT = 3,
    BUTTON_RIGHT = 4,
    UNKNOWN = 5
};

// Gesture types
enum class GestureType {
    NONE,
    SWIPE_UP,           // 向上滑动
    SWIPE_DOWN,         // 向下滑动
    SWIPE_LEFT,         // 向左滑动
    SWIPE_RIGHT,        // 向右滑动
    TWO_FINGER_SCROLL   // 两指滚动模拟
};

// Action types
enum class ActionType {
    NONE,
    TASK_VIEW,              // 任务视图 (Win+Tab)
    SHOW_DESKTOP,           // 显示桌面 (Win+D)
    SWITCH_DESKTOP_LEFT,    // 切换到左边的虚拟桌面
    SWITCH_DESKTOP_RIGHT,   // 切换到右边的虚拟桌面
    SCROLL_SIMULATION,      // 滚动模拟
    CUSTOM_HOTKEY           // 自定义热键
};

// Point structure
struct Point {
    int x;
    int y;
    
    Point() : x(0), y(0) {}
    Point(int x_, int y_) : x(x_), y(y_) {}
    
    Point operator-(const Point& other) const {
        return Point(x - other.x, y - other.y);
    }
    
    Point operator+(const Point& other) const {
        return Point(x + other.x, y + other.y);
    }
    
    double length() const {
        return sqrt(static_cast<double>(x * x + y * y));
    }
};

// Gesture configuration
struct GestureConfig {
    MouseButton triggerButton;
    GestureType gestureType;
    ActionType actionType;
    int threshold;  // 触发手势的最小移动距离（像素）
    
    GestureConfig() 
        : triggerButton(MouseButton::UNKNOWN)
        , gestureType(GestureType::NONE)
        , actionType(ActionType::NONE)
        , threshold(50) 
    {}
};

// Utility functions
inline double distance(const Point& p1, const Point& p2) {
    int dx = p2.x - p1.x;
    int dy = p2.y - p1.y;
    return sqrt(static_cast<double>(dx * dx + dy * dy));
}

inline std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    return wstr;
}

inline std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr, nullptr);
    return str;
}

} // namespace WinMouseFix

