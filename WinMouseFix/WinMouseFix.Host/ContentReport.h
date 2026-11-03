#pragma once
#include "ContentEngine.h"
#include "Report.h"

namespace wmf::host {
struct ContentOptions {
    DragConfig drag;
    DeviceConfig device{sizeof(DeviceConfig),AbiVersion,50000,50000};
    bool captureOnly{};
};
class ContentReport {
public:
    ContentReport(std::filesystem::path,ContentOptions,bool simulation = false);
    ~ContentReport() { Finish(); }
    void Push(const ContentTrace&) noexcept;
    void SetCapture(CaptureStats,Result = {}) noexcept;
    void Finish();
    bool Failed() const noexcept { return failed_.load(); }
    std::uint64_t Lost() const noexcept { return lost_.load(); }
    const std::filesystem::path& Directory() const noexcept { return directory_; }
private:
    void Run() noexcept;
    void WriteReport();
    ContentOptions options_;
    bool simulation_{};
    std::filesystem::path directory_;
    std::string createdUtc_,runId_;
    std::uint64_t frequency_{};
    SpscRing<ContentTrace,8192> queue_;
    std::thread writer_;
    std::atomic<bool> stop_{},failed_{};
    std::atomic<std::uint64_t> lost_{};
    std::mutex captureMutex_;
    CaptureStats capture_;
    Result captureFailure_;
    std::uint64_t inputs_{},frames_{},sessions_{},canceled_{},clicks_{},normalEnds_{},stops_{};
    Result firstFailure_;
    ContentTrace lastTargetGuard_;
    bool hasTargetGuard_{};
    std::vector<double> latency_,latestLatency_,queueMs_,processingMs_,boundaryLatency_;
    bool sampleLimit_{};
    struct Target { std::uint64_t session{},window{}; std::string image,version; };
    std::vector<Target> targets_;
};
}
