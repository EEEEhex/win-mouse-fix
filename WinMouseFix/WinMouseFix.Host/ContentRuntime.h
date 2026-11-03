#pragma once
#include "ContentReport.h"
#include "ContentInputBuffer.h"

namespace wmf::host {
struct ContentRuntimeApi {
    ContentFunctions engine;
    decltype(&WmfCaptureCreate) createCapture{WmfCaptureCreate};
    decltype(&WmfCapturePost) postCapture{WmfCapturePost};
    decltype(&WmfCaptureReadStats) readCapture{WmfCaptureReadStats};
    decltype(&WmfCaptureDestroy) destroyCapture{WmfCaptureDestroy};
};
enum class ContentRuntimeState : unsigned { Starting, Enabled, Paused, Faulted, Stopping, Stopped };
class ContentRuntime {
public:
    ContentRuntime(ContentReport&,ContentOptions,ContentRuntimeApi = {});
    ~ContentRuntime();
    void Enable(bool) noexcept;
    void Stop() noexcept;
    bool Finished() const noexcept { return workerDone_.load() && captureDone_.load(); }
    bool Requested() const noexcept { return requested_.load(); }
    ContentRuntimeState State() const noexcept;
    Result Failure() const noexcept;
private:
    static bool Receive(void*,const MouseInput*) noexcept;
    bool Receive(const MouseInput&) noexcept;
    void Worker() noexcept;
    void Capture() noexcept;
    void Fault(Result) noexcept;
    void PostComplete(std::uint64_t,bool,bool) noexcept;
    void RecordFailure() noexcept;
    ContentReport& report_;
    ContentOptions options_;
    ContentRuntimeApi api_;
    ContentInputBuffer input_;
    HANDLE stop_{},workerWake_{},captureWake_{},workerInitialized_{};
    std::thread workerThread_,captureThread_;
    std::atomic<bool> requested_{true},engineReady_{},captureReady_{},workerDone_{},captureDone_{},cancel_{};
    std::atomic<bool> captureCancel_{},failurePending_{};
    std::atomic<std::uint64_t> requestedGeneration_{1},readyGeneration_{};
    std::atomic<CaptureHandle> capture_{};
    std::atomic<Status> failureCode_{Status::Ok};
    std::atomic<std::uint32_t> nativeError_{},cleanupError_{};
};
}
