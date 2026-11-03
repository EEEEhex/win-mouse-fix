#pragma once
#include "Report.h"
#include <atomic>

namespace wmf::host {
struct RuntimeFunctions {
    decltype(&WmfCoreCreate) coreCreate{WmfCoreCreate};
    decltype(&WmfCoreNext) coreNext{WmfCoreNext};
    decltype(&WmfCoreControl) coreControl{WmfCoreControl};
    decltype(&WmfCoreDestroy) coreDestroy{WmfCoreDestroy};
    decltype(&WmfPlatformCreate) platformCreate{WmfPlatformCreate};
    decltype(&WmfPlatformOpen) platformOpen{WmfPlatformOpen};
    decltype(&WmfPlatformMap) platformMap{WmfPlatformMap};
    decltype(&WmfPlatformSubmit) platformSubmit{WmfPlatformSubmit};
    decltype(&WmfPlatformStopInertia) platformStopInertia{WmfPlatformStopInertia};
    decltype(&WmfPlatformClose) platformClose{WmfPlatformClose};
    decltype(&WmfPlatformDestroy) platformDestroy{WmfPlatformDestroy};
};
enum class RuntimeState : unsigned { Idle, Armed, Running, Stopping, Stopped };
inline std::int64_t NextDeadline(std::int64_t previous, std::int64_t now, std::int64_t interval) noexcept {
    const auto planned = previous + interval;
    return planned > now ? planned : now + interval;
}
inline std::int64_t QpcNow() noexcept { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return t.QuadPart; }
class InputRuntime {
public:
    explicit InputRuntime(ReportWriter&, RuntimeFunctions = {});
    ~InputRuntime();
    InputRuntime(const InputRuntime&) = delete;
    InputRuntime& operator=(const InputRuntime&) = delete;
    Result Start(StartInfo);
    Result ControlMotion(Control);
    void Cancel() noexcept;
    void Suspend() noexcept;
    Result StopInertia();
    void Stop();
    RuntimeState State() const noexcept { return state_.load(); }
    Status LastStatus() const noexcept { return lastStatus_.load(); }
    unsigned LatestExperiment() const noexcept { return latestExperiment_.load(); }
private:
    enum class CommandKind { Start, Control, StopInertia };
    struct Command { CommandKind kind{}; Control control{}; std::int64_t received{}; StartInfo start{}; };
    void Run() noexcept;
    void Begin(const Command&);
    void Step(std::int64_t now);
    void End(Result result, bool discardDevice);
    void Emit(TraceRecord record) noexcept;
    void DisposeDevice() noexcept;
    void ArmTimer();
    ReportWriter& report_;
    RuntimeFunctions api_;
    SpscRing<Command, 32> commands_;
    HANDLE wake_{}, stop_{}, timer_{};
    std::thread thread_;
    std::atomic<RuntimeState> state_{RuntimeState::Idle};
    std::atomic<bool> cancel_{};
    std::atomic<Status> lastStatus_{Status::Ok};
    std::atomic<unsigned> latestExperiment_{};
    unsigned nextExperiment_{};
    StartInfo current_{};
    CoreHandle core_{};
    PlatformHandle platform_{};
    DeviceConfig openedConfig_{};
    bool deviceOpen_{}, begun_{};
    std::int64_t frequency_{}, started_{}, deadline_{}, received_{};
};
}
