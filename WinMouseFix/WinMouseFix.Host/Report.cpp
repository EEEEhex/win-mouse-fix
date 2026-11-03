#include "Report.h"
#include "DiagnosticText.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace wmf::host {
std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto count = static_cast<int>(text.size());
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count, nullptr, 0, nullptr, nullptr);
    if (!size) return "<invalid-unicode>";
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count, result.data(), size, nullptr, nullptr);
    return result;
}
std::string UtcNow() {
    SYSTEMTIME t{}; GetSystemTime(&t);
    char text[40]{};
    sprintf_s(text, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return text;
}
std::filesystem::path DefaultOutputRoot() {
    auto root = std::filesystem::current_path();
    for (auto p = root; !p.empty() && p != p.parent_path(); p = p.parent_path())
        if (std::filesystem::exists(p / L"openspec")) return p / L"artifacts/native-gesture-validation";
    return root / L"artifacts/native-gesture-validation";
}
namespace {
std::pair<std::string, std::string> TargetInfo(unsigned pid) {
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {"unknown", "unknown"};
    wchar_t path[32768]{}; DWORD length = static_cast<DWORD>(std::size(path));
    const auto ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!ok) return {"unknown", "unknown"};
    std::string version = "unknown";
    DWORD unused{}; const auto bytes = GetFileVersionInfoSizeW(path, &unused);
    if (bytes) {
        std::vector<std::byte> data(bytes);
        if (GetFileVersionInfoW(path, 0, bytes, data.data())) {
            VS_FIXEDFILEINFO* info{}; UINT size{};
            if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size) && size >= sizeof(*info)) {
                version = std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
                    std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
            }
        }
    }
    return {Utf8(std::filesystem::path(path).filename().wstring()), version};
}
double P95(std::vector<double> samples) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    return samples[(samples.size() * 95 + 99) / 100 - 1];
}
}

ReportWriter::ReportWriter(const std::filesystem::path& root, ProbeInfo probe, ReportFaults faults)
    : probe_(std::move(probe)), faults_(faults), createdUtc_(UtcNow()) {
    LARGE_INTEGER f{}, counter{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&counter);
    frequency_ = f.QuadPart;
    runId_ = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter.QuadPart);
    directory_ = std::filesystem::absolute(root) / runId_;
    std::filesystem::create_directories(directory_);
    thread_ = std::thread(&ReportWriter::Run, this);
}
ReportWriter::~ReportWriter() { Finish(); }
void ReportWriter::Finish() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}
void ReportWriter::Push(const TraceRecord& trace) noexcept {
    if (!traces_.Push(trace)) lost_.fetch_add(1, std::memory_order_relaxed);
}
bool ReportWriter::Observe(unsigned index, const Observation& observation) {
    if (index >= observations_.size() || observation.finding > Finding::Inconclusive ||
        (observation.finding != Finding::NotRun && !observation.HasEvidence())) return false;
    std::lock_guard lock(mutex_);
    if (pending_.size() >= 128) return false;
    pending_.emplace_back(index, observation);
    return true;
}
void ReportWriter::RecordDiagnostic(std::string event, std::string message, unsigned experiment) {
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    std::lock_guard lock(mutex_);
    if (pendingDiagnostics_.size() >= 128) { lost_.fetch_add(1, std::memory_order_relaxed); return; }
    pendingDiagnostics_.push_back({std::move(event),std::move(message),experiment,now.QuadPart});
}
std::string ReportWriter::Summary() const { std::lock_guard lock(mutex_); return summary_; }

void ReportWriter::Consume(const TraceRecord& r, std::ostream& csv) {
    if (r.kind == TraceKind::Start) {
        Experiment e; e.start = r.start;
        const auto info = TargetInfo(r.start.targetPid);
        e.targetImage = info.first; e.targetVersion = info.second;
        experiments_.push_back(std::move(e));
    }
    auto it = std::find_if(experiments_.begin(), experiments_.end(), [&](const auto& e) { return e.start.experiment == r.experiment; });
    if (r.kind == TraceKind::Frame && it != experiments_.end()) {
        ++it->frameCount;
        it->processingMs.push_back((r.returned - r.processStart) * 1000.0 / static_cast<double>(frequency_));
        if (!r.result) it->result = r.result;
        if (r.submitStart) { probe_.submissionAttempted = true; probe_.submission = r.result; }
    }
    if (r.kind == TraceKind::Finish && it != experiments_.end()) {
        it->finished = true;
        if (!r.result) it->result = r.result;
    }
    if (r.kind == TraceKind::Device) { probe_.deviceAttempted = true; probe_.device = r.result; }
    // Keep the first cleanup failure even when a later device cleans up successfully.
    if (r.kind == TraceKind::Cleanup && probe_.cleanup) probe_.cleanup = r.result;
    if (r.kind == TraceKind::Initialization) {
        probe_.instancesAttempted = true; probe_.instances = r.result;
        if (r.detail == 1) { probe_.coreAttempted = true; probe_.coreInstance = r.result; }
        if (r.detail == 2) { probe_.platformAttempted = true; probe_.platformInstance = r.result; }
    }
    if (r.kind != TraceKind::Frame) events_.push_back(r);
    const auto count = (std::max)(1u, r.mapped.count);
    for (unsigned i = 0; i < count; ++i) {
        const auto c = r.mapped.contacts[i];
        csv << r.experiment << ',' << static_cast<unsigned>(r.kind) << ',' << r.detail << ',' << r.frame.sequence << ',' <<
            static_cast<unsigned>(r.frame.phase) << ',' << r.frame.fingers << ',' << r.frame.logicalSeconds << ',' <<
            r.planned << ',' << r.received << ',' << r.processStart << ',' << r.submitStart << ',' << r.returned << ',' <<
            c.id << ',' << c.x << ',' << c.y << ',' << StatusName(r.result.code) << ',' << r.result.nativeError << ',' << r.result.cleanupError << '\n';
    }
}

void ReportWriter::WriteReport() {
    if (faults_.slowWriteMs) std::this_thread::sleep_for(std::chrono::milliseconds(faults_.slowWriteMs));
    if (faults_.failWrite) throw std::runtime_error("Injected report write failure");
    const bool supported = probe_.query && (probe_.caps.apiBits & RequiredApis) == RequiredApis;
    const bool unsupported = probe_.modulesReady && probe_.query && !supported;
    const bool complete = !probe_.simulation && !failed_ && lost_ == 0 && probe_.modulesReady && probe_.query && supported &&
        probe_.deviceAttempted && probe_.device && probe_.cleanup;
    const auto verdict = Summarize(observations_, unsupported, complete);
    std::ostringstream out;
    out << std::setprecision(12);
    out << "{\n\"schemaVersion\":1,\"simulation\":" << (probe_.simulation ? "true" : "false") << ",\"runId\":" << Json(runId_) << ",\"createdUtc\":" << Json(createdUtc_) <<
        ",\"updatedUtc\":" << Json(UtcNow()) << ",\"build\":" <<
#ifdef _DEBUG
        Json("Debug") <<
#else
        Json("Release") <<
#endif
        ",\"architecture\":\"x64\",\"sdk\":" << Json(WMF_SDK_VERSION) << ",\"abiVersion\":" << AbiVersion <<
        ",\"coreAbiVersion\":" << (probe_.modulesReady ? AbiVersion : 0) << ",\"platformAbiVersion\":" << (probe_.modulesReady ? AbiVersion : 0) <<
        ",\"modulesReady\":" << (probe_.modulesReady ? "true" : "false") << ",\"moduleError\":" << Json(probe_.moduleError) <<
        ",\"runtimeInitialization\":" << (probe_.instancesAttempted ? JsonResult(probe_.instances) : "{\"status\":\"not-run\"}") <<
        ",\"initializationStages\":{\"core\":" << (probe_.coreAttempted ? JsonResult(probe_.coreInstance) : "{\"status\":\"not-run\"}") <<
        ",\"platform\":" << (probe_.platformAttempted ? JsonResult(probe_.platformInstance) : "{\"status\":\"not-run\"}") << "}" <<
        ",\"os\":{\"major\":" << probe_.caps.osMajor << ",\"minor\":" << probe_.caps.osMinor << ",\"build\":" << probe_.caps.osBuild <<
        ",\"revision\":" << probe_.caps.osRevision << ",\"revisionKnown\":" << (probe_.caps.revisionKnown ? "true" : "false") << "},\n";
    out << "\"capabilities\":{\"apiBits\":" << probe_.caps.apiBits << ",\"query\":" << JsonResult(probe_.query) <<
        ",\"createApi\":" << ((probe_.caps.apiBits & CreateApi) ? "true" : "false") <<
        ",\"injectApi\":" << ((probe_.caps.apiBits & InjectApi) ? "true" : "false") <<
        ",\"destroyApi\":" << ((probe_.caps.apiBits & DestroyApi) ? "true" : "false") <<
        ",\"inertiaActionApi\":" << ((probe_.caps.apiBits & ActionApi) ? "true" : "false") <<
        ",\"deviceAttempted\":" << (probe_.deviceAttempted ? "true" : "false") << ",\"device\":" <<
        (probe_.deviceAttempted ? JsonResult(probe_.device) : "{\"status\":\"not-run\"}") <<
        ",\"frameSubmission\":" << (probe_.submissionAttempted ? JsonResult(probe_.submission) : "{\"status\":\"not-run\"}") <<
        ",\"cleanup\":" << JsonResult(probe_.cleanup) << "},\n";
    out << "\"qpcFrequency\":" << frequency_ << ",\"traceRecordsLost\":" << lost_.load() <<
        ",\"evidenceComplete\":" << (complete ? "true" : "false") << ",\"verdict\":" << Json(VerdictName(verdict)) << ",\n\"experiments\":[";
    bool first = true;
    for (const auto& e : experiments_) {
        if (!first) out << ','; first = false;
        const auto& s = e.start;
        out << "{\"id\":" << s.experiment << ",\"case\":" << Json(CaseNames[static_cast<unsigned>(s.scenario)]) <<
            ",\"fingers\":" << s.trajectory.fingers << ",\"profile\":" << static_cast<unsigned>(s.trajectory.profile) <<
            ",\"interactive\":" << (s.trajectory.interactive ? "true" : "false") << ",\"hz\":" << s.hz <<
            ",\"durationSeconds\":" << s.trajectory.durationSeconds << ",\"extentXmm\":" << s.trajectory.extentXmm <<
            ",\"extentYmm\":" << s.trajectory.extentYmm << ",\"deviceWidthHimetric\":" << s.device.widthHimetric <<
            ",\"deviceHeightHimetric\":" << s.device.heightHimetric << ",\"anchor\":[" << s.anchorX << ',' << s.anchorY <<
            "],\"targetPid\":" << s.targetPid << ",\"targetImage\":" << Json(e.targetImage) << ",\"targetVersion\":" << Json(e.targetVersion) <<
            ",\"refreshHz\":" << s.refreshHz << ",\"dpi\":" << s.dpi << ",\"preparation\":" << Json(Utf8(s.preparation)) <<
            ",\"preconditionsConfirmed\":" << (s.prepared ? "true" : "false") << ",\"finished\":" << (e.finished ? "true" : "false") <<
            ",\"frames\":" << e.frameCount << ",\"result\":" << JsonResult(e.result) << ",\"processingP95Ms\":";
        if (e.processingMs.size() >= 100) out << P95(e.processingMs); else out << "null";
        out << ",\"processingTargetMs\":2,\"metric\":\"worker-processing-to-injection-return; not screen latency\"}";
    }
    out << "],\n\"observations\":[";
    first = true;
    for (const auto& o : observations_) {
        if (!first) out << ','; first = false;
        out << "{\"case\":" << Json(CaseNames[static_cast<unsigned>(o.scenario)]) << ",\"item\":" << Json(o.item) <<
            ",\"state\":" << Json(FindingNames[static_cast<unsigned>(o.finding)]) << ",\"experiment\":" << o.experiment <<
            ",\"observer\":" << Json(o.observer) << ",\"utc\":" << Json(o.utc) << ",\"evidence\":" << Json(o.evidence) << ",\"notes\":" << Json(o.notes) << '}';
    }
    out << "],\n\"capabilityVerdicts\":{\n";
    for (unsigned i = 0; i < CaseNames.size(); ++i) {
        std::vector<Observation> subset;
        for (const auto& o : observations_) if (static_cast<unsigned>(o.scenario) == i) subset.push_back(o);
        if (i) out << ',';
        out << Json(CaseNames[i]) << ':' << Json(VerdictName(Summarize(subset, unsupported, complete)));
    }
    out << "},\"events\":[";
    first = true;
    for (const auto& e : events_) {
        if (!first) out << ','; first = false;
        out << "{\"kind\":" << static_cast<unsigned>(e.kind) << ",\"experiment\":" << e.experiment <<
            ",\"detail\":" << e.detail << ",\"qpc\":" << e.returned << ",\"result\":" << JsonResult(e.result) << '}';
    }
    out << "],\"diagnosticEvents\":[";
    first = true;
    for (const auto& event : diagnostics_) {
        if (!first) out << ','; first = false;
        out << "{\"event\":" << Json(event.event) << ",\"message\":" << Json(event.message) <<
            ",\"experiment\":" << event.experiment << ",\"qpc\":" << event.qpc << '}';
    }
    out << "]\n}\n";
    const auto temporary = directory_ / L"report.json.tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file << out.str(); file.close();
    }
    if (!MoveFileExW(temporary.c_str(), (directory_ / L"report.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit report.json");
    std::lock_guard lock(mutex_);
    summary_ = std::string("Verdict: ") + VerdictName(verdict) + " | experiments: " + std::to_string(experiments_.size()) +
        " | lost records: " + std::to_string(lost_.load()) + " | " + Utf8(directory_.wstring());
}

void ReportWriter::Run() noexcept {
    try {
        std::ofstream csv(directory_ / L"frames.csv", std::ios::binary);
        csv.exceptions(std::ios::failbit | std::ios::badbit);
        csv << "experiment,kind,detail,sequence,phase,fingers,logical_seconds,planned_qpc,command_received_qpc,process_start_qpc,submit_start_qpc,returned_qpc,contact_id,x_himetric,y_himetric,status,native_error,cleanup_error\n";
        csv << std::setprecision(12);
        WriteReport();
        while (true) {
            TraceRecord record;
            bool dirty = false;
            while (traces_.Pop(record)) { Consume(record, csv); dirty = true; }
            std::vector<std::pair<unsigned, Observation>> pending;
            std::vector<DiagnosticEvent> diagnostics;
            { std::lock_guard lock(mutex_); pending.swap(pending_); diagnostics.swap(pendingDiagnostics_); }
            for (auto& event : diagnostics) {
                if (diagnostics_.size() < 1024) diagnostics_.push_back(std::move(event));
                else lost_.fetch_add(1, std::memory_order_relaxed);
                dirty = true;
            }
            for (const auto& [index, value] : pending) {
                auto& target = observations_[index];
                const auto experiment = std::find_if(experiments_.begin(), experiments_.end(), [&](const auto& e) {
                    return e.start.experiment == value.experiment && e.start.scenario == target.scenario && e.start.prepared;
                });
                if (experiment != experiments_.end() && experiment->finished &&
                    (value.finding != Finding::Pass || (experiment->frameCount > 0 && experiment->result))) {
                    target.finding = value.finding; target.experiment = value.experiment;
                    target.observer = value.observer; target.utc = value.utc; target.evidence = value.evidence; target.notes = value.notes;
                    dirty = true;
                } else {
                    std::lock_guard lock(mutex_); summary_ = "Observation rejected: matching finished experiment required";
                }
            }
            if (dirty || stop_) { csv.flush(); WriteReport(); }
            if (stop_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        csv.close();
    } catch (const std::exception& error) {
        failed_ = true;
        std::lock_guard lock(mutex_);
        summary_ = std::string("Evidence incomplete: ") + error.what();
    } catch (...) {
        failed_ = true;
        std::lock_guard lock(mutex_); summary_ = "Evidence incomplete: unknown writer error";
    }
}
}
