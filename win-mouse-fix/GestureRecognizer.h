#pragma once

#include "Common.h"
#include "ButtonState.h"
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>

namespace WinMouseFix {

class WindowsActions;

/**
 * @brief 手势识别器类 - 识别鼠标手势并触发相应动作
 */
class GestureRecognizer {
public:
    explicit GestureRecognizer(WindowsActions* actions);
    ~GestureRecognizer();

    /**
     * @brief 加载手势配置
     */
    void LoadConfig(const std::vector<GestureConfig>& configs);

    /**
     * @brief 处理鼠标移动事件
     * @return 如果手势正在进行中返回 true（此时应该阻止默认行为）
     */
    bool OnMouseMove(const Point& currentPos);

    /**
     * @brief 处理鼠标按钮按下事件
     * @return 如果事件被处理返回 true
     */
    bool OnButtonDown(MouseButton button, const Point& position);

    /**
     * @brief 处理鼠标按钮释放事件
     * @return 如果事件被处理返回 true
     */
    bool OnButtonUp(MouseButton button, const Point& position);

    /**
     * @brief 重置手势识别状态
     */
    void Reset();

    /**
     * @brief 检查是否有激活的手势
     */
    bool HasActiveGesture() const { return activeButton_ != MouseButton::UNKNOWN; }
    
    /**
     * @brief 检查按钮是否有配置
     */
    bool HasConfigForButton(MouseButton button) const {
        for (const auto& config : configs_) {
            if (config.triggerButton == button) {
                return true;
            }
        }
        return false;
    }

private:
    /**
     * @brief 根据移动向量识别手势类型
     */
    GestureType RecognizeGesture(const Point& delta, double distance) const;

    /**
     * @brief 查找按钮对应的手势配置
     */
    const GestureConfig* FindConfig(MouseButton button, GestureType gesture) const;

    /**
     * @brief 执行手势对应的动作
     */
    void ExecuteGesture(const GestureConfig& config, const Point& delta);

    /**
     * @brief 处理滚动模拟
     */
    void HandleScrollSimulation(const Point& delta);
    
    /**
     * @brief 在工作线程中处理按钮按下
     */
    void ProcessButtonDown(MouseButton button, const Point& position);
    
    /**
     * @brief 在工作线程中处理按钮释放
     */
    void ProcessButtonUp(MouseButton button, const Point& position);
    
    /**
     * @brief 在工作线程中处理鼠标移动
     */
    void ProcessMouseMove(const Point& position);

private:
    // 事件队列相关
    struct MouseEvent {
        enum Type { BUTTON_DOWN, BUTTON_UP, MOUSE_MOVE };
        Type type;
        MouseButton button;
        Point position;
    };
    
    std::queue<MouseEvent> eventQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::thread processingThread_;
    std::atomic<bool> running_;
    
    void ProcessingThreadFunc();
    void ProcessEvent(const MouseEvent& event);
    
    WindowsActions* actions_;              // Windows 动作执行器
    ButtonState buttonState_;              // 按钮状态跟踪
    std::vector<GestureConfig> configs_;   // 手势配置列表
    
    MouseButton activeButton_;             // 当前激活的按钮
    Point gestureStartPos_;                // 手势开始位置
    Point lastMousePos_;                   // 上一次鼠标位置
    bool gestureTriggered_;                // 手势是否已触发
    GestureType currentGesture_;           // 当前手势类型
    
    // 滚动模拟相关
    bool scrollMode_;                      // 是否处于滚动模式
    Point scrollAccumulator_;              // 滚动累积量
};

} // namespace WinMouseFix

