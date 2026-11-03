#include "ConfigStore.h"
#include <array>

namespace wmf::host {
namespace {
Result LastError() noexcept { const auto code = GetLastError(); return {Status::NativeFailure,code ? code : ERROR_GEN_FAILURE}; }
ConfigError StorageError(Result result) { return {ConfigProblem::Storage,"config.json","Configuration was not saved",0,0,result}; }
struct File {
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit File(HANDLE h) : handle(h) {}
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File(const File&) = delete;
};
}
ConfigRead NativeConfigFiles::Read(bool backup) {
    ConfigRead read;
    const auto path = directory_/(backup ? L"config.backup.json" : L"config.json");
    File file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if (file.handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) read.result = {Status::NativeFailure,error};
        return read;
    }
    read.exists = true; LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle,&size)) { read.result = LastError(); return read; }
    if (size.QuadPart < 0 || size.QuadPart > MaximumConfigBytes) { read.result = {Status::OutOfRange}; return read; }
    read.bytes.resize(static_cast<std::size_t>(size.QuadPart)); DWORD done{};
    while (done < read.bytes.size()) {
        DWORD amount{};
        if (!ReadFile(file.handle,read.bytes.data()+done,static_cast<DWORD>(read.bytes.size()-done),&amount,nullptr)) { read.result = LastError(); return read; }
        if (!amount) { read.result = {Status::NativeFailure,ERROR_HANDLE_EOF}; return read; } done += amount;
    }
    return read;
}
Result NativeConfigFiles::WriteTemporary(const std::filesystem::path& path,std::string_view bytes) {
    File file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
    if (file.handle == INVALID_HANDLE_VALUE) return LastError();
    DWORD done{};
    while (done < bytes.size()) {
        DWORD amount{};
        if (!WriteFile(file.handle,bytes.data()+done,static_cast<DWORD>(bytes.size()-done),&amount,nullptr)) return LastError();
        if (!amount) return {Status::NativeFailure,ERROR_WRITE_FAULT}; done += amount;
    }
    if (!FlushFileBuffers(file.handle)) return LastError();
    const auto handle = file.handle; file.handle = INVALID_HANDLE_VALUE;
    return CloseHandle(handle) ? Result{} : LastError();
}
Result NativeConfigFiles::Save(std::string_view bytes,std::optional<std::string_view> lastValid,bool preserve) {
    if (bytes.size() > MaximumConfigBytes) return {Status::OutOfRange};
    std::filesystem::create_directories(directory_);
    const auto active = directory_/L"config.json",backup = directory_/L"config.backup.json";
    const auto suffix = std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(++serial_);
    const auto temporary = directory_/(L"config."+suffix+L".tmp");
    auto result = WriteTemporary(temporary,bytes);
    if (!result) { DeleteFileW(temporary.c_str()); return result; }
    if (lastValid) {
        const auto backupTemporary = directory_/(L"backup."+suffix+L".tmp");
        result = WriteTemporary(backupTemporary,*lastValid);
        if (result && !MoveFileExW(backupTemporary.c_str(),backup.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) result = LastError();
        if (!result) { DeleteFileW(backupTemporary.c_str()); DeleteFileW(temporary.c_str()); return result; }
    }
    if (preserve && GetFileAttributesW(active.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Recovery is an explicit user operation. Preserve the original before
        // replacing it, including an unknown newer schema.
        const auto original = directory_/(L"config.preserved."+suffix+L".json");
        if (!CopyFileW(active.c_str(),original.c_str(),TRUE)) { result = LastError(); DeleteFileW(temporary.c_str()); return result; }
    }
    // Same-directory rename never requests cross-volume copy. The complete,
    // flushed new file is published only after the valid backup is durable.
    if (!MoveFileExW(temporary.c_str(),active.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) result = LastError();
    if (!result) DeleteFileW(temporary.c_str());
    return result;
}
ConfigStore::ConfigStore(std::unique_ptr<ConfigFiles> files,PreparePublication prepare) : files_(std::move(files)),prepare_(std::move(prepare)) {
    auto state = std::make_shared<ConfigState>(); state->config = DefaultProductConfig(); state_ = std::move(state);
}
ConfigError ConfigStore::Load() {
    std::lock_guard lock(writer_);
    auto state = std::make_shared<ConfigState>(); state->config = DefaultProductConfig();
    const auto file = files_->Read(false);
    if (file.result && file.exists) {
        const auto parsed = ParseConfig(file.bytes,state->config);
        if (parsed) { state_ = state; return {}; }
        state->warning = parsed;
    } else if (!file.result) state->warning = StorageError(file.result);
    const auto backup = files_->Read(true); ProductConfig recovered;
    if (backup.result && backup.exists && ParseConfig(backup.bytes,recovered)) { state->config = recovered; state->fromBackup = true; }
    if (!file.exists && file.result && !backup.exists && backup.result) {
        state->initialFileMissing = true; state_ = state; return {};
    }
    state->recoveryRequired = true;
    if (state->warning) state->warning = {ConfigProblem::RecoveryRequired,"config.json","Configuration missing; a backup requires explicit recovery"};
    state_ = state; return state->warning;
}
ConfigError ConfigStore::Commit(std::uint64_t expectedRevision,ProductConfig candidate,bool recovery,PreparePublication transactionPrepare) {
    std::lock_guard lock(writer_);
    try {
        const auto previous = state_.load();
        if (expectedRevision != previous->config.input.revision) return {ConfigProblem::Conflict,"revision","Configuration changed; reload the current revision"};
        if (previous->recoveryRequired && !recovery) return {ConfigProblem::RecoveryRequired,"config.json","Original file is preserved; explicit recovery is required"};
        if (expectedRevision >= 9007199254740991ull) return {ConfigProblem::Validation,"revision","Configuration revision limit reached"};
        candidate.input.revision = expectedRevision+1;
        const auto valid = ValidateConfig(candidate); if (!valid) return valid;
        const auto json = SerializeConfig(candidate);
        std::optional<std::string> backup;
        if (!previous->initialFileMissing || previous->fromBackup) backup = SerializeConfig(previous->config);
        auto next = std::make_shared<ConfigState>(); next->config = candidate;
        auto publication = transactionPrepare ? transactionPrepare(candidate) : prepare_ ? prepare_(candidate) : nullptr; // Allocate and validate before disk commit.
        const auto saved = files_->Save(json,backup ? std::optional<std::string_view>(*backup) : std::nullopt,previous->recoveryRequired);
        if (!saved) return StorageError(saved);
        state_ = std::move(next);
        if (publication) publication->Publish();
        return {};
    } catch (...) { return StorageError({Status::NativeFailure,ERROR_UNHANDLED_EXCEPTION}); }
}
}
