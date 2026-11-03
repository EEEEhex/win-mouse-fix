#pragma once
#include <Windows.h>
#include <wtsapi32.h>
#include "NativeApi.h"

namespace wmf::host {
class ContentControls {
public:
    ContentControls(decltype(&WTSRegisterSessionNotification) sessions = WTSRegisterSessionNotification,
        decltype(&WTSUnRegisterSessionNotification) unsessions = WTSUnRegisterSessionNotification,
        decltype(&GetLastError) error = GetLastError) noexcept : sessions_(sessions),unsessions_(unsessions),error_(error) {}
    ~ContentControls() { Release(); }
    Result Acquire(HWND window) noexcept {
        if (!window) return {Status::InvalidArgument};
        if (window_) return {Status::InvalidState};
        if (!sessions_(window,NOTIFY_FOR_THIS_SESSION)) return {Status::NativeFailure,error_()};
        window_ = window; return {};
    }
    void Release() noexcept { if (window_) { unsessions_(window_); window_ = nullptr; } }
private:
    HWND window_{};
    decltype(&WTSRegisterSessionNotification) sessions_;
    decltype(&WTSUnRegisterSessionNotification) unsessions_;
    decltype(&GetLastError) error_;
};
}
