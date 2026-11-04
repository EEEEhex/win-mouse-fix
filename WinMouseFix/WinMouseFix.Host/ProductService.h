#pragma once
#include "ConfigCoordinator.h"
#include "ControlPipe.h"
#include "ProductProtocol.h"
#include "StartupRegistration.h"

namespace wmf::host {
struct ProductServiceApi {
    bool simulation{};
    std::uint32_t validatedFlags{};
    void* context{};
    std::uint64_t (*ticks)(void*) noexcept{};
    decltype(&WmfPlatformCapabilities) capabilities{WmfPlatformCapabilities};
    std::function<void()> openSettings,requestExit;
    std::function<void(DWORD,std::uint64_t)> settingsWindow;
    std::function<void(DWORD)> settingsReady;
    std::function<bool(DWORD)> isSettingsProcess;
    std::function<std::filesystem::path()> exportDiagnostics;
    std::function<void()> startTrace;
    std::function<std::uint32_t()> logError;
    std::function<void(std::string_view,const ProtocolReply&)> observe;
    std::function<std::string(const BindingConfig&)> shellObservations;
    std::function<void(const BindingConfig&,std::string_view)> recordShell;
    std::function<std::string()> updateStatus;
    std::function<void()> checkUpdates;
    std::function<void(std::uint64_t)> acknowledgeUpdate;
    std::function<bool(std::uint64_t)> downloadUpdate;
    std::function<bool()> cancelUpdate,installUpdate,updateBusy;
    std::function<Result(DataLocation)> switchDataLocation;
};
class ProductService {
public:
    ProductService(ProductEnvironment,ConfigStore&,ProductRuntime&,StartupRegistration&,ProductServiceApi = {});
    ~ProductService();
    Result Start();
    void Stop() noexcept;
    void Tick(); // One host UI/control owner drains the recorded-button SPSC.
    void BeginExit(bool waitForClientAck = false) noexcept;
    bool MayClose() const noexcept;
    bool ExitRequested() const noexcept { return exiting_; }
    std::string Handle(std::string_view,std::uint64_t,DWORD clientPid = 0);
    std::string StatusJson();
    void Disconnected(std::uint64_t) noexcept;
    std::future<ConfigError> SetEnabled(bool value);
    bool Recording() const noexcept;
    const std::string& RunId() const noexcept { return protocol_.RunId(); }
    void DataLocationSwitchFailed(Result error) noexcept { storageError_ = error.nativeError ? error.nativeError : ERROR_GEN_FAILURE; storageSwitchPending_ = false; }
private:
    ProtocolReply Dispatch(const ProtocolRequest&);
    ProtocolReply DispatchInner(const ProtocolRequest&);
    ProtocolReply ConfigResponse(ConfigError = {});
    ProtocolReply StatusResponse();
    ConfigError Gate(const ProductConfig&) const;
    std::uint64_t Now() const noexcept;
    void EndRecording() noexcept;
    ProductEnvironment environment_;
    ConfigStore& store_;
    ProductRuntime& runtime_;
    StartupRegistration& startup_;
    ProductServiceApi api_;
    ConfigCoordinator coordinator_;
    ProductProtocol protocol_;
    std::unique_ptr<ControlPipe> pipe_;
    Capabilities capabilities_;
    Result capabilityQuery_;
    mutable std::mutex leaseMutex_;
    std::uint64_t leaseOwner_{},leaseUntil_{},leaseGeneration_{},recordedSerial_{};
    InputEvent recorded_;
    std::atomic<bool> exiting_{},exitAcked_{};
    std::atomic<std::uint64_t> exitUntil_{};
    std::atomic<std::uint64_t> statusSerial_{};
    std::atomic<bool> storageSwitchPending_{};
    std::atomic<std::uint32_t> storageError_{};
};
}
