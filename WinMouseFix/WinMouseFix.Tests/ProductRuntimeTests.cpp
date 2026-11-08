#include "../WinMouseFix.Host/ProductRuntime.h"
#include "../WinMouseFix.Host/ConfigCoordinator.h"
#include "../WinMouseFix.Host/ProductService.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include "../WinMouseFix.Host/CapabilityEvidence.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <vector>
#include <fstream>

namespace {
using namespace wmf;
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
std::uint64_t Now() { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return static_cast<std::uint64_t>(q.QuadPart); }
bool Wait(const std::function<bool()>& predicate) {
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!predicate()) { if (std::chrono::steady_clock::now() > until) return false; Sleep(1); } return true;
}
struct Fixture;
Fixture* fixture{};
struct Fixture {
    ProductCaptureConfig config;
    std::atomic<unsigned> created{},destroyed{},opened{},submitted{},actions{},completed{},canceled{},enables{},updates{};
    std::atomic<std::int64_t> extraWheel{};
    std::atomic<double> radius{};
    std::atomic<bool> held{},owned{},release{},extraMotion{},second{},logicalCanceled{},cancelPending{},foreground{true};
    std::atomic<std::uint64_t> applied{},active{},lastRevision{};
    bool hold{},shell{},failCapture{},failSubmit{},recordOnly{},failDestroy{},noMotion{},zoom{};
    DWORD captureThread{},destroyThread{};
    std::uint64_t epoch{},pressEpoch{},session{},sequence{},pressRevision{};
    Button button{Button::Back};
    std::vector<ContactFrame> frames;
    std::vector<ActionRequest> requests;
    void Emit(InputKind kind,std::int64_t dx = 0,std::int64_t dy = 0,std::int64_t wheel = 0) {
        InputEvent e; e.kind = kind; e.button = button; e.session = session; e.sequence = ++sequence;
        e.firstQpc = e.lastQpc = Now(); e.device = 7; e.target = 9; e.revision = pressRevision; e.generation = pressEpoch; e.dx = dx; e.dy = dy;
        e.wheelY = wheel;
        if (recordOnly) e.flags = RecordedInput;
        config.sink(config.context,&e);
    }
    void Press(bool drag) {
        ++session; sequence = 0; pressRevision = config.bindings.revision; pressEpoch = epoch; active = pressRevision; held = owned = true; logicalCanceled = false;
        button = recordOnly ? Button::Middle : shell ? Button::Forward : Button::Back;
        Emit(InputKind::Down); if (drag) Emit(InputKind::Move,shell ? 0 : 20,shell ? -20 : 4);
        if (zoom) Emit(InputKind::Wheel,0,0,120);
        if (!hold) Release();
    }
    void Release() { if (held.exchange(false)) Emit(InputKind::Up); if (logicalCanceled) { owned = false; active = 0; } }
    ProductRuntimeApi Api() {
        fixture = this; ProductRuntimeApi a; a.simulation = true;
        a.engine.context = this;
        a.engine.capabilities = [](Capabilities* c) noexcept -> Result { c->apiBits = RequiredApis|ActionApi; return {}; };
        a.engine.createPlatform = [](PlatformHandle* out) noexcept -> Result { *out = reinterpret_cast<PlatformHandle>(fixture); return {}; };
        a.engine.openPlatform = [](PlatformHandle,const DeviceConfig*) noexcept -> Result { ++fixture->opened; return {}; };
        a.engine.submit = [](PlatformHandle,const ContactFrame* frame,MappedFrame*) noexcept -> Result {
            auto& f = *fixture;
            if (f.failSubmit && f.submitted == 1) return {Status::NativeFailure,123,456};
            f.frames.push_back(*frame); f.radius = frame->contacts[1].xMm; if (f.shell) f.foreground = false; ++f.submitted; return {};
        };
        a.engine.stopInertia = [](PlatformHandle,std::uint32_t) noexcept -> Result { return {}; };
        a.engine.destroyPlatform = [](PlatformHandle* out) noexcept -> Result { *out = nullptr; return {}; };
        a.engine.execute = [](const ActionRequest* r) noexcept -> Result { fixture->requests.push_back(*r); ++fixture->actions; return {}; };
        a.engine.targetValid = [](void* p,const InputEvent&,TargetPolicy policy,bool) noexcept {
            return policy == TargetPolicy::InputDesktop || static_cast<Fixture*>(p)->foreground.load();
        };
        a.engine.trace = [](void* p,const ProductTrace& t) noexcept { if (t.kind == ProductTraceKind::Input) static_cast<Fixture*>(p)->lastRevision = t.input.revision; };
        a.createCapture = [](const ProductCaptureConfig* c,CaptureHandle* out) noexcept -> Result {
            auto& f = *fixture; if (f.failCapture) return {Status::NativeFailure,5};
            f.config = *c; f.applied = c->bindings.revision; f.captureThread = GetCurrentThreadId(); ++f.created;
            *out = reinterpret_cast<CaptureHandle>(&f); return {};
        };
        a.post = [](CaptureHandle p,CaptureCommand command,std::uint64_t generation) noexcept -> Result {
            auto& f = *reinterpret_cast<Fixture*>(p);
            if (command == CaptureCommand::Enable) {
                f.epoch = generation;
                if (f.enables.fetch_add(1) == 0) f.Press(!f.recordOnly && !f.noMotion);
            }
            if (command == CaptureCommand::Disable || command == CaptureCommand::Cancel) f.cancelPending = true;
            if (command == CaptureCommand::CompleteDrag) { ++f.completed; if (!f.held) { f.owned = false; f.active = 0; } }
            return {};
        };
        a.stats = [](CaptureHandle p,CaptureStats*) noexcept -> Result {
            auto& f = *reinterpret_cast<Fixture*>(p);
            if (f.cancelPending.exchange(false) && f.owned && !f.logicalCanceled.exchange(true)) { f.Emit(InputKind::Cancel); ++f.canceled; if (!f.held) { f.owned = false; f.active = 0; } }
            if (f.extraMotion.exchange(false)) f.Emit(InputKind::Move,2,2);
            if (const auto wheel = f.extraWheel.exchange(0)) f.Emit(InputKind::Wheel,0,0,wheel);
            if (f.release.exchange(false)) f.Release();
            if (f.second && !f.owned) { f.second = false; f.hold = false; f.Press(false); }
            return {};
        };
        a.destroyCapture = [](CaptureHandle* out) noexcept -> Result {
            auto& f = *reinterpret_cast<Fixture*>(*out); if (f.held) return {Status::Busy};
            if (f.failDestroy) return {Status::NativeFailure,321};
            f.destroyThread = GetCurrentThreadId(); ++f.destroyed; *out = nullptr; return {};
        };
        a.update = [](CaptureHandle p,const BindingConfig* c) noexcept -> Result {
            auto& f = *reinterpret_cast<Fixture*>(p); f.config.bindings = *c; f.applied = c->revision; ++f.updates; return {};
        };
        a.policy = [](CaptureHandle p,std::uint64_t,TargetPolicy) noexcept -> Result {
            return reinterpret_cast<Fixture*>(p)->owned ? Result{} : Result{Status::InvalidState};
        };
        a.revision = [](CaptureHandle p,CaptureRevision* out) noexcept -> Result {
            auto& f = *reinterpret_cast<Fixture*>(p); out->applied = f.applied; out->active = f.active; return {};
        };
        a.recording = [](CaptureHandle,std::uint32_t) noexcept -> Result { return {}; };
        return a;
    }
};
void PermanentCleanupFailure() {
    auto config = DefaultBindingConfig(); LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); config.qpcFrequency = frequency.QuadPart;
    Fixture f; f.failDestroy = true;
    ProductRuntime runtime(config,true,f.Api()); VERIFY(Wait([&] { return f.completed >= 1; }));
    runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); }));
    VERIFY(runtime.RequiresProcessExit() && runtime.Failure().nativeError == 321 && runtime.Failure().cleanupError == 321);
}
void DeadlineWakesWithoutMoreInput() {
    auto config = DefaultBindingConfig(); LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); config.qpcFrequency = frequency.QuadPart;
    config.bindingCount = 1; config.bindings[0] = {1,Button::Back,Trigger::Hold,0,Action::Mute}; config.holdMs = 100;
    Fixture f; f.hold = f.noMotion = true; ProductRuntime runtime(config,true,f.Api());
    VERIFY(Wait([&] { return f.actions == 1; })); VERIFY(f.requests.front().binding.action == Action::Mute && f.submitted == 0);
    f.release = true; VERIFY(Wait([&] { return f.completed >= 1; }));
    runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); }) && !runtime.RequiresProcessExit());
    VERIFY(f.actions == 1 && runtime.Failure());
}
void ZoomBoundaryDoesNotFaultRuntime() {
    auto config = DefaultBindingConfig(); LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); config.qpcFrequency = frequency.QuadPart;
    config.bindings[config.bindingCount++] = {5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
    config.wheelMode = WheelMode::High;
    Fixture f; f.hold = f.noMotion = f.zoom = true; ProductRuntime runtime(config,true,f.Api(),NativeZoomValidated);
    VERIFY(Wait([&] { return f.submitted >= 3; })); // Timer emits updates without additional physical input.
    VERIFY(f.config.flags == (WheelSourceValidated|NativeZoomValidated) && f.config.bindings.wheelMode == WheelMode::Off);
    f.extraWheel = -120000; VERIFY(Wait([&] { return f.radius == 2; }));
    VERIFY(runtime.State() == ProductRuntimeState::Enabled && runtime.Failure());
    VERIFY(f.owned && !f.logicalCanceled && f.canceled == 0);
    f.extraWheel = 120; VERIFY(Wait([&] { return f.radius > 2; }));
    VERIFY(f.owned && f.canceled == 0 && runtime.Failure());
    f.release = true; VERIFY(Wait([&] { return !f.owned; }));
    const auto submitted = f.submitted.load(); f.second = true;
    VERIFY(Wait([&] { return f.completed >= 2; }));
    VERIFY(f.submitted > submitted && runtime.State() == ProductRuntimeState::Enabled && runtime.Failure());
    runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); }) && !runtime.RequiresProcessExit());
    VERIFY(f.frames.back().phase == Phase::End && f.requests.empty());
}
void Runtime() {
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); auto config = DefaultBindingConfig(); config.qpcFrequency = frequency.QuadPart;
    for (unsigned mode = 0; mode < 6; ++mode) {
        Fixture f; f.shell = mode == 1; f.hold = mode == 2 || mode == 3; f.failCapture = mode == 4; f.failSubmit = mode == 5;
        ProductRuntime runtime(config,true,f.Api());
        if (mode >= 4) VERIFY(Wait([&] { return runtime.State() == ProductRuntimeState::Faulted; }));
        else VERIFY(Wait([&] { return runtime.State() == ProductRuntimeState::Enabled; }));
        if (mode < 2) VERIFY(Wait([&] { return f.completed >= 1; }));
        if (mode == 2 || mode == 3) {
            VERIFY(Wait([&] { return f.submitted >= 2; }));
            if (mode == 2) {
                auto next = config; next.bindings[0].action = Action::Mute;
                for (std::uint64_t revision = 2; revision <= 65; ++revision) { next.revision = revision; VERIFY(runtime.Publish(next)); }
                VERIFY(Wait([&] { return f.applied == 65; })); f.extraMotion = true;
                VERIFY(Wait([&] { return f.submitted >= 3; })); VERIFY(f.lastRevision == 1 && runtime.ActiveRevision() == 1);
                f.release = true; VERIFY(Wait([&] { return f.completed >= 1; })); f.second = true;
                VERIFY(Wait([&] { return f.actions >= 1; }));
            } else {
                runtime.Enable(false); runtime.Enable(true);
                VERIFY(Wait([&] { return f.canceled >= 1 && runtime.State() == ProductRuntimeState::Enabled; }));
                f.extraMotion = true; f.release = true;
            }
        }
        runtime.Stop(); f.release = true; VERIFY(Wait([&] { return runtime.Finished(); }));
        if (mode != 4) VERIFY(f.destroyed == 1 && f.captureThread == f.destroyThread);
        if (mode == 0 || mode == 1) VERIFY(!f.frames.empty() && f.frames.back().phase == Phase::End && f.requests.empty());
        if (mode == 2) VERIFY(f.requests.size() == 1 && f.requests[0].binding.action == Action::Mute);
        if (mode == 3) VERIFY(!f.frames.empty() && f.frames.back().phase == Phase::Cancel && f.requests.empty());
        if (mode == 5) VERIFY(runtime.Failure().nativeError == 123 && runtime.Failure().cleanupError == 456);
    }
    Fixture f; f.recordOnly = true;
    ProductRuntime paused(config,false,f.Api()); Sleep(20); VERIFY(!f.created && !f.opened); paused.Record(true);
    InputEvent recorded; VERIFY(Wait([&] { return paused.ReadRecorded(recorded); }));
    VERIFY(recorded.button == Button::Middle && (recorded.flags & RecordedInput));
    paused.Record(false); paused.Stop(); VERIFY(Wait([&] { return paused.Finished(); })); VERIFY(f.frames.empty() && f.requests.empty());
    bool refused{}; try { ProductRuntimeApi unsafe; unsafe.simulation = true; ProductRuntime mixed(config,false,unsafe); } catch (...) { refused = true; }
    VERIFY(refused);
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(std::size(executable)));
    const auto repo = std::filesystem::path(executable).parent_path().parent_path().parent_path().parent_path();
    const auto isolated = ProductEnvironment::Simulation(repo);
    refused = false; try { ProductRuntime mixed(isolated,config,false); } catch (...) { refused = true; } VERIFY(refused);
    refused = false; try { ProductRuntime wrong(ProductEnvironment::Production(),config,false,f.Api()); } catch (...) { refused = true; } VERIFY(refused);
    Fixture isolatedFixture;
    ProductRuntime safe(isolated,config,false,isolatedFixture.Api()); safe.Stop(); VERIFY(Wait([&] { return safe.Finished(); }));
    VERIFY(!isolatedFixture.created && !isolatedFixture.opened);
}
struct SlowFiles : ConfigFiles {
    std::string active,backup;
    std::atomic<bool> entered{},allowed{};
    explicit SlowFiles(const ProductConfig& c) : active(SerializeConfig(c)) {}
    ConfigRead Read(bool b) override { const auto text = b ? backup : active; return {{},!text.empty(),text}; }
    Result Save(std::string_view bytes,std::optional<std::string_view> old,bool) override {
        entered = true; while (!allowed) Sleep(1);
        if (old) backup = *old; active = bytes; return {};
    }
};
void SavingDoesNotResumePausedInput() {
    Fixture f; f.hold = true;
    auto config = DefaultProductConfig(false); config.enabled = true;
    auto files = std::make_unique<SlowFiles>(config); auto* disk = files.get(); ConfigStore store(std::move(files)); VERIFY(store.Load());
    ProductRuntime runtime(store.Current()->config.input,true,f.Api()); ConfigCoordinator coordinator(store,runtime);
    VERIFY(Wait([&] { return f.submitted >= 2; }));
    auto changed = store.Current()->config; changed.input.mmPerCountX = 0.25;
    auto save = coordinator.Submit(1,changed); VERIFY(Wait([&] { return disk->entered.load(); }));
    const auto before = Now(); runtime.Enable(false); f.release = true;
    VERIFY(Wait([&] { return runtime.State() == ProductRuntimeState::Paused && f.destroyed >= 1; }));
    VERIFY(Now()-before < config.input.qpcFrequency/2 && !runtime.Requested());
    disk->allowed = true; VERIFY(save.get());
    VERIFY(store.Current()->config.enabled && store.Current()->config.input.revision == 2 && !runtime.Requested());
    auto enable = coordinator.SetEnabled(true); VERIFY(enable.get());
    VERIFY(store.Current()->config.input.revision == 3 && store.Current()->config.input.mmPerCountX == 0.25);
    runtime.Stop(); f.release = true; coordinator.Stop(); VERIFY(Wait([&] { return runtime.Finished(); }));
}
struct Registry {
    std::wstring value; unsigned reads{},writes{},removes{}; bool present{},failWrite{};
    StartupApi Api() {
        return {this,
            [](void* p,std::wstring& value,bool& present) -> Result { auto& r = *static_cast<Registry*>(p); ++r.reads; value = r.value; present = r.present; return {}; },
            [](void* p,const std::wstring& value) -> Result { auto& r = *static_cast<Registry*>(p); ++r.writes; if (r.failWrite) return {Status::NativeFailure,5}; r.value = value; r.present = true; return {}; },
            [](void* p) -> Result { auto& r = *static_cast<Registry*>(p); ++r.removes; r.present = false; r.value.clear(); return {}; },true};
    }
};
ProductEnvironment IsolatedEnvironment() {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(std::size(executable)));
    return ProductEnvironment::Simulation(std::filesystem::path(executable).parent_path().parent_path().parent_path().parent_path());
}
void Startup() {
    auto environment = IsolatedEnvironment(); Registry registry;
    StartupRegistration startup(environment,L"D:/With Spaces/WinMouseFix.Host.exe",registry.Api());
    VERIFY(!registry.writes && !registry.removes); VERIFY(startup.Refresh().result && !startup.Status().registered);
    VERIFY(startup.Apply(true).registered && registry.value == L"\"D:\\With Spaces\\WinMouseFix.Host.exe\" --startup");
    VERIFY(startup.Apply(true).registered && registry.writes == 2); VERIFY(startup.Apply(false).result && !registry.present && registry.removes == 1);
    registry.failWrite = true; const auto failed = startup.Apply(true); VERIFY(!failed.result && failed.result.nativeError == 5 && !failed.registered);
    bool refused{}; try { StartupRegistration unsafe(environment,L"D:/Unsafe.exe"); } catch (...) { refused = true; } VERIFY(refused);
    Registry longRegistry; StartupRegistration longPath(environment,L"D:/"+std::wstring(280,L'x')+L"/Host.exe",longRegistry.Api());
    VERIFY(longPath.Apply(true).result.nativeError == ERROR_FILENAME_EXCED_RANGE && !longRegistry.writes);
}
struct ServiceFiles : ConfigFiles {
    std::string active,backup,preserved;
    ConfigRead Read(bool b) override { const auto text = b ? backup : active; return {{},!text.empty(),text}; }
    Result Save(std::string_view bytes,std::optional<std::string_view> old,bool preserve) override {
        if (preserve) preserved = active; if (old) backup = *old; active = bytes; return {};
    }
};
std::string ServiceRequest(const ProductService& service,std::string_view id,std::string_view command,std::string_view payload = "{}",std::uint64_t expected = 0) {
    using namespace winrt::Windows::Data::Json;
    EnsureJsonApartment(); JsonObject root;
    root.SetNamedValue(L"protocolVersion",JsonValue::CreateNumberValue(1)); root.SetNamedValue(L"requestId",JsonValue::CreateStringValue(winrt::to_hstring(id)));
    root.SetNamedValue(L"hostRunId",JsonValue::CreateStringValue(command == "hello" ? L"" : winrt::to_hstring(service.RunId())));
    root.SetNamedValue(L"command",JsonValue::CreateStringValue(winrt::to_hstring(command))); root.SetNamedValue(L"expectedRevision",JsonValue::CreateNumberValue(static_cast<double>(expected)));
    root.SetNamedValue(L"payload",ParseJsonObject(payload)); return winrt::to_string(root.Stringify());
}
void Service() {
    auto environment = IsolatedEnvironment(); auto files = std::make_unique<ServiceFiles>();
    ConfigStore store(std::move(files)); VERIFY(store.Load()); Fixture hostFixture; hostFixture.recordOnly = true;
    ProductRuntime runtime(environment,store.Current()->config.input,false,hostFixture.Api());
    Registry registry; StartupRegistration startup(environment,L"D:/With Spaces/WinMouseFix.Host.exe",registry.Api());
    std::atomic<std::uint64_t> ticks{1000}; std::atomic<unsigned> settings{},exitRequests{};
    ProductServiceApi api; api.simulation = true; api.capabilities = hostFixture.Api().engine.capabilities; api.context = &ticks;
    Capabilities calibrationCaps; calibrationCaps.apiBits = RequiredApis|ActionApi; CapabilityEvidence calibration(environment,calibrationCaps);
    api.shellObservations = [&](const BindingConfig& config) { return calibration.ShellObservations(config); };
    api.recordShell = [&](const BindingConfig& config,std::string_view payload) { calibration.RecordShell(config,payload); };
    api.ticks = [](void* p) noexcept { return static_cast<std::atomic<std::uint64_t>*>(p)->load(); };
    api.openSettings = [&] { ++settings; }; api.requestExit = [&] { ++exitRequests; };
    unsigned updateChecks{}; std::uint64_t updateAcknowledged{};
    api.checkUpdates = [&] { ++updateChecks; }; api.acknowledgeUpdate = [&](std::uint64_t id) { updateAcknowledged = id; };
    api.updateStatus = [] { return std::string(R"({"state":"current","checkId":1,"completedId":1,"currentVersion":"2.0.0"})"); };
    unsigned downloadCalls{},cancelCalls{},installCalls{}; bool updateBusy{};
    api.downloadUpdate = [&](std::uint64_t id) { ++downloadCalls; return id == 1; };
    api.cancelUpdate = [&] { ++cancelCalls; return true; }; api.installUpdate = [&] { ++installCalls; return true; }; api.updateBusy = [&] { return updateBusy; };
    unsigned relocationCalls{}; bool allowRelocation{};
    api.switchDataLocation = [&](DataLocation target) -> Result { ++relocationCalls; return allowRelocation && target == DataLocation::Program ? Result{} : Result{Status::NativeFailure,ERROR_ACCESS_DENIED}; };
    ProductService service(environment,store,runtime,startup,api);
    auto call = [&](std::string_view id,std::string_view command,std::string_view payload = "{}",std::uint64_t expected = 0,std::uint64_t connection = 1) {
        return ParseJsonObject(service.Handle(ServiceRequest(service,id,command,payload,expected),connection,GetCurrentProcessId()));
    };
    VERIFY(call("hello","hello").GetNamedBoolean(L"ok")); VERIFY(!registry.writes && !hostFixture.created);
    VERIFY(!call("bad-storage","set-data-location",R"({"location":"other"})",1).GetNamedBoolean(L"ok"));
    VERIFY(!call("stale-storage","set-data-location",R"({"location":"program"})",0).GetNamedBoolean(L"ok"));
    VERIFY(call("same-storage","set-data-location",R"({"location":"appdata"})",1).GetNamedBoolean(L"ok") && relocationCalls == 0);
    VERIFY(!call("failed-storage","set-data-location",R"({"location":"program"})",1).GetNamedBoolean(L"ok") && relocationCalls == 1);
    allowRelocation = true; VERIFY(call("switch-storage","set-data-location",R"({"location":"program"})",1).GetNamedBoolean(L"ok"));
    VERIFY(ParseJsonObject(service.StatusJson()).GetNamedObject(L"storage").GetNamedBoolean(L"switching"));
    VERIFY(!call("storage-busy","set-enabled",R"({"enabled":true})").GetNamedBoolean(L"ok"));
    service.DataLocationSwitchFailed({Status::NativeFailure,ERROR_ACCESS_DENIED});
    VERIFY(call("check-updates","check-updates").GetNamedBoolean(L"ok") && updateChecks == 1);
    VERIFY(call("check-updates","check-updates").GetNamedBoolean(L"ok") && updateChecks == 1);
    VERIFY(!call("bad-check","check-updates",R"({"unexpected":true})").GetNamedBoolean(L"ok"));
    VERIFY(call("ack-update","acknowledge-update",R"({"checkId":1})").GetNamedBoolean(L"ok") && updateAcknowledged == 1);
    VERIFY(!call("bad-ack","acknowledge-update",R"({"checkId":"1"})").GetNamedBoolean(L"ok"));
    VERIFY(!call("bad-download","download-update",R"({"checkId":"1"})").GetNamedBoolean(L"ok") && downloadCalls == 0);
    VERIFY(!call("stale-download","download-update",R"({"checkId":99})").GetNamedBoolean(L"ok"));
    VERIFY(call("download","download-update",R"({"checkId":1})").GetNamedBoolean(L"ok"));
    VERIFY(call("download","download-update",R"({"checkId":1})").GetNamedBoolean(L"ok") && downloadCalls == 2);
    VERIFY(call("cancel-download","cancel-update").GetNamedBoolean(L"ok") && cancelCalls == 1);
    VERIFY(call("install-download","install-update").GetNamedBoolean(L"ok") && installCalls == 1);
    VERIFY(!call("bad-install","install-update",R"({"anything":true})").GetNamedBoolean(L"ok"));
    updateBusy = true; VERIFY(!call("update-storage","set-data-location",R"({"location":"program"})",1).GetNamedBoolean(L"ok")); updateBusy = false;
    const auto observation = R"({"action":"task-view","trigger":"pass","continuous":"limited","hold":"pass","reverse":"pass","release":"pass","notes":"Fake protocol calibration, not a real Shell result"})";
    VERIFY(!call("old-calibration","record-shell-observation",observation,0).GetNamedBoolean(L"ok"));
    VERIFY(call("calibration","record-shell-observation",observation,1).GetNamedBoolean(L"ok"));
    VERIFY(ParseJsonObject(service.StatusJson()).GetNamedArray(L"capabilities").GetObjectAt(1).GetNamedString(L"continuous") == L"limited");
    VERIFY(!call("bad-calibration","record-shell-observation","{}",1).GetNamedBoolean(L"ok"));
    auto config = store.Current()->config; config.input.mmPerCountX = -0.25;
    const auto payload = "{\"config\":"+SerializeConfig(config)+"}";
    auto reply = call("save","update-config",payload,1); VERIFY(reply.GetNamedBoolean(L"ok") && store.Current()->config.input.revision == 2);
    VERIFY(call("save","update-config",payload,1).Stringify() == reply.Stringify()); VERIFY(store.Current()->config.input.revision == 2);
    VERIFY(!call("stale","update-config",payload,1).GetNamedBoolean(L"ok"));
    config = store.Current()->config; config.input.wheelMode = WheelMode::High;
    VERIFY(call("wheel-unverified","update-config","{\"config\":"+SerializeConfig(config)+"}",2).GetNamedBoolean(L"ok"));
    VERIFY(store.Current()->config.input.revision == 3);
    config = store.Current()->config; config.startup = true; registry.failWrite = true;
    reply = call("startup-fails","update-config","{\"config\":"+SerializeConfig(config)+"}",3);
    VERIFY(reply.GetNamedBoolean(L"ok") && reply.GetNamedString(L"code") == L"saved-with-startup-error");
    VERIFY(store.Current()->config.startup && !startup.Status().registered && registry.writes == 1);
    VERIFY(!call("bad-boolean","set-enabled","{\"enabled\":\"yes\"}").GetNamedBoolean(L"ok"));
    VERIFY(call("open","open-settings").GetNamedBoolean(L"ok") && settings == 1);
    VERIFY(!call("unknown","execute-arbitrary").GetNamedBoolean(L"ok"));
    VERIFY(call("record","record-start").GetNamedBoolean(L"ok")); VERIFY(service.Recording());
    VERIFY(Wait([&] { service.Tick(); return ParseJsonObject(service.StatusJson()).GetNamedValue(L"recordedButton").ValueType() != winrt::Windows::Data::Json::JsonValueType::Null; }));
    VERIFY(ParseJsonObject(service.StatusJson()).GetNamedObject(L"recordedButton").GetNamedString(L"button") == L"middle");
    service.Disconnected(2); VERIFY(service.Recording()); service.Disconnected(1); VERIFY(!service.Recording());
    VERIFY(call("record-timeout","record-start").GetNamedBoolean(L"ok")); ticks = 31001; service.Tick(); VERIFY(!service.Recording());
    VERIFY(!call("reset-cancel","reset-defaults","{\"confirmed\":false}",4).GetNamedBoolean(L"ok"));
    registry.failWrite = false; VERIFY(call("reset","reset-defaults","{\"confirmed\":true}",4).GetNamedBoolean(L"ok"));
    VERIFY(!store.Current()->config.enabled && !store.Current()->config.startup && registry.removes == 1);
    const auto exported = call("export","export-config").GetNamedObject(L"payload").GetNamedObject(L"config");
    VERIFY(call("import","import-config","{\"config\":"+winrt::to_string(exported.Stringify())+"}",5).GetNamedBoolean(L"ok"));
    VERIFY(store.Current()->config.input.revision == 6 && store.Current()->config.input.bindingCount == 4);
    auto invalid = exported; auto bindings = invalid.GetNamedArray(L"bindings"); auto collision = bindings.GetObjectAt(1);
    collision.SetNamedValue(L"trigger",winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"click"));
    collision.SetNamedValue(L"action",winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"mute"));
    VERIFY(!call("invalid-import","import-config","{\"config\":"+winrt::to_string(invalid.Stringify())+"}",6).GetNamedBoolean(L"ok"));
    VERIFY(store.Current()->config.input.revision == 6);
    const auto golden = environment.executableDirectory.parent_path().parent_path().parent_path()/L"artifacts/product-validation/config-goldens";
    std::filesystem::create_directories(golden);
    std::ofstream(golden/L"service-status.json",std::ios::binary) << service.StatusJson();
    std::ofstream(golden/L"service-reply.json",std::ios::binary) << winrt::to_string(call("golden","get-config").Stringify());
    VERIFY(service.Start());
    HANDLE client = CreateFileW(environment.pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (client == INVALID_HANDLE_VALUE) { WaitNamedPipeW(environment.pipe.c_str(),1000); client = CreateFileW(environment.pipe.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr); }
    VERIFY(client != INVALID_HANDLE_VALUE); const auto stop = CreateEventW(nullptr,TRUE,FALSE,nullptr); std::string response;
    const auto request = ServiceRequest(service,"wire-hello","hello");
    VERIFY(WriteControlFrame(client,stop,request) && ReadControlFrame(client,stop,response)); VERIFY(ParseJsonObject(response).GetNamedBoolean(L"ok"));
    ULONG serverPid{}; VERIFY(GetNamedPipeServerProcessId(client,&serverPid) && serverPid == GetCurrentProcessId());
    CloseHandle(client); CloseHandle(stop);
    VERIFY(call("exit","exit").GetNamedBoolean(L"ok") && service.ExitRequested() && exitRequests == 1);
    VERIFY(!call("late-update","update-config",payload,4).GetNamedBoolean(L"ok"));
    VERIFY(call("exit-ack","exit-ack").GetNamedBoolean(L"ok"));
    VERIFY(Wait([&] { return runtime.Finished(); }) && service.MayClose()); service.Stop();
}
void ZoomOnlyService() {
    const auto environment = IsolatedEnvironment(); ConfigStore store(std::make_unique<ServiceFiles>()); VERIFY(store.Load());
    Fixture f; ProductRuntime runtime(environment,store.Current()->config.input,false,f.Api(),NativeZoomValidated);
    Registry registry; StartupRegistration startup(environment,L"D:/With Spaces/WinMouseFix.Host.exe",registry.Api());
    ProductServiceApi api; api.simulation = true; api.validatedFlags = NativeZoomValidated; api.capabilities = f.Api().engine.capabilities;
    ProductService service(environment,store,runtime,startup,api);
    auto config = store.Current()->config; config.input.bindings[config.input.bindingCount++] = {5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
    auto reply = ParseJsonObject(service.Handle(ServiceRequest(service,"zoom-only","update-config","{\"config\":"+SerializeConfig(config)+"}",1),1,GetCurrentProcessId()));
    VERIFY(reply.GetNamedBoolean(L"ok") && store.Current()->config.input.revision == 2);
    const auto status = ParseJsonObject(service.StatusJson()); bool zoom{},wheel{};
    for (const auto entry : status.GetNamedArray(L"capabilities")) { const auto item = entry.GetObject();
        if (item.GetNamedString(L"name") == L"zoom") zoom = item.GetNamedString(L"state") == L"supported";
        if (item.GetNamedString(L"name") == L"wheel") wheel = item.GetNamedString(L"state") == L"supported";
    }
    VERIFY(zoom && !wheel);
    config = store.Current()->config; config.input.wheelMode = WheelMode::High;
    reply = ParseJsonObject(service.Handle(ServiceRequest(service,"no-global-wheel","update-config","{\"config\":"+SerializeConfig(config)+"}",2),1,GetCurrentProcessId()));
    VERIFY(reply.GetNamedBoolean(L"ok") && store.Current()->config.input.revision == 3 && !registry.writes && !f.created);
    runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); })); service.Stop();
}
void OptionalCapabilities() {
    for (const std::uint32_t flags : {0u,static_cast<unsigned>(WheelSourceValidated),static_cast<unsigned>(NativeZoomValidated),static_cast<unsigned>(WheelSourceValidated|NativeZoomValidated),static_cast<unsigned>(ExplicitValidationProbe)}) {
        auto config=DefaultBindingConfig(); config.wheelMode=WheelMode::High;
        config.bindings[config.bindingCount++]={5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
        Fixture f; f.hold=f.noMotion=true; ProductRuntime runtime(config,true,f.Api(),flags);
        VERIFY(Wait([&] { return f.enables >= 1; }));
        const bool zoom=(flags&(NativeZoomValidated|ExplicitValidationProbe))!=0,wheel=(flags&(WheelSourceValidated|ExplicitValidationProbe))!=0;
        VERIFY(f.config.bindings.bindingCount==(zoom?5u:4u));
        VERIFY(f.config.bindings.wheelMode==(wheel?WheelMode::High:WheelMode::Off));
        VERIFY(config.bindingCount==5 && config.bindings[4].action==Action::NativeZoom && config.wheelMode==WheelMode::High);
        config.revision=2; config.mmPerCountX=0.2; VERIFY(runtime.Publish(config));
        VERIFY(Wait([&] { return f.applied==2; }));
        VERIFY(f.config.bindings.bindingCount==(zoom?5u:4u) && f.config.bindings.wheelMode==(wheel?WheelMode::High:WheelMode::Off));
        f.release=true; VERIFY(Wait([&] { return f.completed>=1; })); runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); }) && runtime.Failure());
    }
}
void FullscreenRuntime() {
    for (unsigned mode=0;mode<4;++mode) {
        auto config=DefaultProductConfig(false).input; Fixture f; f.hold=true;
        if (mode==1) f.shell=true;
        if (mode==2) { f.noMotion=f.zoom=true; config.bindings[config.bindingCount++]={5,Button::Back,Trigger::Wheel,0,Action::NativeZoom}; }
        if (mode==3) f.noMotion=true;
        ProductRuntime runtime(config,true,f.Api(),NativeZoomValidated);
        VERIFY(Wait([&] { return runtime.State()==ProductRuntimeState::Enabled && f.held && (mode==3 || f.submitted>=2); }));
        const auto epoch=runtime.ControlEpoch(),generation=runtime.Generation();
        runtime.SetFullscreenPaused(true);
        VERIFY(runtime.Requested() && runtime.FullscreenPaused() && runtime.State()==ProductRuntimeState::Paused);
        VERIFY(runtime.ControlEpoch()==epoch && runtime.Generation()>generation);
        VERIFY(Wait([&] { return f.canceled>=1; }));
        f.release=true; VERIFY(Wait([&] { return f.destroyed>=1; }));
        if (mode!=3) VERIFY(!f.frames.empty() && f.frames.back().phase==Phase::Cancel);
        VERIFY(f.requests.empty());
        const auto frames=f.submitted.load(),created=f.created.load();
        runtime.Record(true); Sleep(30); VERIFY(f.created==created && f.submitted==frames);
        runtime.SetFullscreenPaused(false); VERIFY(Wait([&] { return runtime.State()==ProductRuntimeState::Enabled && f.created>created; }));
        VERIFY(runtime.Requested() && !runtime.FullscreenPaused() && f.submitted==frames && f.requests.empty());
        runtime.SetFullscreenPaused(true); runtime.Enable(false); runtime.SetFullscreenPaused(false);
        VERIFY(Wait([&] { return f.destroyed>=2; })); VERIFY(!runtime.Requested() && runtime.State()==ProductRuntimeState::Paused);
        runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); }) && runtime.Failure());
        const auto finishedCreated=f.created.load(); runtime.SetFullscreenPaused(true); runtime.SetFullscreenPaused(false); Sleep(10); VERIFY(f.created==finishedCreated);
    }
}
void FullscreenService() {
    auto config=DefaultProductConfig(false); config.disableInFullscreen=true;
    auto disk=std::make_unique<ServiceFiles>(); disk->active=SerializeConfig(config); ConfigStore store(std::move(disk)); VERIFY(store.Load());
    const auto environment=IsolatedEnvironment(); Fixture f; f.hold=true; ProductRuntime runtime(environment,config.input,false,f.Api());
    runtime.SetFullscreenPaused(true);
    Registry registry; StartupRegistration startup(environment,L"D:/With Spaces/WinMouseFix.Host.exe",registry.Api());
    ProductServiceApi api; api.simulation=true; api.capabilities=f.Api().engine.capabilities; api.fullscreenDetected=[] { return true; };
    ProductService service(environment,store,runtime,startup,api);
    auto reply=ParseJsonObject(service.Handle(ServiceRequest(service,"fullscreen-enable","set-enabled",R"({"enabled":true})"),1,GetCurrentProcessId()));
    VERIFY(reply.GetNamedBoolean(L"ok") && runtime.Requested() && runtime.FullscreenPaused() && !f.created);
    VERIFY(store.Current()->config.enabled && store.Current()->config.disableInFullscreen);
    auto status=ParseJsonObject(service.StatusJson()); VERIFY(status.GetNamedBoolean(L"fullscreenPaused") && status.GetNamedBoolean(L"fullscreenDetected"));
    VERIFY(status.GetNamedBoolean(L"requested") && status.GetNamedString(L"state")==L"paused");
    reply=ParseJsonObject(service.Handle(ServiceRequest(service,"fullscreen-record","record-start"),1,GetCurrentProcessId()));
    VERIFY(!reply.GetNamedBoolean(L"ok") && reply.GetNamedString(L"code")==L"fullscreen-paused" && !service.Recording());
    auto changed=store.Current()->config; changed.input.mmPerCountX=0.3;
    reply=ParseJsonObject(service.Handle(ServiceRequest(service,"fullscreen-save","update-config","{\"config\":"+SerializeConfig(changed)+"}",changed.input.revision),1,GetCurrentProcessId()));
    VERIFY(reply.GetNamedBoolean(L"ok") && runtime.FullscreenPaused() && runtime.Requested() && !f.created);
    service.SetEnabled(false).get(); runtime.SetFullscreenPaused(false); Sleep(20); VERIFY(!runtime.Requested() && !f.created);
    runtime.ReportFailure({Status::NativeFailure,321}); runtime.SetFullscreenPaused(true); runtime.SetFullscreenPaused(false); VERIFY(!runtime.Failure() && !f.created);
    runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); })); service.Stop();
}
void ExpiredZoomDoesNotBlockEnable() {
    auto config=DefaultProductConfig(false); config.input.bindings[config.input.bindingCount++]={5,Button::Back,Trigger::Wheel,0,Action::NativeZoom};
    auto disk=std::make_unique<ServiceFiles>(); disk->active=SerializeConfig(config); ConfigStore store(std::move(disk)); VERIFY(store.Load());
    const auto environment=IsolatedEnvironment(); Fixture f; f.hold=true; ProductRuntime runtime(environment,store.Current()->config.input,false,f.Api());
    Registry registry; StartupRegistration startup(environment,L"D:/With Spaces/WinMouseFix.Host.exe",registry.Api());
    ProductServiceApi api; api.simulation=true; api.capabilities=f.Api().engine.capabilities; ProductService service(environment,store,runtime,startup,api);
    const auto reply=ParseJsonObject(service.Handle(ServiceRequest(service,"enable-existing-zoom","set-enabled",R"({"enabled":true})"),1,GetCurrentProcessId()));
    VERIFY(reply.GetNamedBoolean(L"ok")); VERIFY(Wait([&] { return f.submitted>=2; }));
    VERIFY(runtime.Requested() && runtime.State()==ProductRuntimeState::Enabled && runtime.Failure());
    VERIFY(store.Current()->config.enabled && store.Current()->config.input.bindingCount==5 && store.Current()->config.input.bindings[4].action==Action::NativeZoom);
    VERIFY(f.config.bindings.bindingCount==4 && f.config.bindings.wheelMode==WheelMode::Off && f.config.flags==0);
    const auto status=ParseJsonObject(service.StatusJson()); bool unverified{};
    for (const auto& entry : status.GetNamedArray(L"capabilities")) { const auto item=entry.GetObject(); if(item.GetNamedString(L"name")==L"zoom") unverified=item.GetNamedString(L"state")==L"unverified"; }
    VERIFY(unverified); f.release=true; VERIFY(Wait([&] { return f.completed>=1; }));
    auto changed=store.Current()->config; changed.input.mmPerCountX=0.2;
    const auto saved=ParseJsonObject(service.Handle(ServiceRequest(service,"save-existing-zoom","update-config","{\"config\":"+SerializeConfig(changed)+"}",changed.input.revision),1,GetCurrentProcessId()));
    VERIFY(saved.GetNamedBoolean(L"ok") && store.Current()->config.input.bindingCount==5); VERIFY(Wait([&] { return f.applied==store.Current()->config.input.revision; }));
    VERIFY(f.config.bindings.bindingCount==4);
    runtime.Stop(); VERIFY(Wait([&] { return runtime.Finished(); })); service.Stop();
}
}
int RunProductRuntimeTests(int& total) { Runtime(); PermanentCleanupFailure(); DeadlineWakesWithoutMoreInput(); ZoomBoundaryDoesNotFaultRuntime(); SavingDoesNotResumePausedInput(); Startup(); Service(); ZoomOnlyService(); OptionalCapabilities(); ExpiredZoomDoesNotBlockEnable(); FullscreenRuntime(); FullscreenService(); total += checks; return failures; }
