#pragma once

#include "Common.h"
#include <string>
#include <vector>
#include "json.hpp"

namespace WinMouseFix {

/**
 * @brief 配置管理器 - 负责加载和保存配置
 */
class ConfigManager {
public:
    ConfigManager();

    /**
     * @brief 从文件加载配置
     * @param filepath 配置文件路径
     * @return 成功返回 true
     */
    bool LoadFromFile(const std::string& filepath);

    /**
     * @brief 保存配置到文件
     * @param filepath 配置文件路径
     * @return 成功返回 true
     */
    bool SaveToFile(const std::string& filepath) const;

    /**
     * @brief 获取手势配置列表
     */
    const std::vector<GestureConfig>& GetGestureConfigs() const {
        return gestureConfigs_;
    }

    /**
     * @brief 添加手势配置
     */
    void AddGestureConfig(const GestureConfig& config);

    /**
     * @brief 创建默认配置
     */
    void CreateDefaultConfig();

private:
    /**
     * @brief 解析 JSON 配置
     */
    bool ParseJson(const nlohmann::json& j);

    /**
     * @brief 生成 JSON 配置
     */
    nlohmann::json GenerateJson() const;

    /**
     * @brief 字符串转 MouseButton
     */
    MouseButton StringToMouseButton(const std::string& str) const;

    /**
     * @brief MouseButton 转字符串
     */
    std::string MouseButtonToString(MouseButton button) const;

    /**
     * @brief 字符串转 GestureType
     */
    GestureType StringToGestureType(const std::string& str) const;

    /**
     * @brief GestureType 转字符串
     */
    std::string GestureTypeToString(GestureType type) const;

    /**
     * @brief 字符串转 ActionType
     */
    ActionType StringToActionType(const std::string& str) const;

    /**
     * @brief ActionType 转字符串
     */
    std::string ActionTypeToString(ActionType type) const;

private:
    std::vector<GestureConfig> gestureConfigs_;
};

} // namespace WinMouseFix

