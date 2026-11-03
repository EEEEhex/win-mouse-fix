#pragma once
#include <Windows.h>

namespace wmf::host {
class DiagnosticHotkeys {
public:
    using RegisterFn = decltype(&RegisterHotKey);
    using UnregisterFn = decltype(&UnregisterHotKey);
    using ErrorFn = decltype(&GetLastError);
    DiagnosticHotkeys(RegisterFn add = RegisterHotKey, UnregisterFn remove = UnregisterHotKey, ErrorFn error = GetLastError)
        : add_(add), remove_(remove), error_(error) {}
    ~DiagnosticHotkeys() { Release(); }
    DiagnosticHotkeys(const DiagnosticHotkeys&) = delete;
    DiagnosticHotkeys& operator=(const DiagnosticHotkeys&) = delete;
    bool Acquire(HWND window, DWORD& error) noexcept {
        if (registered_) { error = ERROR_ALREADY_EXISTS; return false; }
        window_ = window;
        for (int i = 0; i < 7; ++i) {
            if (!add_(window_, i + 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, static_cast<UINT>(VK_F5 + i))) {
                error = error_(); Release(); return false;
            }
            ++registered_;
        }
        error = ERROR_SUCCESS;
        return true;
    }
    void Release() noexcept {
        while (registered_) { remove_(window_, registered_); --registered_; }
        window_ = nullptr;
    }
private:
    RegisterFn add_;
    UnregisterFn remove_;
    ErrorFn error_;
    HWND window_{};
    int registered_{};
};
}
