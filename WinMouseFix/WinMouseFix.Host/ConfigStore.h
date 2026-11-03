#pragma once
#include "ProductConfig.h"
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace wmf::host {
struct ConfigRead {
    Result result;
    bool exists{};
    std::string bytes;
};
class ConfigFiles {
public:
    virtual ~ConfigFiles() = default;
    virtual ConfigRead Read(bool backup) = 0;
    virtual Result Save(std::string_view bytes,std::optional<std::string_view> lastValid,bool preserveOriginal) = 0;
};
// Only used by the config service, never a hook or input worker.
class NativeConfigFiles final : public ConfigFiles {
public:
    explicit NativeConfigFiles(std::filesystem::path directory) : directory_(std::move(directory)) {}
    ConfigRead Read(bool backup) override;
    Result Save(std::string_view,std::optional<std::string_view>,bool) override;
private:
    Result WriteTemporary(const std::filesystem::path&,std::string_view);
    std::filesystem::path directory_;
    std::uint64_t serial_{};
};
struct ConfigState {
    ProductConfig config;
    bool recoveryRequired{},fromBackup{},initialFileMissing{};
    ConfigError warning;
};
class PreparedPublication {
public:
    virtual ~PreparedPublication() = default;
    virtual void Publish() noexcept = 0;
};
using PreparePublication = std::function<std::unique_ptr<PreparedPublication>(const ProductConfig&)>;
class ConfigStore {
public:
    explicit ConfigStore(std::unique_ptr<ConfigFiles>,PreparePublication = {});
    std::shared_ptr<const ConfigState> Current() const noexcept { return state_.load(); }
    ConfigError Load();
    ConfigError Commit(std::uint64_t expectedRevision,ProductConfig candidate,bool explicitRecovery = false,PreparePublication transactionPrepare = {});
private:
    std::unique_ptr<ConfigFiles> files_;
    PreparePublication prepare_;
    std::atomic<std::shared_ptr<const ConfigState>> state_;
    std::mutex writer_;
};
}
