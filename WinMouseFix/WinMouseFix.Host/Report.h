#pragma once
#include <Windows.h>
#include "NativeApi.h"
#include "Evidence.h"
#include "SpscRing.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace wmf::host {
std::string Utf8(std::wstring_view text);
std::string UtcNow();
std::filesystem::path DefaultOutputRoot();

struct StartInfo {
    TrajectoryConfig trajectory{};
    DeviceConfig device{};
    Case scenario{};
    unsigned hz{120};
    unsigned experiment{};
    unsigned prepared{};
    unsigned targetPid{};
    unsigned refreshHz{};
    unsigned dpi{};
    int anchorX{}, anchorY{};
    wchar_t preparation[384]{};
};
enum class TraceKind : unsigned { Start, Frame, Control, Finish, Device, StopAction, Cleanup, Failure, Initialization };
struct TraceRecord {
    TraceKind kind{};
    unsigned experiment{};
    unsigned detail{};
    std::int64_t planned{}, received{}, processStart{}, submitStart{}, returned{};
    GestureFrame frame{};
    MappedFrame mapped{};
    Result result{};
    StartInfo start{};
};
struct ProbeInfo {
    bool simulation{};
    bool modulesReady{};
    std::string moduleError;
    Capabilities caps{};
    Result query{};
    bool instancesAttempted{};
    Result instances{};
    bool coreAttempted{}, platformAttempted{};
    Result coreInstance{}, platformInstance{};
    bool deviceAttempted{};
    Result device{};
    Result cleanup{};
    bool submissionAttempted{};
    Result submission{};
};
struct ReportFaults { unsigned slowWriteMs{}; bool failWrite{}; };
struct DiagnosticEvent {
    std::string event, message;
    unsigned experiment{};
    std::int64_t qpc{};
};

class ReportWriter {
public:
    ReportWriter(const std::filesystem::path& root, ProbeInfo probe, ReportFaults faults = {});
    ~ReportWriter();
    ReportWriter(const ReportWriter&) = delete;
    ReportWriter& operator=(const ReportWriter&) = delete;
    void Push(const TraceRecord& trace) noexcept;
    bool Observe(unsigned index, const Observation& observation);
    // UI thread only: never put a second producer onto the input trace SPSC queue.
    void RecordDiagnostic(std::string event, std::string message, unsigned experiment = 0);
    std::string Summary() const;
    const std::filesystem::path& Directory() const noexcept { return directory_; }
    std::uint64_t Lost() const noexcept { return lost_.load(); }
    bool Failed() const noexcept { return failed_.load(); }
    void Finish();
private:
    struct Experiment {
        StartInfo start{};
        std::string targetImage, targetVersion;
        unsigned frameCount{};
        bool finished{};
        Result result{};
        std::vector<double> processingMs;
    };
    void Run() noexcept;
    void Consume(const TraceRecord&, std::ostream& csv);
    void WriteReport();
    ProbeInfo probe_;
    ReportFaults faults_;
    std::filesystem::path directory_;
    std::string runId_, createdUtc_;
    SpscRing<TraceRecord, 8192> traces_;
    std::atomic<std::uint64_t> lost_{};
    std::atomic<bool> stop_{}, failed_{};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::pair<unsigned, Observation>> pending_;
    std::vector<DiagnosticEvent> pendingDiagnostics_, diagnostics_;
    std::string summary_{"Preparing report"};
    std::vector<Observation> observations_{BaselineObservations()};
    std::vector<Experiment> experiments_;
    std::vector<TraceRecord> events_;
    std::int64_t frequency_{};
};
}
