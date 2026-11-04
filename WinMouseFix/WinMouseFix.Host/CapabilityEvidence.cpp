#include "CapabilityEvidence.h"
#include "JsonSupport.h"
#include "Report.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <iomanip>
#include <charconv>
namespace wmf::host {
namespace {
std::uint64_t HashFile(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); if (!in) throw std::runtime_error("Cannot fingerprint engine module");
    std::uint64_t hash = 14695981039346656037ull; char bytes[16384];
    while (in) { in.read(bytes,sizeof(bytes)); for (std::streamsize i = 0; i < in.gcount(); ++i) { hash ^= static_cast<unsigned char>(bytes[i]); hash *= 1099511628211ull; } }
    if (!in.eof()) throw std::runtime_error("Cannot read engine module"); return hash;
}
std::string Mapping(const BindingConfig& c) {
    std::ostringstream out; out << std::setprecision(17) << c.shellFingers << '/' << c.shellMmPerCountX << '/' << c.shellMmPerCountY << '/' << c.thresholdCounts << '/' << c.directionRatio; return out.str();
}
std::string_view IdentityEnvironment(std::string_view identity) noexcept {
    std::size_t start{},environmentEnd{};
    for (unsigned i=0;i<9;++i) {
        const auto slash=identity.find('/',start);
        if ((i<8 && slash==std::string_view::npos) || (i==8 && slash!=std::string_view::npos)) return {};
        const auto end=slash==std::string_view::npos ? identity.size() : slash;
        if (start==end) return {};
        std::uint64_t number{}; const auto parsed=std::from_chars(identity.data()+start,identity.data()+end,number);
        if (parsed.ec!=std::errc{} || parsed.ptr!=identity.data()+end) return {};
        if (i==5) environmentEnd=slash;
        if (slash!=std::string_view::npos) start=slash+1;
    }
    return identity.substr(0,environmentEnd);
}
std::string PinchScope(std::string_view identity) {
    return std::string(IdentityEnvironment(identity))+"/pinch/"+std::to_string(PinchValidationContract);
}
}
CapabilityEvidence::CapabilityEvidence(ProductEnvironment environment,Capabilities caps) : environment_(std::move(environment)) {
    eligible_ = environment_.simulation || (caps.revisionKnown && (caps.apiBits & (RequiredApis|ActionApi)) == (RequiredApis|ActionApi));
    std::ostringstream out; out << AbiVersion << '/' << caps.osMajor << '/' << caps.osMinor << '/' << caps.osBuild << '/' << caps.osRevision << '/' << caps.revisionKnown
        << '/' << HashFile(environment_.executableDirectory/L"WinMouseFix.Host.exe") << '/' << HashFile(environment_.executableDirectory/L"WinMouseFix.Core.dll") << '/' << HashFile(environment_.executableDirectory/L"WinMouseFix.Platform.Win32.dll"); identity_ = out.str();
    const auto path = environment_.dataDirectory/L"shell-validation.json";
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) <= 16384) { std::ifstream in(path,std::ios::binary); shellJson_.assign(std::istreambuf_iterator<char>(in),{}); }
}
std::string CapabilityEvidence::ShellObservations(const BindingConfig& config) const {
    std::lock_guard lock(shellMutex_);
    try {
        if (shellJson_.empty()) return "{}"; const auto root = ParseJsonObject(shellJson_);
        if (root.GetNamedString(L"identity") != winrt::to_hstring(identity_) || root.GetNamedString(L"mapping") != winrt::to_hstring(Mapping(config)) ||
            root.GetNamedBoolean(L"simulation") != environment_.simulation || root.GetNamedNumber(L"schemaVersion") != 1) return "{}";
        const auto observations = root.GetNamedObject(L"observations"); if (observations.Size() > 4) return "{}";
        for (const auto& entry : observations) {
            const auto name = entry.Key(); if (name != L"task-view" && name != L"show-desktop" && name != L"desktop-left" && name != L"desktop-right") return "{}";
            const auto item = entry.Value().GetObject(); if (item.GetNamedString(L"action") != name || item.GetNamedString(L"notes").size() > 1024) return "{}";
            for (const auto field : {L"trigger",L"continuous",L"hold",L"reverse",L"release"}) {
                const auto state = item.GetNamedString(field);
                if (state != L"not-run" && state != L"pass" && state != L"fail" && !(std::wstring_view(field) == L"continuous" && state == L"limited")) return "{}";
            }
        }
        return winrt::to_string(observations.Stringify());
    } catch (...) { return "{}"; }
}
void CapabilityEvidence::RecordShell(const BindingConfig& config,std::string_view payload) {
    using namespace winrt::Windows::Data::Json;
    const auto observation = ParseJsonObject(payload); if (observation.Size() != 7) throw std::invalid_argument("Expected action, five observations and notes");
    const auto name = observation.GetNamedString(L"action");
    if (name != L"task-view" && name != L"show-desktop" && name != L"desktop-left" && name != L"desktop-right") throw std::invalid_argument("Unknown Shell action");
    for (const auto field : {L"trigger",L"continuous",L"hold",L"reverse",L"release"}) {
        const auto state = observation.GetNamedString(field);
        if (state != L"not-run" && state != L"pass" && state != L"fail" && !(std::wstring_view(field) == L"continuous" && state == L"limited")) throw std::invalid_argument("Invalid observation state");
    }
    const auto notes = observation.GetNamedString(L"notes"); if (notes.empty() || notes.size() > 1024) throw std::invalid_argument("Describe the app, desktop neighbours and actual observation");
    std::lock_guard lock(shellMutex_);
    JsonObject observations;
    try { if (!shellJson_.empty()) { const auto old = ParseJsonObject(shellJson_); if (old.GetNamedString(L"identity") == winrt::to_hstring(identity_) && old.GetNamedString(L"mapping") == winrt::to_hstring(Mapping(config))) observations = old.GetNamedObject(L"observations"); } } catch (...) {}
    auto recorded = observation; recorded.SetNamedValue(L"utc",JsonValue::CreateStringValue(winrt::to_hstring(UtcNow()))); recorded.SetNamedValue(L"revision",JsonValue::CreateNumberValue(static_cast<double>(config.revision))); observations.SetNamedValue(name,recorded);
    JsonObject root; root.SetNamedValue(L"schemaVersion",JsonValue::CreateNumberValue(1)); root.SetNamedValue(L"simulation",JsonValue::CreateBooleanValue(environment_.simulation));
    root.SetNamedValue(L"identity",JsonValue::CreateStringValue(winrt::to_hstring(identity_))); root.SetNamedValue(L"mapping",JsonValue::CreateStringValue(winrt::to_hstring(Mapping(config)))); root.SetNamedValue(L"observations",observations);
    const auto bytes = winrt::to_string(root.Stringify()); std::filesystem::create_directories(environment_.dataDirectory);
    const auto target = environment_.dataDirectory/L"shell-validation.json",temporary = environment_.dataDirectory/L"shell-validation.tmp";
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc); out.exceptions(std::ios::badbit|std::ios::failbit); out << bytes; out.close();
    if (!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot commit Shell observations"); shellJson_ = bytes;
}
std::uint32_t CapabilityEvidence::Flags() const noexcept {
    try {
        if (!eligible_) return 0;
        const auto path = environment_.dataDirectory/L"native-validation.json";
        if (!std::filesystem::exists(path) || std::filesystem::file_size(path) > 8192) return 0;
        std::ifstream in(path,std::ios::binary); const std::string bytes(std::istreambuf_iterator<char>(in),{}); const auto root = ParseJsonObject(bytes);
        if (root.GetNamedNumber(L"schemaVersion") != 1 || root.GetNamedBoolean(L"simulation") != environment_.simulation) return 0;
        const auto recordedIdentity=winrt::to_string(root.GetNamedString(L"identity"));
        const auto environment=IdentityEnvironment(identity_),recordedEnvironment=IdentityEnvironment(recordedIdentity);
        if (environment.empty() || recordedEnvironment!=environment) return 0;
        const auto number = root.GetNamedNumber(L"flags"); if (!std::isfinite(number) || number < 0 || number > 3 || std::floor(number) != number) return 0;
        const auto flags = static_cast<std::uint32_t>(number);
        auto verified = flags;
        if (recordedIdentity!=identity_) verified &= ~WheelSourceValidated;
        if (verified & NativeZoomValidated) {
            try { const auto pinch = root.GetNamedObject(L"pinchObservation");
                // ABI v4 records predate pinchScope and used the same first
                // pinch contract. Reuse that observation without rewriting it.
                const bool compatible=root.HasKey(L"pinchScope")
                    ? root.GetNamedString(L"pinchScope")==winrt::to_hstring(PinchScope(identity_))
                    : AbiVersion==0x00040000 && PinchValidationContract==1;
                const auto raw = pinch.GetNamedNumber(L"raw",0),blocked = pinch.GetNamedNumber(L"blocked",0),accepted = pinch.GetNamedNumber(L"accepted",0);
                if (!compatible || !pinch.GetNamedBoolean(L"visualConfirmed",false) || pinch.GetNamedString(L"visualOutcome",L"") != L"pass" ||
                    !std::isfinite(raw) || raw < 10 || std::floor(raw) != raw || raw != blocked || raw != accepted || pinch.GetNamedNumber(L"frames",0) <= 0)
                    verified &= ~NativeZoomValidated;
            } catch (...) { verified &= ~NativeZoomValidated; }
        }
        return verified;
    } catch (...) { return 0; }
}
void CapabilityEvidence::Confirm(bool zoom,std::uint64_t expected,std::uint64_t raw,std::uint64_t blocked,std::uint64_t accepted,std::uint64_t frames,bool visual,std::string_view notes) {
    using namespace winrt::Windows::Data::Json;
    if (!eligible_ || !visual || expected < 10 || expected > 1000 || expected != raw || raw != blocked || raw != accepted || !frames || notes.empty() || notes.size() > 2048)
        throw std::invalid_argument("Need at least ten physical packets, matching Hook/Raw/accepted counts, native output and explicit target observation");
    const auto prior = Flags();
    JsonObject root;
    if (prior) { const auto path = environment_.dataDirectory/L"native-validation.json"; std::ifstream in(path,std::ios::binary); root = ParseJsonObject(std::string(std::istreambuf_iterator<char>(in),{})); }
    JsonObject observation; observation.SetNamedValue(L"utc",JsonValue::CreateStringValue(winrt::to_hstring(UtcNow())));
    observation.SetNamedValue(L"visualOutcome",JsonValue::CreateStringValue(L"pass"));
    observation.SetNamedValue(L"raw",JsonValue::CreateNumberValue(static_cast<double>(raw))); observation.SetNamedValue(L"blocked",JsonValue::CreateNumberValue(static_cast<double>(blocked))); observation.SetNamedValue(L"accepted",JsonValue::CreateNumberValue(static_cast<double>(accepted))); observation.SetNamedValue(L"frames",JsonValue::CreateNumberValue(static_cast<double>(frames))); observation.SetNamedValue(L"visualConfirmed",JsonValue::CreateBooleanValue(visual)); observation.SetNamedValue(L"notes",JsonValue::CreateStringValue(winrt::to_hstring(notes)));
    root.SetNamedValue(zoom ? L"pinchObservation" : L"sourceObservation",observation);
    root.SetNamedValue(L"schemaVersion",JsonValue::CreateNumberValue(1)); root.SetNamedValue(L"simulation",JsonValue::CreateBooleanValue(environment_.simulation));
    root.SetNamedValue(L"identity",JsonValue::CreateStringValue(winrt::to_hstring(identity_))); root.SetNamedValue(L"utc",JsonValue::CreateStringValue(winrt::to_hstring(UtcNow())));
    root.SetNamedValue(L"flags",JsonValue::CreateNumberValue(prior|(zoom ? NativeZoomValidated : WheelSourceValidated)));
    if (zoom || (prior & NativeZoomValidated)) root.SetNamedValue(L"pinchScope",JsonValue::CreateStringValue(winrt::to_hstring(PinchScope(identity_))));
    root.SetNamedValue(L"expectedPackets",JsonValue::CreateNumberValue(static_cast<double>(expected))); root.SetNamedValue(L"rawPackets",JsonValue::CreateNumberValue(static_cast<double>(raw)));
    root.SetNamedValue(L"blockedHooks",JsonValue::CreateNumberValue(static_cast<double>(blocked))); root.SetNamedValue(L"acceptedPackets",JsonValue::CreateNumberValue(static_cast<double>(accepted)));
    root.SetNamedValue(L"frames",JsonValue::CreateNumberValue(static_cast<double>(frames))); root.SetNamedValue(L"visualConfirmed",JsonValue::CreateBooleanValue(visual)); root.SetNamedValue(L"notes",JsonValue::CreateStringValue(winrt::to_hstring(notes)));
    root.SetNamedValue(L"visualOutcome",JsonValue::CreateStringValue(L"pass"));
    std::filesystem::create_directories(environment_.dataDirectory); const auto target = environment_.dataDirectory/L"native-validation.json",temporary = environment_.dataDirectory/L"native-validation.tmp";
    const auto bytes = winrt::to_string(root.Stringify()); std::ofstream out(temporary,std::ios::binary|std::ios::trunc); out.exceptions(std::ios::badbit|std::ios::failbit); out << bytes; out.close();
    if (!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot commit native validation evidence");
}
void CapabilityEvidence::RecordFailure(bool zoom,std::uint64_t raw,std::uint64_t blocked,std::uint64_t accepted,std::uint64_t frames,std::string_view notes,Result failure) {
    using namespace winrt::Windows::Data::Json;
    if (notes.empty() || notes.size() > 2048) throw std::invalid_argument("Describe the application and failed observation");
    const auto prior = Flags(); JsonObject root;
    if (prior) { std::ifstream in(environment_.dataDirectory/L"native-validation.json",std::ios::binary); root = ParseJsonObject(std::string(std::istreambuf_iterator<char>(in),{})); }
    JsonObject observation; observation.SetNamedValue(L"utc",JsonValue::CreateStringValue(winrt::to_hstring(UtcNow())));
    observation.SetNamedValue(L"visualOutcome",JsonValue::CreateStringValue(L"not-observed")); observation.SetNamedValue(L"visualConfirmed",JsonValue::CreateBooleanValue(false));
    observation.SetNamedValue(L"raw",JsonValue::CreateNumberValue(static_cast<double>(raw))); observation.SetNamedValue(L"blocked",JsonValue::CreateNumberValue(static_cast<double>(blocked))); observation.SetNamedValue(L"accepted",JsonValue::CreateNumberValue(static_cast<double>(accepted))); observation.SetNamedValue(L"frames",JsonValue::CreateNumberValue(static_cast<double>(frames)));
    observation.SetNamedValue(L"notes",JsonValue::CreateStringValue(winrt::to_hstring(notes)));
    JsonObject result; result.SetNamedValue(L"status",JsonValue::CreateNumberValue(static_cast<unsigned>(failure.code))); result.SetNamedValue(L"nativeError",JsonValue::CreateNumberValue(failure.nativeError)); result.SetNamedValue(L"cleanupError",JsonValue::CreateNumberValue(failure.cleanupError)); observation.SetNamedValue(L"result",result);
    root.SetNamedValue(zoom ? L"pinchObservation" : L"sourceObservation",observation); root.SetNamedValue(L"schemaVersion",JsonValue::CreateNumberValue(1)); root.SetNamedValue(L"simulation",JsonValue::CreateBooleanValue(environment_.simulation)); root.SetNamedValue(L"identity",JsonValue::CreateStringValue(winrt::to_hstring(identity_)));
    root.SetNamedValue(L"flags",JsonValue::CreateNumberValue(zoom ? prior & ~NativeZoomValidated : 0)); root.SetNamedValue(L"visualConfirmed",JsonValue::CreateBooleanValue(false)); root.SetNamedValue(L"visualOutcome",JsonValue::CreateStringValue(L"not-observed")); root.SetNamedValue(L"notes",JsonValue::CreateStringValue(winrt::to_hstring(notes)));
    std::filesystem::create_directories(environment_.dataDirectory); const auto target = environment_.dataDirectory/L"native-validation.json",temporary = environment_.dataDirectory/L"native-validation.tmp";
    std::ofstream out(temporary,std::ios::binary|std::ios::trunc); out.exceptions(std::ios::badbit|std::ios::failbit); out << winrt::to_string(root.Stringify()); out.close();
    if (!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot record failed native validation");
}
}
