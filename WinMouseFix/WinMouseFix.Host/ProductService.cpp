#include "ProductService.h"
#include "JsonSupport.h"
#include "ProductVersion.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace wmf::host {
namespace {
using namespace winrt::Windows::Data::Json;
void Text(JsonObject& o,const wchar_t* name,std::string_view text) { o.SetNamedValue(name,JsonValue::CreateStringValue(winrt::to_hstring(text))); }
void Number(JsonObject& o,const wchar_t* name,std::uint64_t value) { o.SetNamedValue(name,JsonValue::CreateNumberValue(static_cast<double>(value))); }
void Boolean(JsonObject& o,const wchar_t* name,bool value) { o.SetNamedValue(name,JsonValue::CreateBooleanValue(value)); }
std::string NewRunId() { FILETIME time{}; GetSystemTimeAsFileTime(&time); const auto value = (static_cast<std::uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime; return std::to_string(GetCurrentProcessId())+"-"+std::to_string(value); }
ProtocolReply Rejected(const char* code,const char* message) { ProtocolReply reply; reply.ok = false; reply.code = code; reply.message = message; return reply; }
void EmptyPayload(const JsonObject& object) { if (object.Size()) throw std::invalid_argument("Unexpected command payload"); }
}
ProductService::ProductService(ProductEnvironment environment,ConfigStore& store,ProductRuntime& runtime,StartupRegistration& startup,ProductServiceApi api)
    : environment_(std::move(environment)),store_(store),runtime_(runtime),startup_(startup),api_(std::move(api)),coordinator_(store,runtime),
      protocol_(NewRunId(),[this](const ProtocolRequest& request) { return Dispatch(request); }) {
    if (!api_.capabilities || api_.simulation != environment_.simulation || (environment_.simulation && api_.capabilities == WmfPlatformCapabilities))
        throw std::invalid_argument("Product service backend does not match its isolation environment");
    capabilityQuery_ = api_.capabilities(&capabilities_); startup_.Refresh();
}
ProductService::~ProductService() { Stop(); }
Result ProductService::Start() {
    if (pipe_) return {Status::InvalidState};
    pipe_ = std::make_unique<ControlPipe>(environment_,[this](std::string_view text,std::uint64_t connection,DWORD pid) {
        return Handle(text,connection,pid);
    },[this](std::uint64_t connection) { Disconnected(connection); });
    return pipe_->Start();
}
void ProductService::Stop() noexcept {
    EndRecording(); coordinator_.Stop();
    if (pipe_) { pipe_->Stop(); pipe_->Join(); pipe_.reset(); }
}
std::uint64_t ProductService::Now() const noexcept { return api_.ticks ? api_.ticks(api_.context) : GetTickCount64(); }
void ProductService::EndRecording() noexcept {
    std::lock_guard lock(leaseMutex_);
    if (leaseOwner_) { leaseOwner_ = leaseUntil_ = leaseGeneration_ = 0; runtime_.Record(false); }
}
void ProductService::Disconnected(std::uint64_t connection) noexcept {
    std::lock_guard lock(leaseMutex_);
    if (leaseOwner_ == connection) { leaseOwner_ = leaseUntil_ = leaseGeneration_ = 0; runtime_.Record(false); }
}
void ProductService::Tick() {
    std::lock_guard lock(leaseMutex_);
    if (leaseOwner_ && (Now() >= leaseUntil_ || !runtime_.Failure())) { leaseOwner_ = leaseUntil_ = leaseGeneration_ = 0; runtime_.Record(false); }
    InputEvent event;
    while (runtime_.ReadRecorded(event)) if (leaseOwner_ && event.generation == leaseGeneration_ && (event.flags & RecordedInput)) { recorded_ = event; ++recordedSerial_; }
}
bool ProductService::Recording() const noexcept { std::lock_guard lock(leaseMutex_); return leaseOwner_ != 0; }
std::future<ConfigError> ProductService::SetEnabled(bool value) {
    EndRecording();
    if (value) { const auto gate = Gate(store_.Current()->config); if (!gate) { std::promise<ConfigError> promise; auto future = promise.get_future(); promise.set_value(gate); return future; } }
    return coordinator_.SetEnabled(value);
}
void ProductService::BeginExit(bool client) noexcept {
    if (!exiting_.exchange(true)) {
        exitUntil_ = client ? Now()+3000 : Now(); EndRecording(); coordinator_.Stop(); runtime_.Stop();
        if (api_.requestExit) { try { api_.requestExit(); } catch (...) {} }
    }
}
bool ProductService::MayClose() const noexcept { return exiting_ && runtime_.Finished() && (exitAcked_ || Now() >= exitUntil_); }
std::string ProductService::Handle(std::string_view text,std::uint64_t connection,DWORD pid) { return protocol_.Handle(text,connection,pid); }
ConfigError ProductService::Gate(const ProductConfig& config) const {
    // Unverified features are excluded from the runtime snapshot, without
    // rejecting the saved configuration or the application's enable switch.
    return ValidateConfig(config);
}
std::string ProductService::StatusJson() {
    EnsureJsonApartment(); JsonObject status;
    Number(status,L"version",++statusSerial_);
    Text(status,L"productVersion",ProductVersion); Number(status,L"abiVersion",AbiVersion); Text(status,L"architecture","x64"); Text(status,L"nativeSdk","10.0.28000.0");
    if (api_.updateStatus) status.SetNamedValue(L"update",ParseJsonObject(api_.updateStatus()));
    JsonObject storage; Text(storage,L"location",DataLocationName(environment_.dataLocation)); Text(storage,L"directory",winrt::to_string(environment_.dataDirectory.wstring()));
    Boolean(storage,L"switching",storageSwitchPending_); Number(storage,L"nativeError",storageError_); status.SetNamedValue(L"storage",storage);
    JsonObject os; Number(os,L"major",capabilities_.osMajor); Number(os,L"minor",capabilities_.osMinor); Number(os,L"build",capabilities_.osBuild); Number(os,L"revision",capabilities_.osRevision); Boolean(os,L"revisionKnown",capabilities_.revisionKnown != 0); status.SetNamedValue(L"os",os);
    constexpr const char* states[]{"paused","starting","enabled","faulted","stopping","stopped"};
    Text(status,L"state",states[static_cast<unsigned>(runtime_.State())]); Boolean(status,L"requested",runtime_.Requested());
    const auto committed = store_.Current(); Number(status,L"configRevision",committed->config.input.revision); Number(status,L"activeRevision",runtime_.ActiveRevision());
    Boolean(status,L"recoveryRequired",committed->recoveryRequired); Text(status,L"recoveryReason",committed->warning.message);
    Number(status,L"lostTraceRecords",runtime_.LostTraces()); Boolean(status,L"exitRequested",exiting_);
    Number(status,L"logError",api_.logError ? api_.logError() : 0);
    const auto startup = startup_.Status(); Boolean(status,L"startupRegistered",startup.registered); Number(status,L"startupError",startup.result.nativeError);
    const auto failed = runtime_.Failure(); JsonObject failure; Number(failure,L"status",static_cast<unsigned>(failed.code));
    Number(failure,L"nativeError",failed.nativeError); Number(failure,L"cleanupError",failed.cleanupError); status.SetNamedValue(L"failure",failure);
    const auto stats = runtime_.Stats(); JsonObject capture;
    Number(capture,L"rawPackets",stats.rawPackets); Number(capture,L"rawWheels",stats.rawWheels); Number(capture,L"blockedWheels",stats.blockedWheels);
    Number(capture,L"injectedWheelHooks",stats.injectedWheelHooks); Number(capture,L"keyboardTakeovers",stats.keyboardTakeovers);
    Number(capture,L"hookCalls",stats.hookCalls); Number(capture,L"hookTicks",stats.hookTicks); Number(capture,L"hookMaxTicks",stats.hookMaxTicks); status.SetNamedValue(L"capture",capture);
    {
        std::lock_guard lock(leaseMutex_); Boolean(status,L"recording",leaseOwner_ != 0);
        const auto now = Now(); Number(status,L"recordingRemainingMilliseconds",leaseOwner_ && leaseUntil_ > now ? leaseUntil_-now : 0);
        if (leaseOwner_ && recordedSerial_) {
            JsonObject button; Text(button,L"button",ButtonName(recorded_.button)); Number(button,L"modifiers",recorded_.modifiers); Number(button,L"serial",recordedSerial_); status.SetNamedValue(L"recordedButton",button);
        } else status.SetNamedValue(L"recordedButton",JsonValue::CreateNullValue());
    }
    JsonArray capabilities;
    const auto shellResults = ParseJsonObject(api_.shellObservations ? api_.shellObservations(committed->config.input) : "{}");
    const bool native = capabilityQuery_ && (capabilities_.apiBits & (RequiredApis|ActionApi)) == (RequiredApis|ActionApi);
    for (const auto name : {"content-drag","task-view","show-desktop","desktop-left","desktop-right","wheel","zoom"}) {
        JsonObject item; Text(item,L"name",name);
        const std::string_view feature(name);
        const bool validated = feature == "wheel" ? (api_.validatedFlags & WheelSourceValidated) != 0 : feature == "zoom" && (api_.validatedFlags & NativeZoomValidated) != 0;
        Text(item,L"state",!native ? "unavailable" : validated ? "supported" : "unverified");
        Text(item,L"reason",!native ? "Required native touchpad APIs are unavailable" : validated ? "Explicit local source/target validation passed for this OS patch and engine build; other applications still need observation" : feature == "wheel" ? "Bounded wheel planner is implemented; source isolation and real target response await explicit validation" :
            feature == "zoom" ? "Independent pinch geometry is implemented; real target response is unverified" :
            feature == "task-view" ? "Diagnostic triggering succeeded; progressive expansion was limited. The real side-button path needs confirmation" : "The new configurable side-button path requires confirmation");
        Text(item,L"trigger","not-run"); Text(item,L"continuous",feature == "task-view" ? "limited" : "not-run");
        Text(item,L"hold","not-run"); Text(item,L"reverse","not-run"); Text(item,L"release","not-run");
        Text(item,L"evidence",feature == "task-view" ? "docs/native-gesture-validation.md; fixed-trajectory observation only" : "");
        if (shellResults.HasKey(winrt::to_hstring(name))) {
            const auto observation = shellResults.GetNamedObject(winrt::to_hstring(name));
            for (const auto field : {L"trigger",L"continuous",L"hold",L"reverse",L"release"}) item.SetNamedValue(field,observation.GetNamedValue(field));
            Text(item,L"state",native && observation.GetNamedString(L"trigger") == L"pass" ? "supported" : native ? "unverified" : "unavailable");
            Text(item,L"reason","Explicit local observation; continuous animation is reported independently; changing the OS, engine or gesture mapping invalidates this record");
            Text(item,L"evidence",winrt::to_string(observation.Stringify()));
        }
        capabilities.Append(item);
    }
    status.SetNamedValue(L"capabilities",capabilities); return winrt::to_string(status.Stringify());
}
ProtocolReply ProductService::StatusResponse() {
    ProtocolReply reply; const auto state = store_.Current(); reply.revision = state->config.input.revision; reply.activeRevision = runtime_.ActiveRevision();
    JsonObject payload; payload.SetNamedValue(L"status",ParseJsonObject(StatusJson())); reply.payload = winrt::to_string(payload.Stringify()); return reply;
}
ProtocolReply ProductService::ConfigResponse(ConfigError error) {
    auto reply = StatusResponse(); const auto state = store_.Current(); auto payload = ParseJsonObject(reply.payload);
    payload.SetNamedValue(L"config",ParseJsonObject(SerializeConfig(state->config))); reply.payload = winrt::to_string(payload.Stringify());
    if (!error) {
        reply.ok = false; reply.code = error.problem == ConfigProblem::Conflict ? "revision-conflict" : error.problem == ConfigProblem::RecoveryRequired ? "recovery-required" :
            error.problem == ConfigProblem::Storage ? "storage-error" : "configuration-rejected";
        reply.message = error.message; reply.field = error.field; reply.bindingId = error.bindingId; reply.conflictingId = error.conflictingId;
        reply.nativeError = error.result.nativeError; reply.cleanupError = error.result.cleanupError;
    }
    return reply;
}
ProtocolReply ProductService::Dispatch(const ProtocolRequest& request) {
    try { auto reply = DispatchInner(request); if (api_.observe) api_.observe(request.command,reply); return reply; }
    catch (const winrt::hresult_error&) { return Rejected("bad-request","Invalid command payload field or type"); }
    catch (const std::invalid_argument&) { return Rejected("bad-request","Invalid command payload"); }
    catch (const std::out_of_range&) { return Rejected("bad-request","Command field out of range"); }
}
ProtocolReply ProductService::DispatchInner(const ProtocolRequest& request) {
    const auto payload = ParseJsonObject(request.payload);
    if (request.command == "hello") {
        if (payload.Size() > 1 || (payload.Size() && !payload.HasKey(L"window"))) return Rejected("bad-request","Unexpected handshake fields");
        if (payload.HasKey(L"window") && api_.settingsWindow) {
            const auto text = winrt::to_string(payload.GetNamedString(L"window")); std::size_t parsed{}; const auto handle = std::stoull(text,&parsed);
            const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle)); DWORD owner{};
            if (parsed != text.size() || !request.clientPid || !api_.isSettingsProcess || !api_.isSettingsProcess(request.clientPid) ||
                (!environment_.simulation && (!IsWindow(window) || !GetWindowThreadProcessId(window,&owner) || owner != request.clientPid)))
                return Rejected("bad-request","Settings window does not belong to the connected process");
            api_.settingsWindow(request.clientPid,handle);
        }
        auto reply = StatusResponse(); auto body = ParseJsonObject(reply.payload); Number(body,L"hostPid",GetCurrentProcessId());
        Text(body,L"productVersion",ProductVersion); Boolean(body,L"simulation",environment_.simulation); Text(body,L"scope",winrt::to_string(environment_.scope));
        reply.payload = winrt::to_string(body.Stringify()); return reply;
    }
    if (request.command == "get-state" || request.command == "subscribe") {
        EmptyPayload(payload); if (request.command == "subscribe" && !exiting_) Sleep(200); return StatusResponse();
    }
    if (request.command == "get-config" || request.command == "export-config") { EmptyPayload(payload); return ConfigResponse(); }
    if (request.command == "exit") { EmptyPayload(payload); BeginExit(true); return StatusResponse(); }
    if (request.command == "exit-ack") { EmptyPayload(payload); exitAcked_ = true; return StatusResponse(); }
    if (exiting_) return Rejected("closing","Host is stopping");
    if (request.command == "set-data-location") {
        if (api_.updateBusy && api_.updateBusy()) return Rejected("busy","An update is in progress");
        if (payload.Size() != 1 || !payload.HasKey(L"location")) return Rejected("bad-request","Expected data location");
        const auto name = payload.GetNamedString(L"location");
        if (name != L"appdata" && name != L"program") return Rejected("bad-request","Unknown data location");
        const auto current = store_.Current();
        if (current->recoveryRequired) return Rejected("recovery-required","Resolve configuration recovery before moving data");
        if (request.expectedRevision != current->config.input.revision) return Rejected("revision-conflict","Reload before moving data");
        const auto target = name == L"program" ? DataLocation::Program : DataLocation::AppData;
        if (target == environment_.dataLocation) return StatusResponse();
        if (!api_.switchDataLocation) return Rejected("not-ready","Data relocation is unavailable");
        if (storageSwitchPending_.exchange(true)) return Rejected("busy","Data relocation is already pending");
        storageError_ = 0; const auto result = api_.switchDataLocation(target);
        if (!result) { DataLocationSwitchFailed(result); return Rejected("storage-error","Could not start configuration relocation"); }
        auto reply = StatusResponse(); reply.code = "storage-restarting"; return reply;
    }
    if (storageSwitchPending_) return Rejected("busy","Configuration relocation is restarting the Host");
    if (request.command == "check-updates") { EmptyPayload(payload); if (!api_.checkUpdates) return Rejected("not-ready","Update service is unavailable"); api_.checkUpdates(); return StatusResponse(); }
    if (request.command == "download-update") {
        if (payload.Size() != 1 || !payload.HasKey(L"checkId")) return Rejected("bad-request","Expected update check identity");
        if (store_.Current()->recoveryRequired || !api_.downloadUpdate || !api_.downloadUpdate(JsonUnsigned(payload,L"checkId"))) return Rejected("not-ready","Update is unavailable or no longer current"); return StatusResponse();
    }
    if (request.command == "cancel-update" || request.command == "install-update") {
        EmptyPayload(payload); auto& action = request.command == "cancel-update" ? api_.cancelUpdate : api_.installUpdate;
        if (!action || !action()) return Rejected("not-ready","Update is not ready for this action"); return StatusResponse();
    }
    if (request.command == "acknowledge-update") {
        if (payload.Size() != 1 || !payload.HasKey(L"checkId")) return Rejected("bad-request","Expected update check identity");
        if (api_.acknowledgeUpdate) api_.acknowledgeUpdate(JsonUnsigned(payload,L"checkId")); return StatusResponse();
    }
    if (request.command == "open-settings") { EmptyPayload(payload); if (api_.openSettings) api_.openSettings(); return StatusResponse(); }
    if (request.command == "settings-ready") {
        EmptyPayload(payload); if (!api_.isSettingsProcess || !api_.isSettingsProcess(request.clientPid)) return Rejected("bad-request","Settings process identity required");
        if (api_.settingsReady) api_.settingsReady(request.clientPid); return StatusResponse();
    }
    if (api_.updateStatus && ParseJsonObject(api_.updateStatus()).GetNamedString(L"state",L"") == L"installing") return Rejected("busy","The update is restarting Win Mouse Fix");
    if (request.command == "record-start") {
        EmptyPayload(payload);
        {
            std::lock_guard lock(leaseMutex_);
            if (leaseOwner_ && leaseOwner_ != request.connection) return Rejected("busy","Another connection owns button identification");
            if (!runtime_.Failure()) return Rejected("runtime-fault","Restart the Host before recording after a native failure");
            leaseOwner_ = request.connection; leaseUntil_ = Now()+30000; recordedSerial_ = 0; recorded_ = {}; runtime_.Record(true); leaseGeneration_ = runtime_.Generation();
        }
        auto reply = StatusResponse(); auto lease = ParseJsonObject(reply.payload); Number(lease,L"leaseMilliseconds",30000); reply.payload = winrt::to_string(lease.Stringify()); return reply;
    }
    if (request.command == "record-stop") {
        EmptyPayload(payload);
        { std::lock_guard lock(leaseMutex_); if (leaseOwner_ && leaseOwner_ != request.connection) return Rejected("busy","Recording lease belongs to a different connection"); }
        EndRecording(); return StatusResponse();
    }
    if (request.command == "record-shell-observation") {
        if (!api_.recordShell) return Rejected("not-ready","Calibration recorder is unavailable");
        const auto current = store_.Current(); if (request.expectedRevision != current->config.input.revision) return Rejected("revision-conflict","Reload before recording observations");
        api_.recordShell(current->config.input,request.payload); return StatusResponse();
    }
    if (request.command == "set-enabled") {
        if (payload.Size() != 1 || !payload.HasKey(L"enabled")) return Rejected("bad-request","Expected enabled boolean");
        const auto value = payload.GetNamedBoolean(L"enabled");
        if (value && (store_.Current()->recoveryRequired || !runtime_.Failure())) return Rejected("runtime-not-ready","Resolve configuration recovery or restart the faulted Host before enabling");
        const auto result = SetEnabled(value).get(); return ConfigResponse(result);
    }
    if (request.command == "set-startup") {
        if (payload.Size() != 1 || !payload.HasKey(L"enabled")) return Rejected("bad-request","Expected startup enabled boolean");
        const auto value = payload.GetNamedBoolean(L"enabled"); auto candidate = store_.Current()->config;
        if (request.expectedRevision != candidate.input.revision) return ConfigResponse({ConfigProblem::Conflict,"revision","Reload current configuration"});
        if (candidate.startup != value) { candidate.startup = value; const auto saved = coordinator_.Submit(request.expectedRevision,candidate).get(); if (!saved) return ConfigResponse(saved); }
        const auto actual = startup_.Apply(value); auto reply = ConfigResponse();
        if (!actual.result) { reply.code = "saved-with-startup-error"; reply.message = "Login startup registration failed"; reply.nativeError = actual.result.nativeError; }
        return reply;
    }
    if (request.command == "start-trace") {
        EmptyPayload(payload); if (!api_.startTrace) return Rejected("not-ready","Detailed logger is not connected"); api_.startTrace(); return StatusResponse();
    }
    if (request.command == "update-config" || request.command == "import-config" || request.command == "reset-defaults") {
        ProductConfig candidate;
        bool recovery{},forceEnable{};
        if (request.command == "reset-defaults") {
            if (payload.Size() != 1 || !payload.HasKey(L"confirmed") || !payload.GetNamedBoolean(L"confirmed")) return Rejected("confirmation-required","Confirm replacement of all settings and bindings");
            candidate = DefaultProductConfig(); recovery = store_.Current()->recoveryRequired; forceEnable = true;
        } else {
            if (payload.Size() != 1 || !payload.HasKey(L"config")) return Rejected("bad-request","Expected complete configuration");
            const auto parsed = ParseConfig(winrt::to_string(payload.GetNamedObject(L"config").Stringify()),candidate);
            if (!parsed) return ConfigResponse(parsed);
            forceEnable = request.command == "import-config";
        }
        const auto gate = Gate(candidate); if (!gate) return ConfigResponse(gate);
        const auto previousStartup = store_.Current()->config.startup;
        const auto saved = coordinator_.Submit(request.expectedRevision,candidate,recovery,forceEnable).get();
        auto reply = ConfigResponse(saved);
        if (saved && candidate.startup != previousStartup) {
            const auto actual = startup_.Apply(candidate.startup); reply = ConfigResponse();
            if (!actual.result) { reply.code = "saved-with-startup-error"; reply.message = "Configuration saved; login startup registration failed"; reply.nativeError = actual.result.nativeError; }
        }
        return reply;
    }
    if (request.command == "export-diagnostics") {
        EmptyPayload(payload); if (!api_.exportDiagnostics) return Rejected("not-ready","Diagnostic exporter is not connected");
        const auto file = api_.exportDiagnostics(); auto reply = StatusResponse(); auto body = ParseJsonObject(reply.payload);
        Text(body,L"path",winrt::to_string(file.wstring())); reply.payload = winrt::to_string(body.Stringify()); return reply;
    }
    return Rejected("unknown-command","Command is not in the product control catalog");
}
}
