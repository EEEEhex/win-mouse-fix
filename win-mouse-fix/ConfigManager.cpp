#include "ConfigManager.h"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace WinMouseFix {

ConfigManager::ConfigManager() {
}

bool ConfigManager::LoadFromFile(const std::string& filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return false;
        }
        
        json j;
        file >> j;
        file.close();
        
        return ParseJson(j);
    } catch (const std::exception&) {
        return false;
    }
}

bool ConfigManager::SaveToFile(const std::string& filepath) const {
    try {
        json j = GenerateJson();
        
        std::ofstream file(filepath);
        if (!file.is_open()) {
            return false;
        }
        
        file << j.dump(2); // 缩进2个空格
        file.close();
        
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void ConfigManager::AddGestureConfig(const GestureConfig& config) {
    gestureConfigs_.push_back(config);
}

void ConfigManager::CreateDefaultConfig() {
    gestureConfigs_.clear();
    
    // 按钮4 + 向上滑动 = 任务视图
    GestureConfig config1;
    config1.triggerButton = MouseButton::BUTTON_4;
    config1.gestureType = GestureType::SWIPE_UP;
    config1.actionType = ActionType::TASK_VIEW;
    config1.threshold = 60;
    gestureConfigs_.push_back(config1);
    
    // 按钮4 + 向左滑动 = 切换到右边的虚拟桌面
    GestureConfig config2;
    config2.triggerButton = MouseButton::BUTTON_4;
    config2.gestureType = GestureType::SWIPE_LEFT;
    config2.actionType = ActionType::SWITCH_DESKTOP_RIGHT;
    config2.threshold = 60;
    gestureConfigs_.push_back(config2);
    
    // 按钮4 + 向右滑动 = 切换到左边的虚拟桌面
    GestureConfig config3;
    config3.triggerButton = MouseButton::BUTTON_4;
    config3.gestureType = GestureType::SWIPE_RIGHT;
    config3.actionType = ActionType::SWITCH_DESKTOP_LEFT;
    config3.threshold = 60;
    gestureConfigs_.push_back(config3);
    
    // 按钮4 + 向下滑动 = 显示桌面
    GestureConfig config4;
    config4.triggerButton = MouseButton::BUTTON_4;
    config4.gestureType = GestureType::SWIPE_DOWN;
    config4.actionType = ActionType::SHOW_DESKTOP;
    config4.threshold = 60;
    gestureConfigs_.push_back(config4);
    
    // 按钮5 + 移动 = 模拟滚动
    GestureConfig config5;
    config5.triggerButton = MouseButton::BUTTON_5;
    config5.gestureType = GestureType::TWO_FINGER_SCROLL;
    config5.actionType = ActionType::SCROLL_SIMULATION;
    config5.threshold = 0;
    gestureConfigs_.push_back(config5);
}

bool ConfigManager::ParseJson(const json& j) {
    try {
        gestureConfigs_.clear();
        
        if (!j.contains("gestures") || !j["gestures"].is_array()) {
            return false;
        }
        
        for (const auto& item : j["gestures"]) {
            GestureConfig config;
            
            config.triggerButton = StringToMouseButton(item.value("triggerButton", ""));
            config.gestureType = StringToGestureType(item.value("gestureType", ""));
            config.actionType = StringToActionType(item.value("actionType", ""));
            config.threshold = item.value("threshold", 50);
            
            if (config.triggerButton != MouseButton::UNKNOWN &&
                config.gestureType != GestureType::NONE &&
                config.actionType != ActionType::NONE) {
                gestureConfigs_.push_back(config);
            }
        }
        
        return !gestureConfigs_.empty();
    } catch (const std::exception&) {
        return false;
    }
}

json ConfigManager::GenerateJson() const {
    json j;
    j["gestures"] = json::array();
    
    for (const auto& config : gestureConfigs_) {
        json item;
        item["triggerButton"] = MouseButtonToString(config.triggerButton);
        item["gestureType"] = GestureTypeToString(config.gestureType);
        item["actionType"] = ActionTypeToString(config.actionType);
        item["threshold"] = config.threshold;
        
        j["gestures"].push_back(item);
    }
    
    return j;
}

MouseButton ConfigManager::StringToMouseButton(const std::string& str) const {
    if (str == "BUTTON_4") return MouseButton::BUTTON_4;
    if (str == "BUTTON_5") return MouseButton::BUTTON_5;
    if (str == "BUTTON_MIDDLE") return MouseButton::BUTTON_MIDDLE;
    return MouseButton::UNKNOWN;
}

std::string ConfigManager::MouseButtonToString(MouseButton button) const {
    switch (button) {
        case MouseButton::BUTTON_4: return "BUTTON_4";
        case MouseButton::BUTTON_5: return "BUTTON_5";
        case MouseButton::BUTTON_MIDDLE: return "BUTTON_MIDDLE";
        default: return "UNKNOWN";
    }
}

GestureType ConfigManager::StringToGestureType(const std::string& str) const {
    if (str == "SWIPE_UP") return GestureType::SWIPE_UP;
    if (str == "SWIPE_DOWN") return GestureType::SWIPE_DOWN;
    if (str == "SWIPE_LEFT") return GestureType::SWIPE_LEFT;
    if (str == "SWIPE_RIGHT") return GestureType::SWIPE_RIGHT;
    if (str == "TWO_FINGER_SCROLL") return GestureType::TWO_FINGER_SCROLL;
    return GestureType::NONE;
}

std::string ConfigManager::GestureTypeToString(GestureType type) const {
    switch (type) {
        case GestureType::SWIPE_UP: return "SWIPE_UP";
        case GestureType::SWIPE_DOWN: return "SWIPE_DOWN";
        case GestureType::SWIPE_LEFT: return "SWIPE_LEFT";
        case GestureType::SWIPE_RIGHT: return "SWIPE_RIGHT";
        case GestureType::TWO_FINGER_SCROLL: return "TWO_FINGER_SCROLL";
        default: return "NONE";
    }
}

ActionType ConfigManager::StringToActionType(const std::string& str) const {
    if (str == "TASK_VIEW") return ActionType::TASK_VIEW;
    if (str == "SHOW_DESKTOP") return ActionType::SHOW_DESKTOP;
    if (str == "SWITCH_DESKTOP_LEFT") return ActionType::SWITCH_DESKTOP_LEFT;
    if (str == "SWITCH_DESKTOP_RIGHT") return ActionType::SWITCH_DESKTOP_RIGHT;
    if (str == "SCROLL_SIMULATION") return ActionType::SCROLL_SIMULATION;
    return ActionType::NONE;
}

std::string ConfigManager::ActionTypeToString(ActionType type) const {
    switch (type) {
        case ActionType::TASK_VIEW: return "TASK_VIEW";
        case ActionType::SHOW_DESKTOP: return "SHOW_DESKTOP";
        case ActionType::SWITCH_DESKTOP_LEFT: return "SWITCH_DESKTOP_LEFT";
        case ActionType::SWITCH_DESKTOP_RIGHT: return "SWITCH_DESKTOP_RIGHT";
        case ActionType::SCROLL_SIMULATION: return "SCROLL_SIMULATION";
        default: return "NONE";
    }
}

} // namespace WinMouseFix

