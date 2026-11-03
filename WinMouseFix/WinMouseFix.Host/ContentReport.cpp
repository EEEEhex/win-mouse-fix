#include "ContentReport.h"
#include "DiagnosticText.h"
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <stdexcept>

namespace wmf::host {
namespace {
std::uint64_t Counter() noexcept { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return static_cast<std::uint64_t>(t.QuadPart); }
void ResultJson(std::ostream& out,Result r) {
    out << "{\"status\":" << Json(StatusName(r.code)) << ",\"nativeError\":" << r.nativeError << ",\"cleanupError\":" << r.cleanupError << '}';
}
double Percentile(std::vector<double> values,unsigned percentile) {
    if (values.empty()) return 0;
    std::sort(values.begin(),values.end());
    return values[(values.size()*percentile+99)/100-1];
}
std::pair<std::string,std::string> TargetVersion(std::uint64_t window) {
    DWORD pid{}; GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window)),&pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    wchar_t path[32768]{}; DWORD length = static_cast<DWORD>(std::size(path));
    const bool found = process && QueryFullProcessImageNameW(process,0,path,&length);
    if (process) CloseHandle(process);
    if (!found) return {"unknown","unknown"};
    std::string version = "unknown";
    const DWORD bytes = GetFileVersionInfoSizeW(path,nullptr);
    if (bytes) {
        std::vector<std::byte> buffer(bytes);
        if (GetFileVersionInfoW(path,0,bytes,buffer.data())) {
            VS_FIXEDFILEINFO* info{}; UINT size{};
            if (VerQueryValueW(buffer.data(),L"\\",reinterpret_cast<void**>(&info),&size) && size >= sizeof(*info))
                version = std::to_string(HIWORD(info->dwFileVersionMS))+"."+std::to_string(LOWORD(info->dwFileVersionMS))+"."+
                    std::to_string(HIWORD(info->dwFileVersionLS))+"."+std::to_string(LOWORD(info->dwFileVersionLS));
        }
    }
    return {Utf8(std::filesystem::path(path).filename().wstring()),version};
}
}
ContentReport::ContentReport(std::filesystem::path root,ContentOptions options,bool simulation)
    : options_(options),simulation_(simulation),createdUtc_(UtcNow()) {
    LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); frequency_ = static_cast<std::uint64_t>(f.QuadPart);
    runId_ = std::to_string(GetCurrentProcessId())+"-"+std::to_string(Counter());
    directory_ = std::move(root)/runId_; std::filesystem::create_directories(directory_);
    writer_ = std::thread(&ContentReport::Run,this);
}
void ContentReport::Push(const ContentTrace& t) noexcept { if (!queue_.Push(t)) ++lost_; }
void ContentReport::SetCapture(CaptureStats stats,Result failure) noexcept {
    std::lock_guard lock(captureMutex_); capture_ = stats;
    if (!failure && captureFailure_) captureFailure_ = failure;
}
void ContentReport::Finish() { stop_ = true; if (writer_.joinable()) writer_.join(); }
void ContentReport::WriteReport() {
    std::ofstream out(directory_/L"report.tmp",std::ios::binary|std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write content report");
    CaptureStats capture; Result captureFailure;
    { std::lock_guard lock(captureMutex_); capture = capture_; captureFailure = captureFailure_; }
    Capabilities caps; WmfPlatformCapabilities(&caps);
    out << std::setprecision(10) << "{\"schemaVersion\":1,\"kind\":\"side-button-content-drag\",\"simulation\":" << (simulation_ ? "true" : "false") <<
        ",\"captureOnly\":" << (options_.captureOnly ? "true" : "false") << ",\"runId\":" << Json(runId_) <<
        ",\"createdUtc\":" << Json(createdUtc_) << ",\"updatedUtc\":" << Json(UtcNow()) << ",\"abiVersion\":" << AbiVersion <<
        ",\"sdk\":" << Json(WMF_SDK_VERSION) << ",\"architecture\":\"x64\",\"build\":" <<
#ifdef _DEBUG
        "\"Debug\"" <<
#else
        "\"Release\"" <<
#endif
        ",\"os\":{\"build\":" << caps.osBuild << ",\"revision\":" << caps.osRevision << "},\"qpcFrequency\":" << frequency_ <<
        ",\"config\":{\"button\":\"XBUTTON1\",\"thresholdCounts\":" << options_.drag.thresholdCounts <<
        ",\"mmPerCountX\":" << options_.drag.mmPerCountX << ",\"mmPerCountY\":" << options_.drag.mmPerCountY <<
        ",\"widthMm\":" << options_.device.widthHimetric/100.0 << ",\"heightMm\":" << options_.device.heightHimetric/100.0 << "}," <<
        "\"counts\":{\"inputs\":" << inputs_ << ",\"submittedFrames\":" << frames_ << ",\"sessions\":" << sessions_ <<
        ",\"clickIntents\":" << clicks_ << ",\"normalDragEnds\":" << normalEnds_ << ",\"cancellations\":" << canceled_ << ",\"inertiaStopCalls\":" << stops_ << "}," <<
        "\"capture\":{\"rawPackets\":" << capture.rawPackets << ",\"rawMoves\":" << capture.rawMoves << ",\"blockedMoves\":" << capture.blockedMoves <<
        ",\"capturedDowns\":" << capture.capturedDowns << ",\"rawDowns\":" << capture.rawDowns << ",\"rawUps\":" << capture.rawUps <<
        ",\"replayCalls\":" << capture.replayCalls << ",\"replayFailures\":" << capture.replayFailures << ",\"hookCalls\":" << capture.hookCalls <<
        ",\"hookAverageMs\":" << (capture.hookCalls ? 1000.0*static_cast<double>(capture.hookTicks)/static_cast<double>(capture.hookCalls)/static_cast<double>(frequency_) : 0) <<
        ",\"hookMaximumMs\":" << 1000.0*static_cast<double>(capture.hookMaxTicks)/static_cast<double>(frequency_) << "},\"firstFailure\":";
    ResultJson(out,firstFailure_); out << ",\"captureFailure\":"; ResultJson(out,captureFailure);
    out << ",\"lastTargetGuard\":";
    if (hasTargetGuard_) {
        const auto& t = lastTargetGuard_;
        out << "{\"session\":" << t.input.session << ",\"flags\":" << t.guardFlags << ",\"expectedWindow\":" << t.input.target <<
            ",\"observedForeground\":" << t.observedForeground << ",\"expectedAnchor\":[" << t.input.anchorX << ',' << t.input.anchorY <<
            "],\"observedCursor\":[" << t.observedX << ',' << t.observedY << "]}";
    } else out << "null";
    out << ",\"traceRecordsLost\":" << lost_.load() << ",\"statisticsTruncated\":" << (sampleLimit_ ? "true" : "false") <<
        ",\"timing\":{\"metric\":\"earliest merged host receipt to injection return; not hardware or screen latency\",\"normalUpdateSamples\":" << latency_.size() <<
        ",\"targetP95Ms\":2,\"targetStatus\":" << Json(latency_.size() < 100 || simulation_ || options_.captureOnly || lost_ || sampleLimit_ ? "inconclusive" : Percentile(latency_,95) <= 2 ? "met" : "exceeded") <<
        ",\"earliestP50Ms\":" << Percentile(latency_,50) << ",\"earliestP95Ms\":" << Percentile(latency_,95) <<
        ",\"latestP95Ms\":" << Percentile(latestLatency_,95) << ",\"queueP95Ms\":" << Percentile(queueMs_,95) <<
        ",\"processingP95Ms\":" << Percentile(processingMs_,95) << ",\"boundarySamples\":" << boundaryLatency_.size() <<
        ",\"boundaryP95Ms\":" << Percentile(boundaryLatency_,95) << "},\"targets\":[";
    bool first = true;
    for (const auto& target : targets_) {
        if (!first) out << ','; first = false;
        out << "{\"session\":" << target.session << ",\"window\":" << target.window << ",\"image\":" << Json(target.image) << ",\"version\":" << Json(target.version) << '}';
    }
    out << "],\"manualObservations\":\"not-run; record user feedback separately\",\"verdict\":\"inconclusive\"}";
    out.close();
    if (!out || !MoveFileExW((directory_/L"report.tmp").c_str(),(directory_/L"report.json").c_str(),MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("Cannot finish content report");
}
void ContentReport::Run() noexcept {
    try {
        std::ofstream csv(directory_/L"events.csv",std::ios::binary);
        csv << "kind,submitted,session,input_sequence,input_kind,reason,first_qpc,last_qpc,samples,raw_dx,raw_dy,device,target,anchor_x,anchor_y,frame_sequence,phase,x_mm,y_mm,process_start_qpc,submit_start_qpc,returned_qpc,status,native_error,cleanup_error,guard_flags,observed_foreground,observed_cursor_x,observed_cursor_y\n";
        auto written = Counter();
        for (;;) {
            ContentTrace t; bool consumed{};
            while (queue_.Pop(t)) {
                consumed = true;
                const auto& e = t.input;
                csv << static_cast<unsigned>(t.kind) << ',' << t.submitted << ',' << e.session << ',' << e.sequence << ',' << static_cast<unsigned>(e.kind) << ',' << static_cast<unsigned>(e.reason) << ',' <<
                    e.firstQpc << ',' << e.lastQpc << ',' << e.samples << ',' << e.dx << ',' << e.dy << ',' << e.device << ',' << e.target << ',' << e.anchorX << ',' << e.anchorY << ',' <<
                    t.frame.sequence << ',' << static_cast<unsigned>(t.frame.phase) << ',' << std::setprecision(12) << t.frame.offsetXmm << ',' << t.frame.offsetYmm << ',' <<
                    t.processStart << ',' << t.submitStart << ',' << t.returned << ',' << StatusName(t.result.code) << ',' << t.result.nativeError << ',' << t.result.cleanupError << ',' <<
                    t.guardFlags << ',' << t.observedForeground << ',' << t.observedX << ',' << t.observedY << '\n';
                if (t.kind == ContentTraceKind::TargetGuard) { lastTargetGuard_ = t; hasTargetGuard_ = true; }
                if (!t.result && firstFailure_) firstFailure_ = t.result;
                if (t.kind == ContentTraceKind::Input) {
                    ++inputs_;
                    if (e.kind == MouseInputKind::Down) {
                        ++sessions_;
                        if (targets_.size() < 512) { const auto info = TargetVersion(e.target); targets_.push_back({e.session,e.target,info.first,info.second}); }
                    }
                    if (e.kind == MouseInputKind::Up && !t.submitted && (t.input.flags & ClickEffect)) ++clicks_;
                }
                if (t.kind == ContentTraceKind::Failure) ++canceled_;
                if (t.kind == ContentTraceKind::StopInertia) ++stops_;
                if (t.kind == ContentTraceKind::Frame && t.submitted) {
                    ++frames_;
                    if (t.frame.phase == Phase::End && t.result) ++normalEnds_;
                    if (t.result && (e.flags & ContentBoundary) && e.firstQpc && t.returned >= e.firstQpc) {
                        if (boundaryLatency_.size() < 100000) boundaryLatency_.push_back(1000.0*static_cast<double>(t.returned-e.firstQpc)/static_cast<double>(frequency_));
                        else sampleLimit_ = true;
                    }
                    if (t.result && !(e.flags & ContentBoundary) && t.frame.phase == Phase::Update && e.firstQpc && t.returned >= e.lastQpc && t.processStart >= e.firstQpc) {
                        if (latency_.size() < 100000) {
                            const double factor = 1000.0/static_cast<double>(frequency_);
                            latency_.push_back(static_cast<double>(t.returned-e.firstQpc)*factor);
                            latestLatency_.push_back(static_cast<double>(t.returned-e.lastQpc)*factor);
                            queueMs_.push_back(static_cast<double>(t.processStart-e.firstQpc)*factor);
                            processingMs_.push_back(static_cast<double>(t.returned-t.processStart)*factor);
                        } else sampleLimit_ = true;
                    }
                }
            }
            if (!csv) throw std::runtime_error("Cannot write content trace");
            if (stop_ && !consumed) break;
            const auto now = Counter();
            if (now-written >= frequency_/4) { csv.flush(); WriteReport(); written = now; }
            Sleep(10);
        }
        csv.flush(); WriteReport();
    } catch (...) { failed_ = true; }
}
}
