#include "GestureRecognizer.h"
#include "WindowsActions.h"
#include <iostream>
#include <cmath>

namespace WinMouseFix {

GestureRecognizer::GestureRecognizer(WindowsActions* actions)
    : actions_(actions)
    , running_(true)
    , activeButton_(MouseButton::UNKNOWN)
    , gestureTriggered_(false)
    , currentGesture_(GestureType::NONE)
    , scrollMode_(false) {
    
    // 启动处理线程
    processingThread_ = std::thread(&GestureRecognizer::ProcessingThreadFunc, this);
}

GestureRecognizer::~GestureRecognizer() {
    // 停止处理线程
    running_ = false;
    queueCV_.notify_one();
    
    if (processingThread_.joinable()) {
        processingThread_.join();
    }
}

void GestureRecognizer::LoadConfig(const std::vector<GestureConfig>& configs) {
    configs_ = configs;
}

void GestureRecognizer::ProcessingThreadFunc() {
    while (running_) {
        MouseEvent event;
        
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] { return !eventQueue_.empty() || !running_; });
            
            if (!running_) break;
            
            if (eventQueue_.empty()) continue;
            
            event = eventQueue_.front();
            eventQueue_.pop();
        }
        
        ProcessEvent(event);
    }
}

void GestureRecognizer::ProcessEvent(const MouseEvent& event) {
    switch (event.type) {
        case MouseEvent::BUTTON_DOWN:
            ProcessButtonDown(event.button, event.position);
            break;
        case MouseEvent::BUTTON_UP:
            ProcessButtonUp(event.button, event.position);
            break;
        case MouseEvent::MOUSE_MOVE:
            ProcessMouseMove(event.position);
            break;
    }
}

bool GestureRecognizer::OnButtonDown(MouseButton button, const Point& position) {
    // 快速入队
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        eventQueue_.push({MouseEvent::BUTTON_DOWN, button, position});
    }
    queueCV_.notify_one();
    
    // 如果有配置，阻止默认行为
    return HasConfigForButton(button);
}

void GestureRecognizer::ProcessButtonDown(MouseButton button, const Point& position) {
    buttonState_.SetPressed(button, position);
    
    // 检查是否有对应的手势配置
    bool hasConfig = false;
    for (const auto& config : configs_) {
        if (config.triggerButton == button) {
            hasConfig = true;
            break;
        }
    }
    
    if (hasConfig) {
        activeButton_ = button;
        gestureStartPos_ = position;
        lastMousePos_ = position;
        gestureTriggered_ = false;
        currentGesture_ = GestureType::NONE;
        scrollMode_ = false;
        scrollAccumulator_ = Point(0, 0);
    }
}

bool GestureRecognizer::OnButtonUp(MouseButton button, const Point& position) {
    // 检查这个按钮是否触发了手势
    bool hadGesture = false;
    if (button == activeButton_ && (gestureTriggered_ || scrollMode_)) {
        hadGesture = true;
    }
    
    // 快速入队
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        eventQueue_.push({MouseEvent::BUTTON_UP, button, position});
    }
    queueCV_.notify_one();
    
    // 如果触发了手势，阻止默认行为
    return hadGesture;
}

void GestureRecognizer::ProcessButtonUp(MouseButton button, const Point& position) {
    // 在工作线程中处理
    buttonState_.SetReleased(button);
    
    if (button == activeButton_) {
        activeButton_ = MouseButton::UNKNOWN;
        gestureTriggered_ = false;
        currentGesture_ = GestureType::NONE;
        scrollMode_ = false;
    }
}

bool GestureRecognizer::OnMouseMove(const Point& currentPos) {
    // 只有在有活动按钮时才入队
    if (activeButton_ != MouseButton::UNKNOWN) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        
        // 如果队列太长，跳过（避免事件堆积）
        if (eventQueue_.size() < 10) {
            eventQueue_.push({MouseEvent::MOUSE_MOVE, MouseButton::UNKNOWN, currentPos});
            queueCV_.notify_one();
        }
    }
    
    return false;
}

void GestureRecognizer::ProcessMouseMove(const Point& currentPos) {
    if (activeButton_ == MouseButton::UNKNOWN) {
        return;
    }
    
    Point delta = currentPos - gestureStartPos_;
    double dist = delta.length();
    Point moveDelta = currentPos - lastMousePos_;
    lastMousePos_ = currentPos;
    
    // 如果已经触发过手势，不再处理（除了滚动模式）
    if (gestureTriggered_ && !scrollMode_) {
        return;
    }
    
    for (const auto& cfg : configs_) {
        if (cfg.triggerButton != activeButton_) {
            continue;
        }
        
        // 滚动模式：持续处理
        if (cfg.gestureType == GestureType::TWO_FINGER_SCROLL) {
            if (!scrollMode_) {
                scrollMode_ = true;
            }
            HandleScrollSimulation(moveDelta);
            return;
        }
        
        // 一次性手势：只触发一次
        if (dist >= cfg.threshold && !gestureTriggered_) {
            GestureType gesture = RecognizeGesture(delta, dist);
            if (gesture != GestureType::NONE && cfg.gestureType == gesture) {
                gestureTriggered_ = true;
                currentGesture_ = gesture;
                ExecuteGesture(cfg, delta);
                return; // 执行后立即返回，不再处理
            }
        }
    }
}

void GestureRecognizer::Reset() {
    activeButton_ = MouseButton::UNKNOWN;
    gestureTriggered_ = false;
    currentGesture_ = GestureType::NONE;
    scrollMode_ = false;
    buttonState_.Reset();
}

GestureType GestureRecognizer::RecognizeGesture(const Point& delta, double distance) const {
    if (distance < 30) {
        return GestureType::NONE;
    }
    
    // 使用绝对值比较，更简单直接
    int absDx = abs(delta.x);
    int absDy = abs(delta.y);
    
    // 判断是水平还是垂直移动
    if (absDx > absDy) {
        // 水平移动
        return delta.x > 0 ? GestureType::SWIPE_RIGHT : GestureType::SWIPE_LEFT;
    } else {
        // 垂直移动
        return delta.y > 0 ? GestureType::SWIPE_DOWN : GestureType::SWIPE_UP;
    }
}

const GestureConfig* GestureRecognizer::FindConfig(MouseButton button, GestureType gesture) const {
    for (const auto& config : configs_) {
        if (config.triggerButton == button && config.gestureType == gesture) {
            return &config;
        }
    }
    return nullptr;
}

void GestureRecognizer::ExecuteGesture(const GestureConfig& config, const Point& delta) {
    // 移除日志输出以提高性能
    actions_->ExecuteAction(config.actionType);
}

void GestureRecognizer::HandleScrollSimulation(const Point& delta) {
    // 累积滚动量
    scrollAccumulator_.x += delta.x;
    scrollAccumulator_.y += delta.y;
    
    // 滚动灵敏度因子（可以调整 - 数值越大越不敏感）
    const int scrollFactor = 5;
    
    // 计算实际滚动量
    int scrollX = scrollAccumulator_.x / scrollFactor;
    int scrollY = scrollAccumulator_.y / scrollFactor;
    
    if (scrollX != 0 || scrollY != 0) {
        // 发送滚动事件
        actions_->SimulateScroll(scrollX, scrollY);
        
        // 减去已处理的量
        scrollAccumulator_.x -= scrollX * scrollFactor;
        scrollAccumulator_.y -= scrollY * scrollFactor;
    }
}

} // namespace WinMouseFix

