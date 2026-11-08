#pragma once
#include "ProductEngine.h"
#include "ProductEnvironment.h"
#include "ProductInputBuffer.h"
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace wmf::host {
struct ProductRuntimeApi {
    ProductFunctions engine;
    decltype(&WmfProductCaptureCreate) createCapture{WmfProductCaptureCreate};
    decltype(&WmfCapturePost) post{WmfCapturePost};
    decltype(&WmfCaptureReadStats) stats{WmfCaptureReadStats};
    decltype(&WmfCaptureDestroy) destroyCapture{WmfCaptureDestroy};
    decltype(&WmfProductCaptureUpdate) update{WmfProductCaptureUpdate};
    decltype(&WmfProductCapturePolicy) policy{WmfProductCapturePolicy};
    decltype(&WmfProductCaptureRevision) revision{WmfProductCaptureRevision};
    decltype(&WmfProductCaptureRecording) recording{WmfProductCaptureRecording};
    bool simulation{};
};
enum class ProductRuntimeState : unsigned { Paused, Starting, Enabled, Faulted, Stopping, Stopped };
class ProductRuntime {
public:
    struct PreparedConfig { std::shared_ptr<const BindingConfig> snapshot; };
    ProductRuntime(BindingConfig,bool enabled,ProductRuntimeApi = {},std::uint32_t validatedFlags = 0);
    ProductRuntime(const ProductEnvironment&,BindingConfig,bool enabled,ProductRuntimeApi = {},std::uint32_t validatedFlags = 0);
    ~ProductRuntime();
    Result Publish(const BindingConfig&);
    Result Prepare(const BindingConfig&,PreparedConfig&);
    Result PublishPrepared(PreparedConfig&&) noexcept;
    void Enable(bool) noexcept;
    void SetFullscreenPaused(bool) noexcept;
    bool FullscreenPaused() const noexcept { return fullscreenPaused_; }
    bool ApplySavedEnabled(bool,std::uint64_t expectedControlEpoch) noexcept;
    std::uint64_t ControlEpoch() const noexcept { return controlEpoch_; }
    std::uint64_t Generation() const noexcept { return generation_; }
    void ReportFailure(Result result) noexcept { Fault(result); }
    void Stop() noexcept;
    void Record(bool) noexcept;
    bool Requested() const noexcept { return requested_; }
    bool Finished() const noexcept { return workerDone_ && captureDone_; }
    // Permanent native teardown failure requires process exit, without unloading
    // modules containing callbacks. The OS then releases the remaining resources.
    bool RequiresProcessExit() const noexcept { return abandonedCapture_; }
    ProductRuntimeState State() const noexcept;
    Result Failure() const noexcept;
    bool ReadTrace(ProductTrace& t) noexcept { return traces_.Pop(t); }
    bool ReadRecorded(InputEvent& e) noexcept { return recorded_.Pop(e); }
    std::uint64_t LostTraces() const noexcept { return lostTraces_; }
    std::uint64_t ActiveRevision() const noexcept { return activeRevision_; }
    CaptureStats Stats() const noexcept;
private:
    static bool Receive(void*,const InputEvent*) noexcept;
    bool Receive(const InputEvent&) noexcept;
    void Fault(Result) noexcept;
    void CleanupFailure(Result) noexcept;
    void Worker() noexcept;
    void Capture() noexcept;
    void Complete(std::uint64_t,bool) noexcept;
    void SetEnabled(bool) noexcept;
    bool Running() const noexcept { return !fullscreenPaused_ && (requested_ || recording_); }
    std::shared_ptr<const BindingConfig> Resolve(std::uint64_t);
    std::shared_ptr<const BindingConfig> Current();
    ProductRuntimeApi api_;
    std::uint32_t flags_{};
    ProductInputBuffer input_;
    SpscRing<ProductTrace,2048> traces_;
    SpscRing<InputEvent,32> recorded_;
    mutable std::mutex configsMutex_,statsMutex_,controlMutex_;
    std::shared_ptr<const BindingConfig> current_;
    std::unordered_map<std::uint64_t,std::weak_ptr<const BindingConfig>> configs_;
    CaptureStats captureStats_;
    HANDLE stop_{},workerWake_{},captureWake_{},deadlineTimer_{};
    std::thread workerThread_,captureThread_;
    std::atomic<CaptureHandle> capture_{};
    std::atomic<bool> requested_{},workerReady_{},workerPaused_{true},captureReady_{},workerDone_{},captureDone_{},cancel_{},captureCancel_{};
    std::atomic<bool> recording_{},recordingDirty_{};
    std::atomic<bool> fullscreenPaused_{};
    std::atomic<bool> abandonedCapture_{};
    std::atomic<std::uint64_t> generation_{1},readyGeneration_{},accepted_{},processed_{},activeRevision_{},lostTraces_{};
    std::atomic<std::uint64_t> controlEpoch_{1};
    std::atomic<Status> failureCode_{Status::Ok};
    std::atomic<bool> failureClaimed_{};
    std::atomic<std::uint32_t> nativeError_{},cleanupError_{};
};
}
