#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(WINMOUSEFIXCORE_EXPORTS)
#define WMF_CORE_API __declspec(dllexport)
#else
#define WMF_CORE_API __declspec(dllimport)
#endif
#if defined(WINMOUSEFIXPLATFORMWIN32_EXPORTS)
#define WMF_PLATFORM_API __declspec(dllexport)
#else
#define WMF_PLATFORM_API __declspec(dllimport)
#endif

namespace wmf {
inline constexpr std::uint32_t AbiVersion = 0x00040000;
enum class Status : std::uint32_t {
    Ok, InvalidArgument, VersionMismatch, InvalidState, OutOfRange,
    Unsupported, NativeFailure, AllocationFailure, WrongThread, Busy
};
struct Result {
    Status code{Status::Ok};
    std::uint32_t nativeError{};
    std::uint32_t cleanupError{};
    std::uint32_t reserved{};
    explicit operator bool() const noexcept { return code == Status::Ok; }
};
enum class Phase : std::uint32_t { Begin, Update, End, Cancel };
enum class Profile : std::uint32_t { Linear, PauseReverse, SmallSteps, SlowRelease, QuickRelease, Diagonal, StartThenMicro };
enum class Control : std::uint32_t { Advance, HoldMotion, Reverse, Release, Cancel };

struct alignas(8) TrajectoryConfig {
    std::uint32_t size{sizeof(TrajectoryConfig)};
    std::uint32_t version{AbiVersion};
    std::uint32_t fingers{2};
    Profile profile{Profile::Linear};
    std::uint32_t interactive{};
    std::uint32_t reserved{};
    double durationSeconds{2.0};
    double extentXmm{};
    double extentYmm{-20.0};
};
// Absolute displacement from the contact origin, in millimetres. No HWND or SDK types.
struct alignas(8) GestureFrame {
    std::uint32_t size{sizeof(GestureFrame)};
    std::uint32_t version{AbiVersion};
    std::uint64_t sequence{};
    Phase phase{Phase::Begin};
    std::uint32_t fingers{2};
    double offsetXmm{};
    double offsetYmm{};
    double logicalSeconds{};
};
struct DeviceConfig {
    std::uint32_t size{sizeof(DeviceConfig)};
    std::uint32_t version{AbiVersion};
    std::uint32_t widthHimetric{10000};
    std::uint32_t heightHimetric{6000};
};
enum ApiBits : std::uint32_t { CreateApi = 1, InjectApi = 2, DestroyApi = 4, ActionApi = 8 };
inline constexpr std::uint32_t RequiredApis = CreateApi | InjectApi | DestroyApi;
struct Capabilities {
    std::uint32_t size{sizeof(Capabilities)};
    std::uint32_t version{AbiVersion};
    std::uint32_t apiBits{};
    std::uint32_t osMajor{};
    std::uint32_t osMinor{};
    std::uint32_t osBuild{};
    std::uint32_t osRevision{};
    std::uint32_t revisionKnown{};
};
struct Contact { std::uint32_t id{}; std::int32_t x{}; std::int32_t y{}; };
struct MappedFrame {
    std::uint32_t count{};
    Contact contacts[5]{};
};
struct CoreInstance;
struct PlatformInstance;
using CoreHandle = CoreInstance*;
using PlatformHandle = PlatformInstance*;
static_assert(sizeof(Result) == 16);
static_assert(sizeof(TrajectoryConfig) == 48);
static_assert(sizeof(GestureFrame) == 48);
static_assert(sizeof(DeviceConfig) == 16);
static_assert(sizeof(Capabilities) == 32);
static_assert(std::is_standard_layout_v<GestureFrame>);
static_assert(std::is_trivially_copyable_v<GestureFrame>);
static_assert(offsetof(GestureFrame, offsetXmm) == 24);
}

extern "C" {
WMF_CORE_API std::uint32_t WmfCoreVersion() noexcept;
WMF_CORE_API wmf::Result WmfCoreCreate(const wmf::TrajectoryConfig*, wmf::CoreHandle*) noexcept;
WMF_CORE_API wmf::Result WmfCoreNext(wmf::CoreHandle, double elapsedSeconds, wmf::GestureFrame*) noexcept;
WMF_CORE_API wmf::Result WmfCoreControl(wmf::CoreHandle, wmf::Control) noexcept;
WMF_CORE_API wmf::Result WmfCoreDestroy(wmf::CoreHandle*) noexcept;
WMF_PLATFORM_API std::uint32_t WmfPlatformVersion() noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformCapabilities(wmf::Capabilities*) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformCreate(wmf::PlatformHandle*) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformOpen(wmf::PlatformHandle, const wmf::DeviceConfig*) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformMap(const wmf::DeviceConfig*, const wmf::GestureFrame*, wmf::MappedFrame*) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformSubmit(wmf::PlatformHandle, const wmf::GestureFrame*, wmf::MappedFrame*) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformStopInertia(wmf::PlatformHandle, std::uint32_t end) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformClose(wmf::PlatformHandle) noexcept;
WMF_PLATFORM_API wmf::Result WmfPlatformDestroy(wmf::PlatformHandle*) noexcept;
}
