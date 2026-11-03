#include "../WinMouseFix.Host/InputRuntime.h"
#include "../WinMouseFix.Host/DiagnosticText.h"
#include "../WinMouseFix.Host/DiagnosticHotkeys.h"
#include "../WinMouseFix.Platform.Win32/DeviceSession.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace wmf;
using namespace wmf::host;
int checks{}, failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
std::string Read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
ProbeInfo SimulatedProbe() {
    ProbeInfo p; p.simulation = true; p.modulesReady = true;
    p.caps.apiBits = RequiredApis | ActionApi; p.deviceAttempted = true;
    return p;
}
int registered{}, removed{}, failHotkeyAt{};
BOOL WINAPI RegisterTestHotkey(HWND, int id, UINT flags, UINT key) {
    if (flags != (MOD_CONTROL | MOD_ALT | MOD_NOREPEAT) || key != static_cast<UINT>(VK_F5 + id - 1)) return FALSE;
    if (id == failHotkeyAt) return FALSE;
    ++registered; return TRUE;
}
BOOL WINAPI RemoveTestHotkey(HWND, int) { ++removed; return TRUE; }
DWORD WINAPI HotkeyTestError() { return ERROR_HOTKEY_ALREADY_REGISTERED; }
void HotkeyTests() {
    for (int failure = 1; failure <= 7; ++failure) {
        registered = removed = 0; failHotkeyAt = failure;
        DiagnosticHotkeys keys(RegisterTestHotkey, RemoveTestHotkey, HotkeyTestError);
        DWORD error{};
        VERIFY(!keys.Acquire(nullptr,error) && error == ERROR_HOTKEY_ALREADY_REGISTERED);
        VERIFY(registered == failure - 1 && removed == registered);
        keys.Release(); VERIFY(removed == registered);
    }
    registered = removed = failHotkeyAt = 0;
    DiagnosticHotkeys keys(RegisterTestHotkey, RemoveTestHotkey, HotkeyTestError);
    DWORD error{};
    VERIFY(keys.Acquire(nullptr,error) && registered == 7);
    VERIFY(!keys.Acquire(nullptr,error) && error == ERROR_ALREADY_EXISTS);
    keys.Release(); keys.Release(); VERIFY(removed == 7);
}
void EvidenceAndQueueTests() {
    auto items = BaselineObservations();
    VERIFY(items.size() == 30);
    VERIFY(Summarize(items,false,true) == Verdict::Inconclusive);
    for (auto& o : items) { o.finding = Finding::Pass; o.experiment = 1; o.observer = "test"; o.utc = "test"; o.evidence = "simulation"; }
    VERIFY(Summarize(items,false,true) == Verdict::Go);
    VERIFY(Summarize(items,false,false) == Verdict::Inconclusive);
    items[12].finding = Finding::Fail;
    VERIFY(Summarize(items,false,true) == Verdict::NoGo);
    items[12].evidence.clear();
    VERIFY(Summarize(items,false,true) == Verdict::Inconclusive);
    VERIFY(Summarize(items,true,true) == Verdict::NoGo);
    VERIFY(NextDeadline(100,100,10) == 110);
    VERIFY(NextDeadline(100,1000,10) == 1010);
    SpscRing<unsigned,2> ring;
    VERIFY(ring.Push(1) && ring.Push(2) && !ring.Push(3));
    unsigned value{};
    VERIFY(ring.Pop(value) && value == 1);
    VERIFY(ring.Push(3));
    VERIFY(ring.Pop(value) && value == 2);
    VERIFY(ring.Pop(value) && value == 3 && !ring.Pop(value));
    SpscRing<unsigned,128> concurrent;
    bool ordered = true;
    std::thread producer([&] { for (unsigned i = 0; i < 20000; ++i) while (!concurrent.Push(i)) std::this_thread::yield(); });
    for (unsigned i = 0; i < 20000; ++i) {
        while (!concurrent.Pop(value)) std::this_thread::yield();
        if (value != i) ordered = false;
    }
    producer.join(); VERIFY(ordered);
}
void ReportTests() {
    const auto root = DefaultOutputRoot() / L"automated-tests";
    std::filesystem::path directory;
    {
        ReportWriter writer(root,SimulatedProbe()); directory = writer.Directory();
        writer.RecordDiagnostic("rejected","prepare: missing \"confirmation\"\n",0);
        TraceRecord start; start.kind = TraceKind::Start; start.experiment = 1; start.start.experiment = 1;
        start.start.scenario = Case::PanY; start.start.prepared = 1;
        writer.Push(start);
        TraceRecord frame; frame.kind = TraceKind::Frame; frame.experiment = 1;
        frame.processStart = QpcNow(); frame.submitStart = frame.processStart; frame.returned = frame.processStart + 10;
        writer.Push(frame);
        TraceRecord finish; finish.kind = TraceKind::Finish; finish.experiment = 1; writer.Push(finish);
        Observation observation = BaselineObservations()[0];
        observation.finding = Finding::Pass;
        VERIFY(!writer.Observe(0,observation));
        observation.experiment = 1; observation.observer = "automated test"; observation.utc = UtcNow(); observation.evidence = "recording adapter, not desktop";
        VERIFY(writer.Observe(0,observation));
        VERIFY(!writer.Observe(100,observation));
        writer.Finish(); VERIFY(!writer.Failed());
    }
    const auto json = Read(directory / L"report.json");
    VERIFY(json.find("\"simulation\":true") != std::string::npos);
    VERIFY(json.find("\"verdict\":\"inconclusive\"") != std::string::npos);
    VERIFY(json.find("\"state\":\"pass\"") != std::string::npos);
    VERIFY(json.find("\"state\":\"not-run\"") != std::string::npos);
    VERIFY(json.find("\"diagnosticEvents\":[{\"event\":\"rejected\"") != std::string::npos);
    VERIFY(json.find("prepare: missing \\\"confirmation\\\"\\n") != std::string::npos);
    VERIFY(Read(directory / L"frames.csv").find("process_start_qpc") != std::string::npos);
    {
        ReportWriter writer(root,SimulatedProbe()); directory = writer.Directory();
        TraceRecord record; record.kind = TraceKind::Cleanup; record.result = {Status::NativeFailure,456}; writer.Push(record);
        record.result = {}; writer.Push(record);
        writer.Finish();
    }
    VERIFY(Read(directory / L"report.json").find("\"cleanup\":{\"status\":\"native-failure\",\"nativeError\":456") != std::string::npos);
    {
        auto p = SimulatedProbe(); p.caps.apiBits &= ~CreateApi;
        ReportWriter writer(root,p); directory = writer.Directory(); writer.Finish();
    }
    VERIFY(Read(directory / L"report.json").find("\"verdict\":\"no-go\"") != std::string::npos);
    {
        ReportWriter writer(root,SimulatedProbe(),{0,true}); writer.Finish();
        VERIFY(writer.Failed());
        VERIFY(writer.Summary().find("Evidence incomplete") != std::string::npos);
    }
    {
        ReportWriter writer(root,SimulatedProbe(),{150,false}); directory = writer.Directory();
        TraceRecord record; record.kind = TraceKind::Frame;
        const auto begin = QpcNow();
        for (unsigned i = 0; i < 12000; ++i) writer.Push(record);
        const auto elapsed = QpcNow() - begin;
        LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
        // This observes scheduling, not a hard performance assertion on the machine running CI.
        std::cout << "Slow-writer producer duration: " << elapsed * 1000.0 / f.QuadPart << " ms, lost=" << writer.Lost() << '\n';
        writer.Finish();
        const auto result = Read(directory / L"report.json");
        VERIFY(result.find("\"traceRecordsLost\":" + std::to_string(writer.Lost())) != std::string::npos);
        VERIFY(result.find("\"verdict\":\"go\"") == std::string::npos);
    }
}

struct RuntimeFakeState {
    std::atomic<unsigned> injections{}, creates{}, destroys{}, actions{}, owner{};
    std::atomic<bool> wrongThread{};
    unsigned slowMs{};
};
RuntimeFakeState* fakeState{};
struct RuntimeFakeInstance {
    RuntimeFakeState& state;
    platform::DeviceSession device;
    explicit RuntimeFakeInstance(RuntimeFakeState& s) : state(s), device(Api()) {}
    platform::NativeApi Api() {
        return {this,
            [](void* p,SYNTHETIC_DEVICE_CREATION_PARAMS*) -> HSYNTHETICPOINTERDEVICE {
                auto& s = static_cast<RuntimeFakeInstance*>(p)->state; ++s.creates; s.owner = GetCurrentThreadId();
                return reinterpret_cast<HSYNTHETICPOINTERDEVICE>(static_cast<std::uintptr_t>(1));
            },
            [](void* p,HSYNTHETICPOINTERDEVICE,const POINTER_TYPE_INFO*,UINT32) -> BOOL {
                auto& s = static_cast<RuntimeFakeInstance*>(p)->state;
                if (s.owner != GetCurrentThreadId()) s.wrongThread = true;
                ++s.injections;
                if (s.slowMs) std::this_thread::sleep_for(std::chrono::milliseconds(s.slowMs));
                return TRUE;
            },
            [](void* p,HSYNTHETICPOINTERDEVICE) { ++static_cast<RuntimeFakeInstance*>(p)->state.destroys; },
            [](void* p,HSYNTHETICPOINTERDEVICE,TOUCHPAD_ACTION) -> BOOL { ++static_cast<RuntimeFakeInstance*>(p)->state.actions; return TRUE; },
            [](void*) -> DWORD { return 123; }};
    }
};
RuntimeFunctions FakeFunctions() {
    RuntimeFunctions f;
    f.platformCreate = [](PlatformHandle* out) noexcept -> Result {
        try { *out = reinterpret_cast<PlatformHandle>(new RuntimeFakeInstance(*fakeState)); return {}; }
        catch (...) { return {Status::AllocationFailure}; }
    };
    f.platformOpen = [](PlatformHandle h,const DeviceConfig* c) noexcept { return reinterpret_cast<RuntimeFakeInstance*>(h)->device.Open(*c); };
    f.platformSubmit = [](PlatformHandle h,const GestureFrame* frame,MappedFrame* mapped) noexcept { return reinterpret_cast<RuntimeFakeInstance*>(h)->device.Submit(*frame,*mapped); };
    f.platformStopInertia = [](PlatformHandle h,std::uint32_t end) noexcept { return reinterpret_cast<RuntimeFakeInstance*>(h)->device.StopInertia(end != 0); };
    f.platformClose = [](PlatformHandle h) noexcept { return reinterpret_cast<RuntimeFakeInstance*>(h)->device.Close(); };
    f.platformDestroy = [](PlatformHandle* h) noexcept -> Result {
        if (!*h) return {};
        const auto r = reinterpret_cast<RuntimeFakeInstance*>(*h)->device.Close();
        delete reinterpret_cast<RuntimeFakeInstance*>(*h); *h = nullptr; return r;
    };
    return f;
}
template<class Predicate> bool WaitUntil(Predicate predicate) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return predicate();
}
void RuntimeTests() {
    const auto root = DefaultOutputRoot() / L"automated-runtime-tests";
    RuntimeFakeState s; fakeState = &s;
    ReportWriter writer(root,SimulatedProbe());
    {
        InputRuntime runtime(writer,FakeFunctions());
        StartInfo info;
        VERIFY(runtime.Start(info).code == Status::InvalidArgument);
        VERIFY(s.injections == 0);
        info.prepared = 1; info.trajectory.durationSeconds = 0.5; info.hz = 240;
        VERIFY(runtime.Start(info));
        VERIFY(runtime.Start(info).code == Status::Busy);
        VERIFY(WaitUntil([&] { return runtime.State() == RuntimeState::Idle; }));
        VERIFY(s.injections >= 100 && !s.wrongThread);
        const auto old = s.injections.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        VERIFY(s.injections == old);
        VERIFY(runtime.StopInertia());
        runtime.Cancel();
        VERIFY(WaitUntil([&] { return s.destroys == 1; }));
        runtime.Stop(); runtime.Stop();
        VERIFY(runtime.State() == RuntimeState::Stopped);
    }
    writer.Finish(); VERIFY(!writer.Failed());

    RuntimeFakeState slow; slow.slowMs = 15; fakeState = &slow;
    ReportWriter slowWriter(root,SimulatedProbe(),{100,false});
    {
        InputRuntime runtime(slowWriter,FakeFunctions());
        StartInfo info; info.prepared = 1; info.trajectory.interactive = 1;
        VERIFY(runtime.Start(info));
        VERIFY(WaitUntil([&] { return slow.injections >= 1; }));
        bool sawBusy = false;
        for (int i = 0; i < 200; ++i) if (runtime.ControlMotion(Control::Advance).code == Status::Busy) sawBusy = true;
        VERIFY(sawBusy);
        runtime.Cancel();
        VERIFY(WaitUntil([&] { return runtime.State() == RuntimeState::Idle; }));
        VERIFY(slow.destroys == 1 && !slow.wrongThread);
        const auto stopped = slow.injections.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        VERIFY(slow.injections == stopped);
        VERIFY(runtime.Start(info));
        VERIFY(WaitUntil([&] { return slow.creates == 2; }));
        runtime.Suspend();
        VERIFY(WaitUntil([&] { return runtime.State() == RuntimeState::Idle; }));
        VERIFY(slow.destroys == 2);
        runtime.Stop();
    }
    slowWriter.Finish();
    VERIFY(!slowWriter.Failed());

    for (const unsigned module : {1u,2u}) {
        RuntimeFakeState failing; fakeState = &failing;
        auto functions = FakeFunctions();
        if (module == 1) functions.coreCreate = [](const TrajectoryConfig*,CoreHandle*) noexcept -> Result { return {Status::AllocationFailure}; };
        else functions.platformCreate = [](PlatformHandle*) noexcept -> Result { return {Status::AllocationFailure}; };
        ReportWriter failedWriter(root,SimulatedProbe());
        {
            InputRuntime runtime(failedWriter,functions);
            StartInfo info; info.prepared = 1;
            VERIFY(runtime.Start(info));
            VERIFY(WaitUntil([&] { return runtime.State() == RuntimeState::Idle; }));
            VERIFY(runtime.LastStatus() == Status::AllocationFailure);
            VERIFY(failing.injections == 0 && failing.creates == 0);
            runtime.Stop();
        }
        failedWriter.Finish();
        const auto result = Read(failedWriter.Directory() / L"report.json");
        const auto stage = module == 1 ? "core" : "platform";
        VERIFY(result.find(std::string("\"")+stage+"\":{\"status\":\"allocation-failure\"") != std::string::npos);
        VERIFY(result.find("\"frameSubmission\":{\"status\":\"not-run\"}") != std::string::npos);
    }
}
}

int RunRuntimeTests(int& total) {
    HotkeyTests(); EvidenceAndQueueTests(); ReportTests(); RuntimeTests();
    total += checks;
    return failures;
}
