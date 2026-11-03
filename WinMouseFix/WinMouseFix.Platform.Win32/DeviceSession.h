#pragma once
#include <SDKDDKVer.h>
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN11_GE
#endif
#include <Windows.h>
#include "NativeApi.h"
#include "ProductApi.h"
#include "FrameClock.h"

namespace wmf::platform {
// The same adapter is used by the real session and recording/fault-injection tests.
struct NativeApi {
    void* context{};
    HSYNTHETICPOINTERDEVICE (*create)(void*, SYNTHETIC_DEVICE_CREATION_PARAMS*){};
    BOOL (*inject)(void*, HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO*, UINT32){};
    void (*destroy)(void*, HSYNTHETICPOINTERDEVICE){};
    BOOL (*action)(void*, HSYNTHETICPOINTERDEVICE, TOUCHPAD_ACTION){};
    DWORD (*error)(void*){};
};
NativeApi LoadNativeApi() noexcept;
std::uint32_t ApiMask(const NativeApi&) noexcept;
Result MapContacts(const DeviceConfig&, const GestureFrame&, MappedFrame&) noexcept;
Result MapContacts(const DeviceConfig&, const ContactFrame&, MappedFrame&) noexcept;

class DeviceSession {
public:
    explicit DeviceSession(NativeApi api, FrameTiming timing = {}) noexcept : api_(api), owner_(GetCurrentThreadId()), clock_(timing) {}
    DeviceSession(const DeviceSession&) = delete;
    DeviceSession& operator=(const DeviceSession&) = delete;
    ~DeviceSession() { Close(); }
    Result Open(const DeviceConfig&) noexcept;
    Result Submit(const GestureFrame&, MappedFrame&) noexcept;
    Result Submit(const ContactFrame&, MappedFrame&) noexcept;
    Result StopInertia(bool end) noexcept;
    Result Close() noexcept;
private:
    Result Release(bool cancel) noexcept;
    Result Fail(DWORD error) noexcept;
    Result SubmitMapped(Phase,std::uint64_t,double,const MappedFrame&) noexcept;
    bool OnOwner() const noexcept { return owner_ == GetCurrentThreadId(); }
    NativeApi api_;
    DWORD owner_{};
    HSYNTHETICPOINTERDEVICE device_{};
    DeviceConfig config_{};
    MappedFrame last_{};
    std::uint64_t lastSequence_{};
    double lastLogicalSeconds_{};
    FrameClock clock_;
    std::uint32_t active_{};
    bool faulted_{};
    Result firstError_{};
};
}
