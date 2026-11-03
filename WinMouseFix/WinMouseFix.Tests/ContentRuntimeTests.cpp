#include "../WinMouseFix.Host/ContentMode.h"
#include "../WinMouseFix.Host/InputOwner.h"
#include "../WinMouseFix.Host/ContentControls.h"
#include "../WinMouseFix.Host/ContentTarget.h"
#include "../WinMouseFix.Platform.Win32/DeviceSession.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <functional>

namespace {
using namespace wmf;
using namespace wmf::host;
int checks{}, failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
std::uint64_t Now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return static_cast<std::uint64_t>(t.QuadPart); }
bool Wait(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!predicate()) { if (std::chrono::steady_clock::now() >= deadline) return false; Sleep(1); }
    return true;
}
struct EngineFixture;
EngineFixture* engineFixture{};
struct EngineFixture {
    std::atomic<unsigned> creates{},opens{},submits{},destroys{},stops{},completed{};
    unsigned failSubmit{},failDestroy{},sleepMs{},targetChecks{},failTargetAt{};
    bool actionAvailable{true},targetValid{true},failStop{};
    DeviceConfig device;
    std::vector<GestureFrame> frames;
    std::vector<ContentTrace> traces;
    struct Completion { std::uint64_t session; bool click,canceled; };
    std::vector<Completion> completions;
    ContentFunctions Api() {
        engineFixture = this;
        ContentFunctions api;
        api.capabilities = [](Capabilities* out) noexcept -> Result { *out = {}; out->apiBits = RequiredApis | (engineFixture->actionAvailable ? ActionApi : 0); return {}; };
        api.createPlatform = [](PlatformHandle* out) noexcept -> Result { ++engineFixture->creates; *out = reinterpret_cast<PlatformHandle>(engineFixture); return {}; };
        api.openPlatform = [](PlatformHandle p,const DeviceConfig* c) noexcept -> Result { auto& f = *reinterpret_cast<EngineFixture*>(p); ++f.opens; f.device = *c; return {}; };
        api.submit = [](PlatformHandle p,const GestureFrame* frame,MappedFrame* mapped) noexcept -> Result {
            auto& f = *reinterpret_cast<EngineFixture*>(p); ++f.submits; f.frames.push_back(*frame);
            if (f.sleepMs) Sleep(f.sleepMs);
            if (f.submits == f.failSubmit) return {Status::NativeFailure,5};
            return platform::MapContacts(f.device,*frame,*mapped);
        };
        api.stopInertia = [](PlatformHandle p,std::uint32_t) noexcept -> Result { auto& f = *reinterpret_cast<EngineFixture*>(p); ++f.stops; return f.failStop ? Result{Status::NativeFailure,5} : Result{}; };
        api.destroyPlatform = [](PlatformHandle* p) noexcept -> Result {
            auto& f = *reinterpret_cast<EngineFixture*>(*p); ++f.destroys; *p = nullptr;
            return f.failDestroy ? Result{Status::NativeFailure,f.failDestroy} : Result{};
        };
        api.context = this;
        api.trace = [](void* p,const ContentTrace& t) noexcept { static_cast<EngineFixture*>(p)->traces.push_back(t); };
        api.complete = [](void* p,std::uint64_t s,bool click,bool canceled) noexcept {
            auto& f = *static_cast<EngineFixture*>(p); f.completions.push_back({s,click,canceled}); ++f.completed;
        };
        api.targetValid = [](void* p,const MouseInput&) noexcept { auto& f = *static_cast<EngineFixture*>(p); ++f.targetChecks; return f.targetValid && f.targetChecks != f.failTargetAt; };
        return api;
    }
};
struct Events {
    MouseInput e;
    Events() { e.session = 1; e.device = 7; e.target = 9; }
    MouseInput Next(MouseInputKind kind,std::int64_t dx = 0,std::int64_t dy = 0) {
        ++e.sequence; e.kind = kind; e.dx = dx; e.dy = dy; e.firstQpc = e.lastQpc = Now(); return e;
    }
};
void EngineTests() {
    ContentOptions options;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); options.drag.qpcFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
    {
        EngineFixture f; ContentEngine engine(options.drag,options.device,false,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down)));
        VERIFY(engine.Process(e.Next(MouseInputKind::Up)));
        VERIFY(f.submits == 0 && f.completions.back().click);
        ++e.e.session; VERIFY(engine.Process(e.Next(MouseInputKind::Down)));
        VERIFY(engine.Process(e.Next(MouseInputKind::Move,8,4)));
        VERIFY(f.frames.size() == 2 && f.frames[0].phase == Phase::Begin && f.frames[0].offsetXmm == 0);
        VERIFY(engine.Process(e.Next(MouseInputKind::Move)) && f.frames.size() == 2);
        VERIFY(engine.Process(e.Next(MouseInputKind::Up,2,3)));
        VERIFY(f.frames.size() == 4 && f.frames[2].phase == Phase::Update && f.frames[3].phase == Phase::End);
        VERIFY(f.frames[2].offsetXmm == f.frames[3].offsetXmm && f.frames[3].sequence == f.frames[2].sequence+1);
        VERIFY(f.destroys == 0 && !f.completions.back().click);
        ++e.e.session; VERIFY(engine.Process(e.Next(MouseInputKind::Down)));
        VERIFY(f.stops == 1); VERIFY(engine.Cancel(CancelReason::Pause)); VERIFY(f.destroys == 1);
        VERIFY(!engine.Ready() && f.completions.back().canceled);
    }
    {
        EngineFixture f; f.failSubmit = 2; f.failDestroy = 9;
        ContentEngine engine(options.drag,options.device,false,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down)));
        const auto r = engine.Process(e.Next(MouseInputKind::Move,8,0));
        VERIFY(r.nativeError == 5 && r.cleanupError == 9 && !engine.Ready());
        VERIFY(!f.completions.back().click && f.completions.back().canceled);
    }
    {
        EngineFixture f; f.failStop = true; ContentEngine engine(options.drag,options.device,false,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down)));
        VERIFY(engine.Process(e.Next(MouseInputKind::Move,8,0))); VERIFY(engine.Process(e.Next(MouseInputKind::Up)));
        const auto before = f.frames.size(); ++e.e.session;
        VERIFY(engine.Process(e.Next(MouseInputKind::Down)).nativeError == 5);
        VERIFY(f.frames.size() == before && f.completions.back().canceled && !engine.Ready());
    }
    {
        EngineFixture f; ContentEngine engine(options.drag,options.device,false,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down))); f.targetValid = false;
        VERIFY(engine.Process(e.Next(MouseInputKind::Up))); VERIFY(!f.completions.back().click && f.completions.back().canceled);
        VERIFY(f.submits == 0 && !engine.Ready()); // Target cancellation is not an injection failure.
    }
    for (unsigned guardAt : {3u,4u}) {
        EngineFixture f; f.failTargetAt = guardAt;
        ContentEngine engine(options.drag,options.device,false,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down)));
        VERIFY(engine.Process(e.Next(MouseInputKind::Move,8,0)));
        VERIFY(!engine.Ready() && f.completions.back().canceled && !f.completions.back().click);
        if (guardAt == 3) VERIFY(f.frames.empty()); // Never submit Cancel without a native Begin.
        else {
            VERIFY(f.frames.size() == 2 && f.frames[0].phase == Phase::Begin && f.frames[1].phase == Phase::Cancel);
            VERIFY(f.frames[1].sequence == 1 && f.frames[1].offsetXmm == 0); // Actual native position, not rejected Core position.
        }
    }
    {
        EngineFixture f; f.failDestroy = 9; ContentEngine engine(options.drag,options.device,false,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down))); f.targetValid = false;
        VERIFY(engine.Process(e.Next(MouseInputKind::Up)).nativeError == 9); // Genuine cleanup failure still propagates.
    }
    {
        EngineFixture f; f.actionAvailable = false; ContentEngine engine(options.drag,options.device,false,f.Api());
        VERIFY(engine.Initialize().code == Status::Unsupported && f.creates == 0 && f.submits == 0);
    }
    {
        EngineFixture f; ContentEngine engine(options.drag,options.device,true,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down))); VERIFY(engine.Process(e.Next(MouseInputKind::Up)));
        VERIFY(f.creates == 0 && f.submits == 0 && !f.completions.back().click);
        ++e.e.session; VERIFY(engine.Process(e.Next(MouseInputKind::Down))); VERIFY(engine.Process(e.Next(MouseInputKind::Move,30,20)));
        VERIFY(engine.Process(e.Next(MouseInputKind::Up))); VERIFY(f.submits == 0);
    }
    {
        EngineFixture f; ContentEngine engine(options.drag,options.device,false,f.Api()); Events e;
        VERIFY(engine.Initialize()); VERIFY(engine.Process(e.Next(MouseInputKind::Down)));
        VERIFY(engine.Process(e.Next(MouseInputKind::Move,1000000)).code == Status::OutOfRange);
        VERIFY(f.destroys == 1 && !f.completions.back().click);
    }
}
void BufferAndArguments() {
    MouseInput target; target.target = 9; target.anchorX = -1920; target.anchorY = 120;
    TargetSample actual{9,-1920,120,true,true};
    VERIFY(CheckTarget(target,actual) == 0);
    ++actual.x; VERIFY(CheckTarget(target,actual) == AnchorChanged);
    actual.cursorKnown = false; VERIFY(CheckTarget(target,actual) == CursorUnavailable);
    actual = {10,-1920,120,true,true}; VERIFY(CheckTarget(target,actual) == ForegroundChanged);
    actual = {9,-1920,120,false,true}; VERIFY(CheckTarget(target,actual) == TargetMissing);
    ContentInputBuffer buffer; Events e;
    VERIFY(buffer.Push(e.Next(MouseInputKind::Down)));
    const auto first = e.Next(MouseInputKind::Move,1,2); VERIFY(buffer.Push(first));
    VERIFY(buffer.Push(e.Next(MouseInputKind::Move,2,3)));
    const auto reverse = e.Next(MouseInputKind::Move,-1,0); VERIFY(buffer.Push(reverse));
    VERIFY(buffer.Push(e.Next(MouseInputKind::Up)));
    MouseInput output; VERIFY(buffer.Pop(output) && output.kind == MouseInputKind::Down);
    VERIFY(buffer.Pop(output) && output.dx == 3 && output.dy == 5 && output.samples == 2 && output.firstQpc == first.firstQpc);
    VERIFY(buffer.Pop(output) && output.dx == -1 && output.sequence == reverse.sequence);
    VERIFY(buffer.Pop(output) && output.kind == MouseInputKind::Up); VERIFY(!buffer.Pop(output));
    for (unsigned i = 0; i < 512; ++i) VERIFY(buffer.Push(e.Next(MouseInputKind::Move,1)));
    VERIFY(!buffer.Push(e.Next(MouseInputKind::Up))); buffer.Discard(); VERIFY(!buffer.Pop(output));
    const auto parsed = ParseContentArguments({L"--content-drag",L"--threshold-counts",L"4",L"--mm-per-count",L"0.2",L"--y-mm-per-count",L"-0.3",L"--device-width-mm",L"600",L"--capture-only"});
    VERIFY(parsed.options.drag.thresholdCounts == 4 && parsed.options.drag.mmPerCountX == 0.2 && parsed.options.drag.mmPerCountY == -0.3);
    VERIFY(parsed.options.device.widthHimetric == 60000 && parsed.options.captureOnly);
    for (const auto& args : std::vector<std::vector<std::wstring>>{{L"--content-drag",L"--diagnostic"},{L"--content-drag",L"--probe"},
        {L"--content-drag",L"--device-width-mm",L"1001"},{L"--content-drag",L"--mm-per-count",L"nan"},{L"--content-drag",L"--threshold-counts"}}) {
        bool rejected{}; try { ParseContentArguments(args); } catch (...) { rejected = true; } VERIFY(rejected);
    }
    const auto suffix = L".test."+std::to_wstring(GetCurrentProcessId());
    { InputOwner a(suffix),b(suffix); VERIFY(a.Acquire()); VERIFY(b.Acquire().code == Status::Busy); }
    { InputOwner c(suffix); VERIFY(c.Acquire()); }
}
struct RuntimeFixture {
    EngineFixture engine;
    CaptureConfig config;
    std::atomic<unsigned> created{},destroyed{},completions{},plays{},cancellations{};
    bool failCapture{},failCleanup{},emitEvents{},hold{},overflow{};
    DWORD captureThread{},destroyThread{};
};
RuntimeFixture* runtimeFixture{};
ContentRuntimeApi RuntimeApi(RuntimeFixture& f) {
    runtimeFixture = &f; ContentRuntimeApi api; api.engine = f.engine.Api();
    api.createCapture = [](const CaptureConfig* c,CaptureHandle* out) noexcept -> Result {
        auto& f = *runtimeFixture; ++f.created;
        if (f.failCapture) return {Status::NativeFailure,5};
        f.captureThread = GetCurrentThreadId(); f.config = *c; *out = reinterpret_cast<CaptureHandle>(&f); return {};
    };
    api.postCapture = [](CaptureHandle p,CaptureCommand command,std::uint64_t) noexcept -> Result {
        auto& f = *reinterpret_cast<RuntimeFixture*>(p);
        if (command == CaptureCommand::Enable && f.emitEvents && f.plays.fetch_add(1) == 0) {
            Events e;
            auto event = e.Next(MouseInputKind::Down); f.config.sink(f.config.context,&event);
            bool accepted = true;
            for (int i = 0; i < (f.overflow ? 10000 : 200); ++i) { event = e.Next(MouseInputKind::Move,1,1); if (!f.config.sink(f.config.context,&event)) { accepted = false; break; } }
            if (accepted && !f.hold) { event = e.Next(MouseInputKind::Up); f.config.sink(f.config.context,&event); }
        }
        if (command == CaptureCommand::CompleteClick || command == CaptureCommand::CompleteDrag) ++f.completions;
        if (command == CaptureCommand::Cancel) ++f.cancellations;
        return {};
    };
    api.readCapture = [](CaptureHandle,CaptureStats* out) noexcept -> Result { *out = {}; return {}; };
    api.destroyCapture = [](CaptureHandle* p) noexcept -> Result {
        auto& f = *reinterpret_cast<RuntimeFixture*>(*p); ++f.destroyed; f.destroyThread = GetCurrentThreadId();
        if (f.failCleanup && f.destroyed == 1) return {Status::NativeFailure,5};
        *p = nullptr; return {};
    };
    return api;
}
std::string Read(const std::filesystem::path& path) { std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
void RuntimeAndReport() {
    ContentOptions options; LARGE_INTEGER freq{}; QueryPerformanceFrequency(&freq); options.drag.qpcFrequency = static_cast<std::uint64_t>(freq.QuadPart);
    const auto root = DefaultOutputRoot().parent_path()/L"content-drag-validation/automated-tests";
    for (unsigned mode = 0; mode < 6; ++mode) {
        RuntimeFixture f; f.emitEvents = mode == 0 || mode >= 3; f.failCapture = mode == 1; f.failCleanup = mode == 2;
        f.hold = mode == 3; f.overflow = mode == 4; if (f.overflow) f.engine.sleepMs = 2;
        if (mode == 5) f.engine.targetValid = false;
        ContentReport report(root,options,true);
        {
            ContentRuntime runtime(report,options,RuntimeApi(f));
            if (mode == 1 || mode == 4) VERIFY(Wait([&] { return runtime.State() == ContentRuntimeState::Faulted; }));
            else VERIFY(Wait([&] { return runtime.State() == ContentRuntimeState::Enabled; }));
            if (mode == 0) {
                VERIFY(Wait([&] { return f.completions == 1; }));
                runtime.Enable(false); VERIFY(Wait([&] { return runtime.State() == ContentRuntimeState::Paused; }));
                runtime.Enable(true); VERIFY(Wait([&] { return runtime.State() == ContentRuntimeState::Enabled; }));
                VERIFY(f.engine.creates >= 2);
            }
            if (mode == 3) {
                VERIFY(Wait([&] { return f.engine.submits >= 2; }));
                runtime.Enable(false); runtime.Enable(true); // No wait between commands.
                VERIFY(Wait([&] { return runtime.State() == ContentRuntimeState::Enabled && f.engine.creates >= 2; }));
            }
            if (mode == 4) VERIFY(runtime.Failure().code == Status::Busy);
            if (mode == 5) {
                VERIFY(Wait([&] { return f.cancellations >= 1 && runtime.State() == ContentRuntimeState::Enabled; }));
                VERIFY(runtime.Failure() && runtime.Requested());
            }
            runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); }));
            if (mode != 1) VERIFY(f.captureThread == f.destroyThread && f.destroyed >= 1);
        }
        report.Finish(); VERIFY(!report.Failed());
        const auto json = Read(report.Directory()/L"report.json");
        VERIFY(json.find("\"simulation\":true") != std::string::npos && json.find("\"verdict\":\"inconclusive\"") != std::string::npos);
        if (mode == 0) {
            VERIFY(f.engine.frames.size() >= 3 && f.engine.frames.back().phase == Phase::End);
            VERIFY(json.find("\"normalDragEnds\":1") != std::string::npos);
        } else if (mode == 3) VERIFY(!f.engine.frames.empty() && f.engine.frames.back().phase == Phase::Cancel);
        else if (mode == 4) VERIFY(json.find("\"nativeError\":111") != std::string::npos);
        else if (mode == 5) VERIFY(json.find("\"firstFailure\":{\"status\":\"ok\"") != std::string::npos);
        else VERIFY(json.find("\"nativeError\":5") != std::string::npos);
    }
    ContentReport guardReport(root,options,true);
    ContentTrace guard; guard.kind = ContentTraceKind::TargetGuard; guard.input.session = 99;
    guard.input.target = 9; guard.input.anchorX = -1920; guard.input.anchorY = 120;
    guard.guardFlags = AnchorChanged; guard.observedForeground = 9; guard.observedX = -1919; guard.observedY = 120;
    guardReport.Push(guard); guardReport.Finish();
    const auto guardJson = Read(guardReport.Directory()/L"report.json");
    VERIFY(guardJson.find("\"lastTargetGuard\":{\"session\":99,\"flags\":8") != std::string::npos);
    VERIFY(guardJson.find("\"observedCursor\":[-1919,120]") != std::string::npos);
    VERIFY(guardJson.find("\"firstFailure\":{\"status\":\"ok\"") != std::string::npos);
}
int controlFailure{},controlRegisters{},controlRemoves{},sessionRegisters{},sessionRemoves{};
BOOL WINAPI RegisterControl(HWND,int,UINT,UINT) { ++controlRegisters; return controlFailure != 1; }
BOOL WINAPI RemoveControl(HWND,int) { ++controlRemoves; return TRUE; }
BOOL WINAPI RegisterSession(HWND,DWORD) { ++sessionRegisters; return controlFailure != 2; }
BOOL WINAPI RemoveSession(HWND) { ++sessionRemoves; return TRUE; }
DWORD WINAPI ControlError() { return 5; }
void ControlTests() {
    for (int failure : {0,1,2}) {
        controlFailure = failure; controlRegisters = controlRemoves = sessionRegisters = sessionRemoves = 0;
        ContentControls controls(RegisterSession,RemoveSession,ControlError);
        const auto result = controls.Acquire(reinterpret_cast<HWND>(1));
        VERIFY(static_cast<bool>(result) == (failure != 2));
        if (failure != 2) VERIFY(!controls.Acquire(reinterpret_cast<HWND>(1)));
        controls.Release(); controls.Release();
        VERIFY(controlRegisters == 0 && controlRemoves == 0);
        VERIFY(sessionRegisters == 1 && sessionRemoves == (failure == 2 ? 0 : 1));
    }
}
}
int RunContentRuntimeTests(int& total) {
    EngineTests(); BufferAndArguments(); RuntimeAndReport(); ControlTests(); total += checks; return failures;
}
