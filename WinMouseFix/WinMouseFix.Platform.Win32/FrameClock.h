#pragma once
#include <Windows.h>
#include "NativeApi.h"

namespace wmf::platform {
struct FrameTiming {
    void* context{};
    std::uint64_t frequency{};
    std::uint64_t (*read)(void*){};
    bool (*waitUntil)(void*,std::uint64_t){};
};

// Boundary frames can arrive back-to-back even when motion is scheduled at 120 Hz.
// Pace actual calls; never advance a timestamp beyond the observed clock.
class FrameClock {
public:
    explicit FrameClock(FrameTiming timing = {}) noexcept : timing_(timing) {}
    ~FrameClock() { if (timer_) CloseHandle(timer_); }
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;
    Result Initialize() noexcept {
        if (frequency_) return {};
        if (timing_.read || timing_.waitUntil || timing_.frequency) {
            if (!timing_.read || !timing_.waitUntil || !timing_.frequency) return {Status::InvalidArgument};
            frequency_ = timing_.frequency;
        } else {
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return {Status::NativeFailure,ERROR_GEN_FAILURE};
            timer_ = CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
            if (!timer_) timer_ = CreateWaitableTimerW(nullptr,FALSE,nullptr);
            if (!timer_) return {Status::NativeFailure,GetLastError()};
            frequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
        }
        minimumGap_ = (frequency_ + 999) / 1000; // At least 1 ms, including release and cleanup.
        return {};
    }
    Result Stamp(std::uint64_t& timestamp) noexcept {
        if (!frequency_) return {Status::InvalidState};
        auto now = Read();
        const auto earliest = last_ ? last_ + minimumGap_ : now;
        while (now < earliest) {
            if (timing_.waitUntil) {
                if (!timing_.waitUntil(timing_.context,earliest)) return {Status::NativeFailure,ERROR_GEN_FAILURE};
            } else {
                const auto ticks = earliest-now;
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>((ticks*10000000 + frequency_-1)/frequency_);
                if (!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)) return {Status::NativeFailure,GetLastError()};
                if (WaitForSingleObject(timer_,INFINITE) != WAIT_OBJECT_0) return {Status::NativeFailure,GetLastError()};
            }
            const auto after = Read();
            if (after <= now) return {Status::NativeFailure,ERROR_INVALID_TIME};
            now = after;
        }
        last_ = now;
        timestamp = now;
        return {};
    }
private:
    std::uint64_t Read() const noexcept {
        if (timing_.read) return timing_.read(timing_.context);
        LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
        return static_cast<std::uint64_t>(counter.QuadPart);
    }
    FrameTiming timing_;
    HANDLE timer_{};
    std::uint64_t frequency_{}, minimumGap_{}, last_{};
};
}
