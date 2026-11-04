#include "DataLocation.h"
#include "JsonSupport.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace wmf::host {
namespace {
bool Inside(const std::filesystem::path& child,const std::filesystem::path& parent) {
    const auto c = std::filesystem::weakly_canonical(child).wstring(),p = std::filesystem::weakly_canonical(parent).wstring();
    return c.size() >= p.size() && !_wcsnicmp(c.c_str(),p.c_str(),p.size()) && (c.size() == p.size() || c[p.size()] == L'\\' || c[p.size()] == L'/');
}
void Move(const std::filesystem::path& from,const std::filesystem::path& to) {
    if (!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)) throw std::system_error(GetLastError(),std::system_category());
}
void CheckTree(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) return;
    if (!std::filesystem::is_directory(root)) throw std::invalid_argument("Data path is not a directory");
    const auto check = [&](const std::filesystem::path& path) {
        const auto flags = GetFileAttributesW(path.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::invalid_argument("Data migration does not follow reparse points");
    };
    check(root); for (const auto& item : std::filesystem::recursive_directory_iterator(root)) check(item.path());
}
void CopyVerified(const std::filesystem::path& from,const std::filesystem::path& to) {
    std::filesystem::create_directory(to);
    if (!std::filesystem::exists(from)) return;
    std::vector<char> a(65536),b(65536);
    for (const auto& item : std::filesystem::recursive_directory_iterator(from)) {
        const auto target = to/std::filesystem::relative(item.path(),from);
        if (item.is_directory()) { std::filesystem::create_directories(target); continue; }
        if (!item.is_regular_file()) throw std::invalid_argument("Unsupported data file");
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::copy_file(item.path(),target);
        const auto attributes = GetFileAttributesW(target.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(target.c_str(),attributes & ~FILE_ATTRIBUTE_READONLY)) throw std::system_error(GetLastError(),std::system_category());
        HANDLE copied = CreateFileW(target.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (copied == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(),std::system_category());
        const bool flushed = FlushFileBuffers(copied); const auto flushError = GetLastError(); CloseHandle(copied);
        if (!flushed) throw std::system_error(flushError,std::system_category());
        std::ifstream first(item.path(),std::ios::binary),second(target,std::ios::binary);
        if (!first || !second) throw std::runtime_error("Cannot verify copied data");
        do { first.read(a.data(),a.size()); second.read(b.data(),b.size()); if (first.gcount() != second.gcount() || !std::equal(a.begin(),a.begin()+first.gcount(),b.begin())) throw std::runtime_error("Data verification failed"); } while (first.gcount());
        if (first.bad() || second.bad()) throw std::runtime_error("Data verification failed");
    }
}
void SaveMarker(const std::filesystem::path& path,DataLocation location,const std::filesystem::path& temporary) {
    const auto json = std::string("{\"schemaVersion\":1,\"location\":\"")+std::string(DataLocationName(location))+"\"}";
    HANDLE file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(),std::system_category());
    DWORD written{}; const bool saved = WriteFile(file,json.data(),static_cast<DWORD>(json.size()),&written,nullptr) && written == json.size() && FlushFileBuffers(file);
    const auto error = GetLastError(); CloseHandle(file);
    if (!saved) throw std::system_error(error ? error : ERROR_WRITE_FAULT,std::system_category());
    if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::system_error(GetLastError(),std::system_category());
}
}
std::string_view DataLocationName(DataLocation location) noexcept { return location == DataLocation::Program ? "program" : "appdata"; }
DataLocation ReadDataLocation(const DataPaths& paths) {
    if (!std::filesystem::exists(paths.marker)) return DataLocation::AppData;
    if (std::filesystem::file_size(paths.marker) > 4096) throw std::runtime_error("Configuration location marker is invalid: WinMouseFix.storage.json");
    std::ifstream file(paths.marker,std::ios::binary); if (!file) throw std::runtime_error("Cannot read WinMouseFix.storage.json");
    const std::string bytes{std::istreambuf_iterator<char>(file),{}};
    try {
        const auto json = ParseJsonObject(bytes);
        if (json.Size() != 2 || JsonUnsigned(json,L"schemaVersion") != 1) throw std::invalid_argument("Invalid marker");
        const auto mode = json.GetNamedString(L"location"); if (mode == L"program") return DataLocation::Program; if (mode == L"appdata") return DataLocation::AppData;
    } catch (...) { throw std::runtime_error("Configuration location marker is invalid: WinMouseFix.storage.json"); }
    throw std::runtime_error("Configuration location marker is invalid: WinMouseFix.storage.json");
}
DataMigration MigrateData(const DataPaths& paths,DataLocation from,DataLocation to,const std::function<void(std::string_view)>& checkpoint) {
    DataMigration output; if (from == to) return output;
    const auto source = paths.Directory(from),target = paths.Directory(to);
    bool oldTargetMoved{},newTargetInstalled{},committed{};
    static std::atomic<std::uint64_t> serial{};
    const auto suffix = std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++serial);
    const auto staging = target.parent_path()/(target.filename().wstring()+L".migrating-"+suffix);
    output.targetBackup = target.parent_path()/(target.filename().wstring()+L".backup-"+suffix);
    output.sourceBackup = source.parent_path()/(source.filename().wstring()+L".migrated-"+suffix);
    const auto markerTemporary = paths.marker.parent_path()/(L"WinMouseFix.storage."+suffix+L".tmp");
    try {
        if (Inside(source,target) || Inside(target,source) || Inside(paths.marker,source) || Inside(paths.marker,target)) throw std::invalid_argument("Configuration directories overlap");
        if (ReadDataLocation(paths) != from) throw std::invalid_argument("Configuration location changed before migration");
        CheckTree(source); CheckTree(target);
        std::filesystem::create_directories(target.parent_path());
        CopyVerified(source,staging); if (checkpoint) checkpoint("copied");
        if (std::filesystem::exists(target)) { Move(target,output.targetBackup); oldTargetMoved = true; } else output.targetBackup.clear();
        Move(staging,target); newTargetInstalled = true;
        if (checkpoint) checkpoint("before-marker");
        SaveMarker(paths.marker,to,markerTemporary); committed = true;
    } catch (const std::system_error& error) { output.result = {Status::NativeFailure,static_cast<std::uint32_t>(error.code().value())}; }
    catch (const std::invalid_argument&) { output.result = {Status::InvalidArgument,ERROR_INVALID_DATA}; }
    catch (...) { output.result = {Status::NativeFailure,ERROR_WRITE_FAULT}; }
    if (!committed) {
        // All cleanup paths are created beside the known data roots, never
        // supplied by a client. Renaming keeps every source/destination copy.
        try {
            if (newTargetInstalled) Move(target,staging);
            if (oldTargetMoved) Move(output.targetBackup,target);
        } catch (const std::system_error& error) { output.result.cleanupError = static_cast<std::uint32_t>(error.code().value()); }
        DeleteFileW(markerTemporary.c_str());
        return output;
    }
    if (std::filesystem::exists(source)) {
        try { Move(source,output.sourceBackup); }
        catch (...) { output.sourceRetained = true; output.sourceBackup.clear(); }
    } else output.sourceBackup.clear();
    return output;
}
}
