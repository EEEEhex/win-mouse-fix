#pragma once
#include <Windows.h>
#include <string>
#include "NativeApi.h"

namespace wmf::host {
class InputOwner {
public:
    ~InputOwner() { if (handle_) { ReleaseMutex(handle_); CloseHandle(handle_); } }
    explicit InputOwner(std::wstring suffix = {}) : suffix_(std::move(suffix)) {}
    InputOwner(const InputOwner&) = delete;
    InputOwner& operator=(const InputOwner&) = delete;
    Result Acquire() noexcept {
        if (handle_) return {Status::InvalidState};
        wchar_t station[256]{},desktop[256]{}; DWORD bytes{};
        if (!GetUserObjectInformationW(GetProcessWindowStation(),UOI_NAME,station,sizeof(station),&bytes) ||
            !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop,sizeof(desktop),&bytes))
            return {Status::NativeFailure,GetLastError()};
        try {
            const auto name = std::wstring(L"Local\\WinMouseFix.InputOwner.v2.")+station+L"."+desktop+suffix_;
            const auto handle = CreateMutexW(nullptr,TRUE,name.c_str());
            const auto error = GetLastError();
            if (!handle) return {Status::NativeFailure,error};
            if (error == ERROR_ALREADY_EXISTS) { CloseHandle(handle); return {Status::Busy,error}; }
            handle_ = handle; return {};
        } catch (...) { return {Status::AllocationFailure}; }
    }
private:
    HANDLE handle_{};
    std::wstring suffix_;
};
}
