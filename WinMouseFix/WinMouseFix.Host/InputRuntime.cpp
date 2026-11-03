#include "InputRuntime.h"
#include <algorithm>
#include <stdexcept>

namespace wmf::host {
InputRuntime::InputRuntime(ReportWriter& report, RuntimeFunctions functions) : report_(report), api_(functions) {
    LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); frequency_ = f.QuadPart;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!wake_ || !stop_ || !timer_) {
        if (wake_) CloseHandle(wake_); if (stop_) CloseHandle(stop_); if (timer_) CloseHandle(timer_);
        throw std::runtime_error("Cannot create diagnostic runtime wait handles");
    }
    try { thread_ = std::thread(&InputRuntime::Run, this); }
    catch (...) { CloseHandle(wake_); CloseHandle(stop_); CloseHandle(timer_); throw; }
}
InputRuntime::~InputRuntime() { Stop(); CloseHandle(wake_); CloseHandle(stop_); CloseHandle(timer_); }
Result InputRuntime::Start(StartInfo info) {
    if (!info.prepared || info.scenario > Case::DesktopRight ||
        (info.hz != 60 && info.hz != 120 && info.hz != 240)) return {Status::InvalidArgument};
    RuntimeState expected = RuntimeState::Idle;
    if (!state_.compare_exchange_strong(expected, RuntimeState::Armed)) return {Status::Busy};
    info.experiment = ++nextExperiment_;
    latestExperiment_ = info.experiment;
    Command command; command.kind = CommandKind::Start; command.start = info; command.received = QpcNow();
    if (!commands_.Push(command)) { state_ = RuntimeState::Idle; return {Status::Busy}; }
    SetEvent(wake_);
    return {};
}
Result InputRuntime::ControlMotion(Control control) {
    if (control == Control::Cancel) { Cancel(); return {}; }
    if (state_ != RuntimeState::Running) return {Status::InvalidState};
    Command c; c.kind = CommandKind::Control; c.control = control; c.received = QpcNow();
    if (!commands_.Push(c)) return {Status::Busy};
    SetEvent(wake_); return {};
}
void InputRuntime::Cancel() noexcept { cancel_ = true; SetEvent(wake_); }
void InputRuntime::Suspend() noexcept { Cancel(); }
Result InputRuntime::StopInertia() {
    if (state_ == RuntimeState::Stopped || state_ == RuntimeState::Stopping) return {Status::InvalidState};
    Command c; c.kind = CommandKind::StopInertia; c.received = QpcNow();
    if (!commands_.Push(c)) return {Status::Busy};
    SetEvent(wake_); return {};
}
void InputRuntime::Stop() {
    SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
}
void InputRuntime::Emit(TraceRecord r) noexcept {
    if (!r.returned) r.returned = QpcNow();
    if (!r.experiment) r.experiment = current_.experiment;
    report_.Push(r);
}
void InputRuntime::DisposeDevice() noexcept {
    if (!platform_) return;
    const auto result = api_.platformDestroy(&platform_);
    TraceRecord r; r.kind = TraceKind::Cleanup; r.result = result; Emit(r);
    if (!result) lastStatus_ = result.code;
    deviceOpen_ = false;
}
void InputRuntime::Begin(const Command& command) {
    current_ = command.start; received_ = command.received;
    TraceRecord record; record.kind = TraceKind::Start; record.start = current_; record.received = received_; Emit(record);
    auto result = api_.coreCreate(&current_.trajectory, &core_);
    record = {}; record.kind = TraceKind::Initialization; record.detail = 1; record.result = result; Emit(record);
    if (!result) { End(result, false); return; }
    for (const double position : {0.0, 1.0}) {
        GestureFrame endpoint; endpoint.fingers = current_.trajectory.fingers;
        endpoint.offsetXmm = position * current_.trajectory.extentXmm;
        endpoint.offsetYmm = position * current_.trajectory.extentYmm;
        MappedFrame mapped;
        result = api_.platformMap(&current_.device, &endpoint, &mapped);
        if (!result) { End(result, false); return; }
    }
    if (deviceOpen_ && (openedConfig_.widthHimetric != current_.device.widthHimetric ||
        openedConfig_.heightHimetric != current_.device.heightHimetric)) DisposeDevice();
    if (!platform_) {
        result = api_.platformCreate(&platform_);
        record = {}; record.kind = TraceKind::Initialization; record.detail = 2; record.result = result; Emit(record);
        if (!result) { End(result, true); return; }
    }
    if (!deviceOpen_) {
        result = api_.platformOpen(platform_, &current_.device);
        record = {}; record.kind = TraceKind::Device; record.result = result; Emit(record);
        if (!result) { End(result, true); return; }
        openedConfig_ = current_.device; deviceOpen_ = true;
    }
    begun_ = false;
    started_ = QpcNow(); deadline_ = started_;
    lastStatus_ = Status::Ok; state_ = RuntimeState::Running;
    Step(started_);
}
void InputRuntime::Step(std::int64_t now) {
    if (state_ != RuntimeState::Running || !core_) return;
    TraceRecord r; r.kind = TraceKind::Frame; r.planned = deadline_; r.received = received_; r.processStart = QpcNow();
    r.result = api_.coreNext(core_, static_cast<double>(now - started_) / static_cast<double>(frequency_), &r.frame);
    if (!r.result) { r.returned = QpcNow(); Emit(r); End(r.result, true); return; }
    r.submitStart = QpcNow();
    r.result = api_.platformSubmit(platform_, &r.frame, &r.mapped);
    r.returned = QpcNow(); Emit(r);
    if (!r.result) { End(r.result, true); return; }
    begun_ = true;
    if (r.frame.phase == Phase::End || r.frame.phase == Phase::Cancel) {
        End({}, r.frame.phase == Phase::Cancel);
        return;
    }
    deadline_ = NextDeadline(deadline_, r.returned, frequency_ / current_.hz);
}
void InputRuntime::End(Result result, bool discardDevice) {
    if (core_) api_.coreDestroy(&core_);
    if (discardDevice) DisposeDevice();
    TraceRecord r; r.kind = TraceKind::Finish; r.result = result; Emit(r);
    if (!result) lastStatus_ = result.code;
    begun_ = false;
    state_ = RuntimeState::Idle;
    CancelWaitableTimer(timer_);
}
void InputRuntime::ArmTimer() {
    if (state_ != RuntimeState::Running) { CancelWaitableTimer(timer_); return; }
    const auto remaining = (std::max)(std::int64_t{1}, deadline_ - QpcNow());
    LARGE_INTEGER due{};
    due.QuadPart = -(std::max)(std::int64_t{1}, remaining * 10000000 / frequency_);
    if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) End({Status::NativeFailure, GetLastError()}, true);
}
void InputRuntime::Run() noexcept {
    try {
        MSG message{}; PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        HANDLE waits[]{stop_, wake_, timer_};
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            if (cancel_.exchange(false)) {
                Command abandoned; while (commands_.Pop(abandoned)) {}
                if (core_ && begun_) {
                    auto r = api_.coreControl(core_, Control::Cancel);
                    if (r) { deadline_ = QpcNow(); Step(deadline_); }
                    else End(r, true);
                } else if (core_) End({}, true);
                else { DisposeDevice(); state_ = RuntimeState::Idle; }
                // This acknowledgement is written after all old commands and contacts are gone.
                TraceRecord ack; ack.kind = TraceKind::Control; ack.detail = static_cast<unsigned>(Control::Cancel); Emit(ack);
            }
            Command command;
            while (!cancel_ && commands_.Pop(command)) {
                if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
                if (command.kind == CommandKind::Start) Begin(command);
                else if (command.kind == CommandKind::StopInertia) {
                    TraceRecord r; r.kind = TraceKind::StopAction; r.received = command.received;
                    r.result = platform_ ? api_.platformStopInertia(platform_, 0) : Result{Status::InvalidState};
                    lastStatus_ = r.result.code; Emit(r);
                } else if (state_ == RuntimeState::Running) {
                    received_ = command.received;
                    // Account for motion before changing direction/hold state.
                    Step(QpcNow());
                    if (state_ != RuntimeState::Running) continue;
                    const auto result = api_.coreControl(core_, command.control);
                    TraceRecord r; r.kind = TraceKind::Control; r.received = command.received;
                    r.detail = static_cast<unsigned>(command.control); r.result = result; Emit(r);
                    if (!result) lastStatus_ = result.code;
                    else if (command.control == Control::Release) { deadline_ = QpcNow(); Step(deadline_); }
                }
            }
            const auto now = QpcNow();
            if (!cancel_ && state_ == RuntimeState::Running && now >= deadline_) Step(now);
            ArmTimer();
            const auto wait = MsgWaitForMultipleObjectsEx(3, waits, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_FAILED) { End({Status::NativeFailure, GetLastError()}, true); break; }
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) SetEvent(stop_);
                else { TranslateMessage(&message); DispatchMessageW(&message); }
            }
        }
        state_ = RuntimeState::Stopping;
        if (core_) End({}, true);
        DisposeDevice();
    } catch (...) {
        lastStatus_ = Status::NativeFailure;
        if (core_) api_.coreDestroy(&core_);
        DisposeDevice();
        TraceRecord r; r.kind = TraceKind::Failure; r.result = {Status::NativeFailure, ERROR_UNHANDLED_EXCEPTION}; Emit(r);
    }
    state_ = RuntimeState::Stopped;
}
}
