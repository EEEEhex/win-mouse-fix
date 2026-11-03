#pragma once
#include "NativeApi.h"

namespace wmf {
enum class MouseInputKind : std::uint32_t { Down, Move, Up, Cancel, Wheel };
enum class CancelReason : std::uint32_t {
    None, Pause, TargetChanged, DeviceRemoved, IncompleteInput, Overflow,
    NativeError, Shutdown, OtherInput, UnsupportedInput, StaleInput
};
struct alignas(8) MouseInput {
    std::uint32_t size{sizeof(MouseInput)}, version{AbiVersion};
    MouseInputKind kind{MouseInputKind::Move};
    CancelReason reason{CancelReason::None};
    std::uint64_t session{}, sequence{}, firstQpc{}, lastQpc{}, device{}, target{};
    std::int64_t dx{}, dy{}; // Raw relative counts, never screen pixels.
    std::int32_t anchorX{}, anchorY{}; // Per-monitor-aware screen coordinates.
    std::uint32_t samples{1}, nativeError{};
    std::uint32_t cleanupError{}, flags{};
};
struct DragConfig {
    std::uint32_t size{sizeof(DragConfig)}, version{AbiVersion};
    double thresholdCounts{8.0};
    double mmPerCountX{0.1}, mmPerCountY{0.1};
    std::uint64_t qpcFrequency{10000000};
};
enum class DragState : std::uint32_t { Idle, Pending, Active, Ended, Canceled };
enum DragEffect : std::uint32_t { ClickEffect = 1, FinishedEffect = 2, CanceledEffect = 4 };
struct DragOutput {
    std::uint32_t size{sizeof(DragOutput)}, version{AbiVersion};
    DragState state{DragState::Idle};
    std::uint32_t effects{}, frameCount{}, reserved{};
    std::int64_t rawX{}, rawY{};
    GestureFrame frames[3]{};
};
struct DragInstance;
using DragHandle = DragInstance*;
using CaptureSink = bool (*)(void*, const MouseInput*) noexcept;
struct CaptureConfig {
    std::uint32_t size{sizeof(CaptureConfig)}, version{AbiVersion};
    void* context{};
    CaptureSink sink{};
    std::uint32_t confirmationTimeoutMs{100}, reserved{};
};
enum class CaptureCommand : std::uint32_t { Enable, Disable, CompleteDrag, CompleteClick, Cancel };
struct CaptureStats {
    std::uint32_t size{sizeof(CaptureStats)}, version{AbiVersion};
    std::uint64_t rawPackets{}, rawMoves{}, blockedMoves{}, capturedDowns{},
        rawDowns{}, rawUps{}, replayCalls{}, replayFailures{}, cancellations{},
        hookCalls{}, hookTicks{}, hookMaxTicks{},rawWheels{},blockedWheels{},injectedWheelHooks{},keyboardTakeovers{};
};
struct CaptureInstance;
using CaptureHandle = CaptureInstance*;
static_assert(sizeof(MouseInput) == 104 && sizeof(DragConfig) == 40);
static_assert(sizeof(DragOutput) == 184 && sizeof(CaptureConfig) == 32);
static_assert(std::is_standard_layout_v<MouseInput> && std::is_trivially_copyable_v<MouseInput>);
static_assert(std::is_standard_layout_v<DragOutput> && std::is_trivially_copyable_v<DragOutput>);
}
extern "C" {
WMF_CORE_API wmf::Result WmfDragCreate(const wmf::DragConfig*, wmf::DragHandle*) noexcept;
WMF_CORE_API wmf::Result WmfDragFeed(wmf::DragHandle, const wmf::MouseInput*, wmf::DragOutput*) noexcept;
WMF_CORE_API wmf::Result WmfDragReset(wmf::DragHandle) noexcept;
WMF_CORE_API wmf::Result WmfDragDestroy(wmf::DragHandle*) noexcept;
WMF_PLATFORM_API wmf::Result WmfCaptureCreate(const wmf::CaptureConfig*, wmf::CaptureHandle*) noexcept;
// Commands may be posted from the worker; destruction must run on the creating thread.
WMF_PLATFORM_API wmf::Result WmfCapturePost(wmf::CaptureHandle, wmf::CaptureCommand, std::uint64_t session) noexcept;
WMF_PLATFORM_API wmf::Result WmfCaptureReadStats(wmf::CaptureHandle, wmf::CaptureStats*) noexcept;
WMF_PLATFORM_API wmf::Result WmfCaptureDestroy(wmf::CaptureHandle*) noexcept;
}
