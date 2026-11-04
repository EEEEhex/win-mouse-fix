#include "UpdatePackage.h"
#include "../../WinMouseFix.Host/JsonSupport.h"
#include "../../ThirdParty/miniz/miniz.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <cwctype>
#include <io.h>
#include <cmath>
#undef GetObject

namespace wmf::update {
namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;
namespace {
constexpr std::uint64_t MaximumExpanded = 2ull*1024*1024*1024;
void Require(bool ok,const char* error) { if (!ok) throw std::runtime_error(error); }
void Cancel(std::stop_token stop) { if (stop.stop_requested()) throw std::runtime_error("cancelled"); }
std::wstring Key(const fs::path& path) { auto value = path.generic_wstring(); std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); }); return value; }
bool HashValid(std::string_view hash) { return hash.size() == 64 && std::all_of(hash.begin(),hash.end(),[](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
void Move(const fs::path& source,const fs::path& destination) { Require(MoveFileExW(source.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"replace-failed"); }
JsonObject Object(std::string_view text) { wmf::host::EnsureJsonApartment(); return JsonObject::Parse(winrt::to_hstring(text)); }
JsonObject LoadJournal(const fs::path& tx) { return Object(ReadText(tx/L"journal.json")); }
void SaveJournal(const fs::path& tx,JsonObject journal,const wchar_t* state) { journal.SetNamedValue(L"state",JsonValue::CreateStringValue(state)); WriteText(tx/L"journal.json",winrt::to_string(journal.Stringify())); }
void Flush(const fs::path& path);
void RestoreFile(const fs::path& source,const fs::path& target,const fs::path& temporary) {
    if (fs::is_regular_file(target) && Sha256(source) == Sha256(target)) return;
    fs::create_directories(target.parent_path()); fs::copy_file(source,temporary,fs::copy_options::overwrite_existing);
    Flush(temporary);
    Move(temporary,target);
}
void Flush(const fs::path& path) {
    const auto file = CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); Require(file != INVALID_HANDLE_VALUE,"backup-flush-failed"); const auto result = FlushFileBuffers(file); CloseHandle(file); Require(result,"backup-flush-failed");
}
std::uint64_t Fingerprint(const fs::path& path) {
    std::ifstream in(path,std::ios::binary); Require(static_cast<bool>(in),"read-failed"); std::uint64_t hash=14695981039346656037ull; char bytes[65536];
    while (in) { in.read(bytes,sizeof(bytes)); for (std::streamsize i=0;i<in.gcount();++i) { hash ^= static_cast<unsigned char>(bytes[i]); hash *= 1099511628211ull; } } Require(in.eof(),"read-failed"); return hash;
}
void CarryAcceptedEvidence(const fs::path& tx,const fs::path& data) {
    const auto root = tx.parent_path(),backup = tx/L"backup";
    for (const auto name : {L"WinMouseFix.Core.dll",L"WinMouseFix.Platform.Win32.dll"}) if (Sha256(root/name) != Sha256(backup/name)) return;
    const auto oldHost=std::to_string(Fingerprint(backup/L"WinMouseFix.Host.exe")),newHost=std::to_string(Fingerprint(root/L"WinMouseFix.Host.exe"));
    const auto core=std::to_string(Fingerprint(root/L"WinMouseFix.Core.dll")),platform=std::to_string(Fingerprint(root/L"WinMouseFix.Platform.Win32.dll"));
    for (const auto name : {L"native-validation.json",L"shell-validation.json"}) {
        if (!fs::exists(data/name)) continue;
        try {
            auto record=Object(ReadText(data/name,16384)); auto identity=winrt::to_string(record.GetNamedString(L"identity"));
            std::vector<std::string> parts; std::size_t start{}; for (;;) { auto end=identity.find('/',start); parts.push_back(identity.substr(start,end-start)); if (end==std::string::npos) break; start=end+1; }
            if (record.GetNamedNumber(L"schemaVersion") != 1 || record.GetNamedBoolean(L"simulation") || parts.size()!=9 || parts[6]!=oldHost || parts[7]!=core || parts[8]!=platform) continue;
            parts[6]=newHost; identity.clear(); for (const auto& part : parts) { if (!identity.empty()) identity+='/'; identity+=part; }
            record.SetNamedValue(L"identity",JsonValue::CreateStringValue(winrt::to_hstring(identity))); record.SetNamedValue(L"updateCarryForward",JsonValue::CreateBooleanValue(true)); WriteText(data/name,winrt::to_string(record.Stringify()));
        } catch (...) { /* Missing/ineligible observations never grant a capability. */ }
    }
}
}
fs::path SafeRelative(std::string_view name) {
    Require(!name.empty() && name.size() < 240 && name.find('\0') == std::string_view::npos,"unsafe-package-path");
    auto path = fs::path(winrt::to_hstring(name).c_str()); Require(!path.is_absolute() && !path.has_root_path(),"unsafe-package-path");
    for (const auto& component : path) {
        const auto part = component.wstring(); Require(!part.empty() && part != L"." && part != L".." && part.back() != L'.' && part.back() != L' ',"unsafe-package-path");
        Require(part.find_first_of(L":<>\"|?*\r\n\t") == std::wstring::npos && std::none_of(part.begin(),part.end(),[](wchar_t c) { return c < 32; }),"unsafe-package-path");
        auto base = Key(part.substr(0,part.find(L'.')));
        Require(base != L"con" && base != L"prn" && base != L"aux" && base != L"nul" && base != L"conin$" && base != L"conout$" && !(base.size() == 4 && (base.starts_with(L"com") || base.starts_with(L"lpt")) && base[3] >= L'0' && base[3] <= L'9'),"unsafe-package-path");
    }
    const auto first = Key(*path.begin());
    Require(first != L"data" && !first.starts_with(L"data.") && !first.starts_with(L"winmousefix.storage") && !first.starts_with(L".wmf-"),"protected-package-path");
    return path.lexically_normal();
}
void CheckTree(const fs::path& path) {
    auto absolute = fs::absolute(path).lexically_normal(),current = absolute.root_path();
    for (const auto& component : absolute.relative_path()) { current /= component; const auto attributes = GetFileAttributesW(current.c_str()); if (attributes != INVALID_FILE_ATTRIBUTES) Require(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT),"reparse-point"); }
}
std::string ReadText(const fs::path& path,std::size_t limit) {
    CheckTree(path); Require(fs::file_size(path) <= limit,"file-too-large");
    std::ifstream in(path,std::ios::binary); Require(static_cast<bool>(in),"read-failed"); std::string value(std::istreambuf_iterator<char>(in),{}); Require(in.eof() || !in.fail(),"read-failed");
    if (value.starts_with("\xef\xbb\xbf")) value.erase(0,3); return value;
}
void WriteText(const fs::path& path,std::string_view value) {
    CheckTree(path); fs::create_directories(path.parent_path()); auto temporary = path; temporary += L".tmp"; CheckTree(temporary);
    HANDLE file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr); Require(file != INVALID_HANDLE_VALUE,"write-failed");
    DWORD written{}; const bool ok = value.size() < MAXDWORD && WriteFile(file,value.data(),static_cast<DWORD>(value.size()),&written,nullptr) && written == value.size() && FlushFileBuffers(file); CloseHandle(file);
    Require(ok,"write-failed"); Move(temporary,path);
}
std::string Sha256(const fs::path& path,std::stop_token stop) {
    CheckTree(path); BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0) >= 0,"hash-failed");
    struct Cleanup { BCRYPT_ALG_HANDLE& a; BCRYPT_HASH_HANDLE& h; ~Cleanup() { if (h) BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(a,0); } } cleanup{algorithm,hash};
    Require(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0) >= 0,"hash-failed");
    std::ifstream in(path,std::ios::binary); Require(static_cast<bool>(in),"read-failed"); std::array<char,65536> buffer;
    while (in) { Cancel(stop); in.read(buffer.data(),buffer.size()); Require(BCryptHashData(hash,reinterpret_cast<PUCHAR>(buffer.data()),static_cast<ULONG>(in.gcount()),0) >= 0,"hash-failed"); }
    Require(in.eof(),"read-failed"); std::array<unsigned char,32> digest; Require(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0) >= 0,"hash-failed");
    constexpr char digits[] = "0123456789abcdef"; std::string result; for (auto c : digest) { result += digits[c>>4]; result += digits[c&15]; } return result;
}
Manifest ReadManifest(const fs::path& root) {
    const auto object = Object(ReadText(root/L"manifest.json"));
    Require(object.GetNamedNumber(L"schemaVersion") == 1 && object.GetNamedString(L"architecture") == L"x64","invalid-manifest");
    Require(object.GetNamedNumber(L"updaterProtocol",1) == 1,"unsupported-updater-protocol");
    OSVERSIONINFOW os{sizeof(os)}; auto getVersion = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
    const auto minimum = object.GetNamedNumber(L"minimumOSBuild",22000); Require(getVersion && getVersion(&os) == 0 && std::isfinite(minimum) && minimum >= 22000 && minimum <= os.dwBuildNumber,"unsupported-os");
    Manifest result; result.version = winrt::to_string(object.GetNamedString(L"version")); const auto files = object.GetNamedArray(L"files"); Require(files.Size() > 0 && files.Size() <= 10000,"invalid-manifest");
    std::set<std::wstring> names; std::uint64_t expanded{};
    for (const auto& value : files) {
        auto entry = value.GetObject(); File file; file.path = SafeRelative(winrt::to_string(entry.GetNamedString(L"path"))); file.sha256 = winrt::to_string(entry.GetNamedString(L"sha256"));
        const auto size = entry.GetNamedNumber(L"bytes"); Require(std::isfinite(size) && size >= 0 && size <= MaximumExpanded && std::floor(size) == size,"invalid-manifest"); file.bytes = static_cast<std::uint64_t>(size);
        Require(HashValid(file.sha256) && Key(file.path) != L"manifest.json" && names.insert(Key(file.path)).second,"invalid-manifest"); expanded += file.bytes; Require(expanded <= MaximumExpanded,"package-too-large"); result.files.push_back(std::move(file));
    }
    return result;
}
Manifest VerifyPackage(const fs::path& root,std::string_view expected,std::stop_token stop,Progress progress) {
    CheckTree(root); auto manifest = ReadManifest(root); Require(manifest.version == expected,"version-mismatch");
    for (const auto required : {L"WinMouseFix.Host.exe",L"WinMouseFix.Core.dll",L"WinMouseFix.Platform.Win32.dll",L"WinMouseFix.Updater.exe",L"Settings/WinMouseFix.Settings.exe",L"Settings/WinMouseFix.Settings.dll"})
        Require(std::any_of(manifest.files.begin(),manifest.files.end(),[&](const File& file) { return Key(file.path) == Key(required); }),"incomplete-package");
    std::set<std::wstring> files{L"manifest.json"}; std::uint64_t done{};
    for (const auto& file : manifest.files) { Cancel(stop); auto path = root/file.path; CheckTree(path); Require(fs::is_regular_file(path) && fs::file_size(path) == file.bytes && Sha256(path,stop) == file.sha256,"package-integrity"); files.insert(Key(file.path)); if (progress) progress(++done,manifest.files.size()); }
    for (const auto& item : fs::recursive_directory_iterator(root)) { CheckTree(item.path()); if (!item.is_directory()) Require(files.contains(Key(fs::relative(item.path(),root))),"unlisted-package-file"); }
    return manifest;
}
void ExtractPackage(const fs::path& zip,const fs::path& destination,std::stop_token stop,Progress progress) {
    CheckTree(zip); CheckTree(destination); Require(!fs::exists(destination),"stage-exists");
    FILE* file{}; Require(_wfopen_s(&file,zip.c_str(),L"rb") == 0,"read-failed");
    mz_zip_archive archive{}; if (!mz_zip_reader_init_cfile(&archive,file,0,0)) { fclose(file); throw std::runtime_error("invalid-zip"); }
    struct Close { mz_zip_archive& a; FILE* f; ~Close() { mz_zip_reader_end(&a); fclose(f); } } close{archive,file};
    const auto count = mz_zip_reader_get_num_files(&archive); Require(count > 0 && count <= 10000,"invalid-zip");
    std::set<std::wstring> names; std::uint64_t size{};
    // Validate all paths and limits before creating any extracted file.
    for (mz_uint i = 0; i < count; ++i) {
        Cancel(stop); mz_zip_archive_file_stat info{}; Require(mz_zip_reader_file_stat(&archive,i,&info),"invalid-zip");
        std::string name = info.m_filename; if (info.m_is_directory && !name.empty()) name.pop_back(); const auto path = SafeRelative(name);
        Require(names.insert(Key(path)).second && !info.m_is_encrypted && mz_zip_reader_is_file_supported(&archive,i) && ((info.m_external_attr>>16)&0170000) != 0120000,"invalid-zip");
        Require(info.m_uncomp_size <= MaximumExpanded-size,"package-too-large"); size += info.m_uncomp_size;
    }
    Require(fs::space(destination.parent_path()).available >= size+16*1024*1024,"insufficient-space");
    fs::create_directories(destination);
    for (mz_uint i = 0; i < count; ++i) {
        Cancel(stop); mz_zip_archive_file_stat info{}; Require(mz_zip_reader_file_stat(&archive,i,&info),"invalid-zip");
        std::string name = info.m_filename; if (info.m_is_directory) { name.pop_back(); fs::create_directories(destination/SafeRelative(name)); continue; }
        const auto target = destination/SafeRelative(name); CheckTree(target); fs::create_directories(target.parent_path());
        FILE* output{}; Require(_wfopen_s(&output,target.c_str(),L"wb") == 0,"write-failed");
        const bool extracted = mz_zip_reader_extract_to_cfile(&archive,i,output,0) != 0; const bool flushed = fflush(output) == 0 && FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(output)))); fclose(output);
        Require(extracted && flushed,"invalid-zip"); if (progress) progress(i+1,count);
    }
}
void CheckTransactionPath(const fs::path& tx) { Require(tx.is_absolute() && tx.filename().wstring().starts_with(L".wmf-update-") && tx.filename().wstring().size() > 12,"invalid-transaction"); CheckTree(tx); }
fs::path PendingTransaction(const fs::path& root) {
    auto name = ReadText(root/L".wmf-update.json",4096); const auto object = wmf::host::ParseJsonObject(name); auto leaf = fs::path(object.GetNamedString(L"transaction").c_str());
    Require(leaf == leaf.filename(),"invalid-transaction"); auto tx = root/leaf; CheckTransactionPath(tx); return tx;
}
bool TransactionPending(const fs::path& root) { return fs::exists(root/L".wmf-update.json"); }
void PrepareTransaction(const fs::path& tx,const fs::path& data) {
    CheckTransactionPath(tx); const auto root = tx.parent_path(); Require(!TransactionPending(root),"update-already-pending");
    auto incoming = ReadManifest(tx/L"payload"); VerifyPackage(tx/L"payload",incoming.version);
    std::set<std::wstring> names; std::vector<fs::path> paths;
    auto add = [&](const fs::path& path) { if (names.insert(Key(path)).second) paths.push_back(path); };
    for (const auto& file : ReadManifest(root).files) add(file.path);
    for (const auto& file : incoming.files) add(file.path); add(L"manifest.json");
    Require(paths.size() <= 10000,"package-too-large");
    std::uint64_t needed=16*1024*1024,largest{}; for (const auto& path : paths) if (fs::is_regular_file(root/path)) { const auto bytes=fs::file_size(root/path); needed += bytes; largest=(std::max)(largest,bytes); }
    for (const auto& file : incoming.files) largest=(std::max)(largest,file.bytes);
    Require(fs::space(root).available >= needed+largest,"insufficient-space");
    JsonArray entries; const auto backup = tx/L"backup"; fs::create_directories(backup);
    for (const auto& path : paths) {
        const auto target = root/path; CheckTree(target); JsonObject entry; entry.SetNamedValue(L"path",JsonValue::CreateStringValue(path.wstring())); const bool existed = fs::exists(target);
        entry.SetNamedValue(L"existed",JsonValue::CreateBooleanValue(existed));
        if (existed) { Require(fs::is_regular_file(target),"invalid-installation"); fs::create_directories((backup/path).parent_path()); fs::copy_file(target,backup/path); Flush(backup/path); Require(Sha256(target) == Sha256(backup/path),"backup-failed"); }
        entries.Append(entry);
    }
    // Configuration snapshots are restored only on rollback, never overwritten by the release.
    JsonArray settings;
    for (const auto name : {L"config.json",L"config.backup.json",L"Settings/ui.json",L"native-validation.json",L"shell-validation.json"}) {
        auto source = data/name; CheckTree(source); JsonObject entry; entry.SetNamedValue(L"path",JsonValue::CreateStringValue(name)); const bool exists = fs::exists(source); entry.SetNamedValue(L"existed",JsonValue::CreateBooleanValue(exists)); settings.Append(entry);
        if (exists) { auto target = tx/L"configuration"/name; fs::create_directories(target.parent_path()); fs::copy_file(source,target); Flush(target); Require(Sha256(source) == Sha256(target),"backup-failed"); }
    }
    JsonObject journal; journal.SetNamedValue(L"files",entries); journal.SetNamedValue(L"settings",settings); journal.SetNamedValue(L"data",JsonValue::CreateStringValue(data.wstring())); journal.SetNamedValue(L"version",JsonValue::CreateStringValue(winrt::to_hstring(incoming.version)));
    SaveJournal(tx,journal,L"prepared"); JsonObject marker; marker.SetNamedValue(L"transaction",JsonValue::CreateStringValue(tx.filename().wstring())); WriteText(root/L".wmf-update.json",winrt::to_string(marker.Stringify()));
}
void ApplyTransaction(const fs::path& tx,Progress progress) {
    CheckTransactionPath(tx); auto journal = LoadJournal(tx); const auto root = tx.parent_path(); auto incoming = VerifyPackage(tx/L"payload",winrt::to_string(journal.GetNamedString(L"version")));
    SaveJournal(tx,journal,L"applying"); std::set<std::wstring> next{L"manifest.json"}; for (const auto& file : incoming.files) next.insert(Key(file.path));
    auto entries = journal.GetNamedArray(L"files"); std::uint64_t done{};
    for (const auto& value : entries) {
        auto path = SafeRelative(winrt::to_string(value.GetObject().GetNamedString(L"path"))); auto target = root/path; CheckTree(target);
        if (next.contains(Key(path))) { fs::create_directories(target.parent_path()); auto source = tx/L"payload"/path; auto temporary = tx/L"replace.tmp"; fs::copy_file(source,temporary,fs::copy_options::overwrite_existing); Flush(temporary); Move(temporary,target); }
        else if (fs::exists(target)) Require(fs::remove(target),"replace-failed");
        if (progress) progress(++done,entries.Size());
    }
    CarryAcceptedEvidence(tx,fs::path(journal.GetNamedString(L"data").c_str()));
    SaveJournal(tx,journal,L"health-check");
}
void RollbackTransaction(const fs::path& tx,Progress progress) {
    CheckTransactionPath(tx); auto journal = LoadJournal(tx); const auto root = tx.parent_path();
    std::error_code ignored; fs::remove(tx/L"replace.tmp",ignored); fs::remove(tx/L"restore.tmp",ignored);
    JsonObject marker; marker.SetNamedValue(L"transaction",JsonValue::CreateStringValue(tx.filename().wstring())); WriteText(root/L".wmf-update.json",winrt::to_string(marker.Stringify())); SaveJournal(tx,journal,L"rolling-back");
    auto entries = journal.GetNamedArray(L"files"); std::uint64_t done{};
    for (const auto& value : entries) { auto entry = value.GetObject(); auto path = SafeRelative(winrt::to_string(entry.GetNamedString(L"path"))); auto target = root/path; CheckTree(target);
        if (entry.GetNamedBoolean(L"existed")) RestoreFile(tx/L"backup"/path,target,tx/L"restore.tmp"); else fs::remove(target); if (progress) progress(++done,entries.Size()); }
    const fs::path data{journal.GetNamedString(L"data").c_str()}; CheckTree(data);
    for (const auto& value : journal.GetNamedArray(L"settings")) { auto entry = value.GetObject(); const auto path = SafeRelative(winrt::to_string(entry.GetNamedString(L"path"))); auto target = data/path; CheckTree(target); if (entry.GetNamedBoolean(L"existed")) RestoreFile(tx/L"configuration"/path,target,tx/L"restore.tmp"); else fs::remove(target); }
    SaveJournal(tx,journal,L"rolled-back"); fs::remove(root/L".wmf-update.json");
}
void CommitTransaction(const fs::path& tx) { CheckTransactionPath(tx); auto journal = LoadJournal(tx); SaveJournal(tx,journal,L"completed"); fs::remove(tx.parent_path()/L".wmf-update.json"); }
void CleanupTransactions(const fs::path& root) noexcept {
    try {
        CheckTree(root); if (TransactionPending(root)) return;
        HANDLE lock=CreateFileW((root/L".wmf-update.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_HIDDEN,nullptr); if (lock==INVALID_HANDLE_VALUE) return;
        struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{lock};
        for (const auto& entry : fs::directory_iterator(root)) {
            if (!entry.is_directory() || !entry.path().filename().wstring().starts_with(L".wmf-update-") || !fs::is_regular_file(entry.path()/L"journal.json")) continue;
            try { CheckTransactionPath(entry.path()); auto state=LoadJournal(entry.path()).GetNamedString(L"state"); if (state!=L"completed" && state!=L"rolled-back") continue; for (const auto& child : fs::recursive_directory_iterator(entry.path())) CheckTree(child.path()); fs::remove_all(entry.path()); } catch (...) {}
        }
    } catch (...) {}
}
}
