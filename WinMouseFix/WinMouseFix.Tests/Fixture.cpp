#include <cstdint>
#if defined(WMF_FIXTURE_NO_SYSTEM_APIS)
#include "NativeApi.h"
#include "ContentDragApi.h"
#include "ProductApi.h"
extern "C" std::uint32_t WmfPlatformVersion() noexcept { return wmf::AbiVersion; }
extern "C" wmf::Result WmfPlatformCapabilities(wmf::Capabilities* out) noexcept { *out = {}; return {}; }
extern "C" wmf::Result WmfPlatformCreate(wmf::PlatformHandle* out) noexcept {
    *out = reinterpret_cast<wmf::PlatformHandle>(static_cast<std::uintptr_t>(1)); return {};
}
extern "C" wmf::Result WmfPlatformOpen(wmf::PlatformHandle, const wmf::DeviceConfig*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfPlatformMap(const wmf::DeviceConfig*,const wmf::GestureFrame*,wmf::MappedFrame*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfPlatformSubmit(wmf::PlatformHandle,const wmf::GestureFrame*,wmf::MappedFrame*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfPlatformMapContacts(const wmf::DeviceConfig*,const wmf::ContactFrame*,wmf::MappedFrame*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfPlatformSubmitContacts(wmf::PlatformHandle,const wmf::ContactFrame*,wmf::MappedFrame*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfPlatformStopInertia(wmf::PlatformHandle,std::uint32_t) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfPlatformClose(wmf::PlatformHandle) noexcept { return {}; }
extern "C" wmf::Result WmfPlatformDestroy(wmf::PlatformHandle* out) noexcept { *out = nullptr; return {}; }
extern "C" wmf::Result WmfCaptureCreate(const wmf::CaptureConfig*,wmf::CaptureHandle*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfCapturePost(wmf::CaptureHandle,wmf::CaptureCommand,std::uint64_t) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfCaptureReadStats(wmf::CaptureHandle,wmf::CaptureStats*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfCaptureDestroy(wmf::CaptureHandle*) noexcept { return {}; }
extern "C" wmf::Result WmfExecuteAction(const wmf::ActionRequest*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfProductCaptureCreate(const wmf::ProductCaptureConfig*,wmf::CaptureHandle*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfProductCaptureUpdate(wmf::CaptureHandle,const wmf::BindingConfig*) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfProductCapturePolicy(wmf::CaptureHandle,std::uint64_t,wmf::TargetPolicy) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfProductCaptureRecording(wmf::CaptureHandle,std::uint32_t) noexcept { return {wmf::Status::Unsupported,127}; }
extern "C" wmf::Result WmfProductCaptureRevision(wmf::CaptureHandle,wmf::CaptureRevision*) noexcept { return {wmf::Status::Unsupported,127}; }
#elif defined(WMF_FIXTURE_NO_VERSION)
extern "C" __declspec(dllexport) std::uint32_t FixtureMarker() noexcept { return 1; }
#elif defined(WMF_FIXTURE_WRONG_VERSION)
// The previous product ABI must be rejected after Core takes over pinch timing.
extern "C" __declspec(dllexport) std::uint32_t WmfCoreVersion() noexcept { return 0x00030000; }
#else
extern "C" __declspec(dllexport) std::uint32_t WmfCoreVersion() noexcept { return 0x00040000; }
#endif
