#include "UpdateService.h"
#include "ProductVersion.h"
#include "JsonSupport.h"
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <array>
#include <objbase.h>
#undef GetObject

namespace wmf::host {
namespace {
using namespace winrt::Windows::Data::Json;
constexpr std::string_view ReleasePrefix = "https://github.com/EEEEhex/win-mouse-fix/releases/tag/";
std::string Bounded(const winrt::hstring& text,std::size_t limit) {
    auto value = winrt::to_string(text); if (value.size() <= limit) return value;
    auto end = limit; while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
    value.resize(end); value += "\n…"; return value;
}
struct Handle {
    HINTERNET value{};
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
};
struct AsyncRequest {
    HANDLE event{CreateEventW(nullptr,FALSE,FALSE,nullptr)},closed{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    HINTERNET request{};
    DWORD stage{},error{},bytes{};
    static void CALLBACK Callback(HINTERNET,DWORD_PTR context,DWORD status,void* info,DWORD size) {
        auto* self = reinterpret_cast<AsyncRequest*>(context); if (!self) return;
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent(self->closed); return; }
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) self->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
        else if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && size == sizeof(DWORD)) self->bytes = *static_cast<DWORD*>(info);
        else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self->bytes = size;
        else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE && status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
        self->stage = status; SetEvent(self->event);
    }
    void Wait(DWORD expected,HANDLE cancel,ULONGLONG deadline) {
        HANDLE handles[]{event,cancel}; auto now = GetTickCount64();
        auto wait = WaitForMultipleObjects(2,handles,FALSE,now >= deadline ? 0 : static_cast<DWORD>(deadline-now));
        if (wait != WAIT_OBJECT_0) throw std::runtime_error(wait == WAIT_OBJECT_0+1 ? "cancelled" : "network-timeout");
        if (error || stage != expected) throw std::runtime_error("network-error");
    }
    ~AsyncRequest() {
        // HANDLE_CLOSING is the final callback; keep its context alive until then.
        if (request) { WinHttpCloseHandle(request); WaitForSingleObject(closed,INFINITE); }
        if (event) CloseHandle(event); if (closed) CloseHandle(closed);
    }
};
void Require(BOOL value) { if (!value) throw std::runtime_error("network-error"); }
std::string UtcNow() {
    SYSTEMTIME time{}; GetSystemTime(&time); char value[32]{};
    sprintf_s(value,"%04u-%02u-%02uT%02u:%02u:%02uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond); return value;
}
}
std::string ReleaseVersion::Text() const { return std::to_string(parts[0])+"."+std::to_string(parts[1])+"."+std::to_string(parts[2]); }
bool ParseReleaseTitle(std::string_view title,ReleaseVersion& version) {
    // The release title (name), never tag_name, is the version authority.
    static const std::regex pattern(R"((?:^|[\s(])v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:\+[0-9A-Za-z.-]+)?(?=$|[\s)]))",std::regex::icase);
    if (title.empty()) return false;
    std::cmatch match; if (!std::regex_search(title.data(),title.data()+title.size(),match,pattern)) return false;
    ReleaseVersion result;
    for (unsigned i = 0; i < 3; ++i) { const auto& part = match[i+1]; if (std::from_chars(part.first,part.second,result.parts[i]).ec != std::errc{}) return false; }
    version = result; return true;
}
ReleasePage ParseReleasePage(std::string_view text) {
    EnsureJsonApartment(); const auto array = JsonArray::Parse(winrt::to_hstring(text)); ReleasePage page; page.count = array.Size();
    if (page.count > 100) throw std::runtime_error("invalid-response");
    for (const auto& value : array) {
        const auto item = value.GetObject(); if (item.GetNamedBoolean(L"draft",false) || item.GetNamedBoolean(L"prerelease",false)) continue;
        ReleaseInfo release; const auto nameValue = item.GetNamedValue(L"name",JsonValue::CreateNullValue());
        if (nameValue.ValueType() == JsonValueType::Null) continue;
        const auto name = nameValue.GetString();
        if (name.size() > 512 || !ParseReleaseTitle(winrt::to_string(name),release.version)) continue;
        release.title = winrt::to_string(name);
        release.url = winrt::to_string(item.GetNamedString(L"html_url",L""));
        if (!release.url.starts_with(ReleasePrefix) || release.url.size() > 2048 || release.url.find_first_of("\r\n\t") != std::string::npos) throw std::runtime_error("invalid-response");
        const auto body = item.GetNamedValue(L"body",JsonValue::CreateNullValue());
        release.notes = body.ValueType() == JsonValueType::Null ? "" : Bounded(body.GetString(),12000);
        for (auto& character : release.notes) if (static_cast<unsigned char>(character) < 32 && character != '\n' && character != '\r' && character != '\t') character = ' ';
        const auto filename = "WinMouseFix-v"+release.version.Text()+"-win-x64.zip";
        for (const auto& assetValue : item.GetNamedArray(L"assets",JsonArray{})) {
            const auto asset = assetValue.GetObject(); const auto assetName = winrt::to_string(asset.GetNamedString(L"name",L""));
            if (assetName != filename && assetName != filename+".sha256") continue;
            const auto url = winrt::to_string(asset.GetNamedString(L"browser_download_url",L""));
            if (!url.starts_with("https://github.com/EEEEhex/win-mouse-fix/releases/download/") || url.size() > 2048 || url.find_first_of("\r\n\t") != std::string::npos) continue;
            if (assetName == filename) {
                const auto bytes = asset.GetNamedNumber(L"size",0); if (!std::isfinite(bytes) || bytes <= 0 || bytes > 512ull*1024*1024 || std::floor(bytes) != bytes) continue;
                release.assetUrl = url; release.assetBytes = static_cast<std::uint64_t>(bytes);
                const auto digestValue=asset.GetNamedValue(L"digest",JsonValue::CreateNullValue());
                auto digest = digestValue.ValueType()==JsonValueType::String ? winrt::to_string(digestValue.GetString()) : std::string{}; if (digest.starts_with("sha256:")) digest.erase(0,7);
                if (digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos) release.digest = digest;
            } else release.checksumUrl = url;
        }
        if (!page.found || release.version > page.best.version) { page.found = true; page.best = std::move(release); }
    }
    return page;
}
std::string FetchGithubReleases(unsigned page,std::stop_token stop) {
    // Automatic proxy mode includes current-user static proxy, PAC and WPAD.
    Handle session{WinHttpOpen(L"WinMouseFix/2.0.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC)};
    if (!session.value) throw std::runtime_error("network-error");
    Require(WinHttpSetTimeouts(session,5000,5000,10000,10000));
    Handle connection{WinHttpConnect(session,L"api.github.com",INTERNET_DEFAULT_HTTPS_PORT,0)};
    if (!connection.value) throw std::runtime_error("network-error");
    AsyncRequest operation; if (!operation.event || !operation.closed) throw std::runtime_error("network-error");
    const auto path = L"/repos/EEEEhex/win-mouse-fix/releases?per_page=100&page="+std::to_wstring(page);
    auto request = WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
    if (!request) throw std::runtime_error("network-error");
    if (WinHttpSetStatusCallback(request,AsyncRequest::Callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0) == WINHTTP_INVALID_STATUS_CALLBACK) { WinHttpCloseHandle(request); throw std::runtime_error("network-error"); }
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&operation);
    if (!WinHttpSetOption(request,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))) { WinHttpCloseHandle(request); throw std::runtime_error("network-error"); }
    operation.request = request;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; Require(WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)));
    HANDLE cancel = CreateEventW(nullptr,TRUE,FALSE,nullptr); if (!cancel) throw std::runtime_error("network-error");
    struct EventCloser { HANDLE handle; ~EventCloser() { CloseHandle(handle); } } cancelCloser{cancel};
    std::stop_callback cancelled(stop,[cancel] { SetEvent(cancel); }); const auto deadline = GetTickCount64()+20000;
    if (stop.stop_requested()) throw std::runtime_error("cancelled");
    constexpr auto headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    Require(WinHttpSendRequest(request,headers,static_cast<DWORD>(-1),WINHTTP_NO_REQUEST_DATA,0,0,context));
    operation.Wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,cancel,deadline);
    Require(WinHttpReceiveResponse(request,nullptr)); operation.Wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,cancel,deadline);
    DWORD status{},size = sizeof(status); Require(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX));
    if (status == 403 || status == 429) throw std::runtime_error("rate-limit");
    if (status != 200) throw std::runtime_error("http-"+std::to_string(status));
    std::string body;
    for (;;) {
        Require(WinHttpQueryDataAvailable(request,nullptr)); operation.Wait(WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE,cancel,deadline);
        if (!operation.bytes) break;
        auto count = (std::min)(operation.bytes,65536ul); if (body.size()+count > 2*1024*1024) throw std::runtime_error("response-too-large");
        const auto offset = body.size(); body.resize(offset+count);
        Require(WinHttpReadData(request,body.data()+offset,count,nullptr)); operation.Wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,cancel,deadline);
        body.resize(offset+operation.bytes);
    }
    return body;
}
namespace {
void DownloadAsset(std::string url,const std::filesystem::path& output,std::uint64_t maximum,std::stop_token stop,wmf::update::Progress progress) {
    Handle session{WinHttpOpen(L"WinMouseFix/2.0.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC)};
    if (!session.value) throw std::runtime_error("network-error"); Require(WinHttpSetTimeouts(session,10000,10000,30000,30000));
    HANDLE cancel = CreateEventW(nullptr,TRUE,FALSE,nullptr); if (!cancel) throw std::runtime_error("network-error");
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } closer{cancel}; std::stop_callback cancelled(stop,[cancel] { SetEvent(cancel); });
    for (unsigned redirects = 0; redirects <= 5; ++redirects) {
        if (stop.stop_requested()) throw std::runtime_error("cancelled");
        const auto wide = winrt::to_hstring(url); URL_COMPONENTS parts{sizeof(parts)}; parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        Require(WinHttpCrackUrl(wide.c_str(),static_cast<DWORD>(wide.size()),0,&parts));
        const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
        if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != 443 || !(host == L"github.com" || host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com")) throw std::runtime_error("unsafe-download-url");
        Handle connection{WinHttpConnect(session,host.c_str(),parts.nPort,0)}; if (!connection.value) throw std::runtime_error("network-error");
        std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength); if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
        AsyncRequest operation; auto request = WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE); if (!request) throw std::runtime_error("network-error");
        if (!operation.event || !operation.closed || WinHttpSetStatusCallback(request,AsyncRequest::Callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0) == WINHTTP_INVALID_STATUS_CALLBACK) { WinHttpCloseHandle(request); throw std::runtime_error("network-error"); }
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&operation);
        if (!WinHttpSetOption(request,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))) { WinHttpCloseHandle(request); throw std::runtime_error("network-error"); } operation.request = request;
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; Require(WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)));
        Require(WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,context)); operation.Wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,cancel,GetTickCount64()+60000);
        Require(WinHttpReceiveResponse(request,nullptr)); operation.Wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,cancel,GetTickCount64()+60000);
        DWORD status{},size = sizeof(status); Require(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX));
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::array<wchar_t,8192> location{}; size = static_cast<DWORD>(location.size()*sizeof(wchar_t)); Require(WinHttpQueryHeaders(request,WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,location.data(),&size,WINHTTP_NO_HEADER_INDEX)); url = winrt::to_string(location.data()); continue;
        }
        if (status != 200) throw std::runtime_error("http-"+std::to_string(status));
        std::ofstream file(output,std::ios::binary|std::ios::trunc); if (!file) throw std::runtime_error("write-failed");
        std::array<char,65536> buffer; std::uint64_t received{}; const auto started = GetTickCount64();
        for (;;) {
            if (stop.stop_requested()) throw std::runtime_error("cancelled"); if (GetTickCount64()-started > 30*60*1000) throw std::runtime_error("network-timeout");
            Require(WinHttpReadData(request,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr)); operation.Wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,cancel,GetTickCount64()+60000); if (!operation.bytes) break;
            received += operation.bytes; if (received > maximum) throw std::runtime_error("response-too-large");
            file.write(buffer.data(),operation.bytes); if (!file) throw std::runtime_error("write-failed"); if (progress) progress(received,maximum);
        }
        file.close(); if (!file) throw std::runtime_error("write-failed"); return;
    }
    throw std::runtime_error("too-many-redirects");
}
}
void DownloadRelease(const ReleaseInfo& release,const std::filesystem::path& tx,std::stop_token stop,wmf::update::Progress progress) {
    auto hash = release.digest;
    if (hash.empty()) {
        if (release.checksumUrl.empty()) throw std::runtime_error("missing-checksum");
        DownloadAsset(release.checksumUrl,tx/L"checksum.txt",1024,stop,{}); hash = wmf::update::ReadText(tx/L"checksum.txt",1024).substr(0,64);
        if (hash.size() != 64 || hash.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) throw std::runtime_error("invalid-checksum"); std::transform(hash.begin(),hash.end(),hash.begin(),[](char c) { return static_cast<char>(std::tolower(c)); });
    }
    DownloadAsset(release.assetUrl,tx/L"package.part",release.assetBytes,stop,progress);
    if (std::filesystem::file_size(tx/L"package.part") != release.assetBytes || wmf::update::Sha256(tx/L"package.part",stop) != hash) throw std::runtime_error("package-integrity");
    std::filesystem::rename(tx/L"package.part",tx/L"package.zip");
}
UpdateService::UpdateService(bool simulation,std::filesystem::path file,UpdateFetch fetch,std::filesystem::path installation,UpdateDownload download) : fetch_(std::move(fetch)),reminderFile_(std::move(file)),installation_(std::move(installation)),download_(std::move(download)) {
    if (simulation && !fetch_) throw std::invalid_argument("Simulated updater requires a fake transport");
    if (!fetch_) fetch_ = FetchGithubReleases;
    if (simulation && !installation_.empty() && !download_) throw std::invalid_argument("Simulated downloads require a fake transport");
    if (!download_) download_ = DownloadRelease;
    try { if (std::filesystem::exists(reminderFile_) && std::filesystem::file_size(reminderFile_) <= 512) { std::ifstream in(reminderFile_); std::getline(in,lastNotified_); } } catch (...) {}
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
UpdateService::~UpdateService() { downloadWorker_.request_stop(); worker_.request_stop(); ready_.notify_all(); if (downloadWorker_.joinable()) downloadWorker_.join(); if (worker_.joinable()) worker_.join(); }
void UpdateService::Queue(std::uint64_t now,bool automatic) {
    if (checking_ || downloading_ || state_ == "ready" || state_ == "installing") return;
    checking_ = queued_ = true; automaticRequest_ = automatic; lastAttempt_ = now; ++checkId_; state_ = "checking"; error_.clear(); ready_.notify_all();
}
void UpdateService::Request(std::uint64_t now) { std::lock_guard lock(mutex_); Queue(now,false); }
void UpdateService::Tick(std::uint64_t now,bool automatic,unsigned hours) {
    std::lock_guard lock(mutex_); const bool enabled = automatic && (!configured_ || !automatic_); configured_ = true; automatic_ = automatic; hours_ = (std::clamp)(hours,1u,168u);
    if (!automatic) { pendingPrompt_ = false; promptTaken_ = false; }
    if (automatic && (enabled || (!checking_ && now >= lastAttempt_ && now-lastAttempt_ >= static_cast<std::uint64_t>(hours_)*3600000))) Queue(now,true);
}
void UpdateService::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        { std::unique_lock lock(mutex_); if (!ready_.wait(lock,stop,[&] { return queued_; })) return; queued_ = false; }
        ReleasePage result; std::string error;
        try {
            for (unsigned page = 1;; ++page) {
                if (stop.stop_requested()) return;
                const auto parsed = ParseReleasePage(fetch_(page,stop));
                if (parsed.found && (!result.found || parsed.best.version > result.best.version)) { result.found = true; result.best = parsed.best; }
                if (parsed.count < 100) break;
                if (page >= 10) throw std::runtime_error("too-many-releases");
            }
        } catch (const winrt::hresult_error&) { error = "invalid-response"; }
        catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "network-error"; }
        if (stop.stop_requested()) return;
        std::lock_guard lock(mutex_); completedId_ = checkId_; checking_ = false; checkedUtc_ = UtcNow(); error_ = std::move(error);
        release_ = std::move(result.best); ReleaseVersion current; ParseReleaseTitle(ProductVersion,current);
        state_ = !error_.empty() ? "error" : !result.found ? "no-releases" : release_.version > current ? "available" : "current";
        pendingPrompt_ = state_ == "available" && automaticRequest_ && automatic_ && lastNotified_ != release_.version.Text(); promptTaken_ = false;
    }
}
void UpdateService::Acknowledge(std::uint64_t id) {
    std::lock_guard lock(mutex_); if (checking_ || id != completedId_ || state_ != "available") return;
    pendingPrompt_ = false; lastNotified_ = release_.version.Text();
    try { if (!reminderFile_.empty()) { std::filesystem::create_directories(reminderFile_.parent_path()); std::ofstream out(reminderFile_,std::ios::trunc); out << lastNotified_; } } catch (...) {}
}
bool UpdateService::TakePrompt() { std::lock_guard lock(mutex_); if (!pendingPrompt_ || promptTaken_ || checking_) return false; promptTaken_ = true; return true; }
std::string UpdateService::StatusJson() const {
    EnsureJsonApartment(); std::lock_guard lock(mutex_); JsonObject object;
    auto text = [&](const wchar_t* key,std::string_view value) { object.SetNamedValue(key,JsonValue::CreateStringValue(winrt::to_hstring(value))); };
    text(L"state",state_); text(L"error",error_); text(L"checkedUtc",checkedUtc_); text(L"currentVersion",ProductVersion);
    text(L"version",release_.version.Text()); text(L"title",release_.title); text(L"notes",release_.notes); text(L"url",release_.url);
    object.SetNamedValue(L"checkId",JsonValue::CreateNumberValue(static_cast<double>(checkId_)));
    object.SetNamedValue(L"completedId",JsonValue::CreateNumberValue(static_cast<double>(completedId_)));
    object.SetNamedValue(L"notificationPending",JsonValue::CreateBooleanValue(pendingPrompt_ && !checking_));
    object.SetNamedValue(L"canInstall",JsonValue::CreateBooleanValue(!installation_.empty() && !release_.assetUrl.empty() && (!release_.digest.empty() || !release_.checksumUrl.empty())));
    object.SetNamedValue(L"receivedBytes",JsonValue::CreateNumberValue(static_cast<double>(received_)));
    object.SetNamedValue(L"totalBytes",JsonValue::CreateNumberValue(static_cast<double>(total_)));
    const auto elapsed = downloadStarted_ ? GetTickCount64()-downloadStarted_ : 0;
    object.SetNamedValue(L"bytesPerSecond",JsonValue::CreateNumberValue(elapsed ? static_cast<double>(received_)*1000/elapsed : 0));
    return winrt::to_string(object.Stringify());
}
bool UpdateService::Busy() const { std::lock_guard lock(mutex_); return downloading_ || state_ == "ready" || state_ == "installing"; }
bool UpdateService::Download(std::uint64_t id) {
    std::lock_guard gate(downloadGate_); ReleaseInfo release;
    { std::lock_guard lock(mutex_); if (id != completedId_ || (state_ != "available" && state_ != "download-error" && state_ != "cancelled") || installation_.empty() || release_.assetUrl.empty() || (release_.digest.empty() && release_.checksumUrl.empty())) return false; release = release_; downloading_ = true; state_ = "downloading"; error_.clear(); received_ = 0; total_ = release.assetBytes; downloadStarted_ = GetTickCount64(); pendingPrompt_ = false; }
    if (downloadWorker_.joinable()) downloadWorker_.join();
    downloadWorker_ = std::jthread([this,release](std::stop_token stop) {
        std::filesystem::path tx;
        try {
            wmf::update::CheckTree(installation_); if (!std::filesystem::is_regular_file(installation_/L"manifest.json")) throw std::runtime_error("missing-install-manifest");
            if (wmf::update::TransactionPending(installation_)) throw std::runtime_error("update-already-pending");
            if (std::filesystem::space(installation_).available < release.assetBytes+16*1024*1024) throw std::runtime_error("insufficient-space");
            GUID guid{}; if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("write-failed"); wchar_t id[40]{}; StringFromGUID2(guid,id,40);
            tx = installation_/(L".wmf-update-"+std::wstring(id)); std::filesystem::create_directory(tx);
            download_(release,tx,stop,[this](std::uint64_t received,std::uint64_t total) { std::lock_guard lock(mutex_); received_ = received; total_ = total; });
            { std::lock_guard lock(mutex_); state_ = "verifying"; }
            wmf::update::ExtractPackage(tx/L"package.zip",tx/L"payload",stop);
            wmf::update::VerifyPackage(tx/L"payload",release.version.Text(),stop);
            if (stop.stop_requested()) throw std::runtime_error("cancelled");
            std::lock_guard lock(mutex_); transaction_ = tx; state_ = "ready"; downloading_ = false;
        } catch (const std::exception& error) { std::error_code ignored; if (!tx.empty()) std::filesystem::remove_all(tx,ignored); std::lock_guard lock(mutex_); error_ = error.what(); state_ = stop.stop_requested() ? "cancelled" : "download-error"; downloading_ = false; }
        catch (...) { std::error_code ignored; if (!tx.empty()) std::filesystem::remove_all(tx,ignored); std::lock_guard lock(mutex_); error_ = "invalid-package"; state_ = "download-error"; downloading_ = false; }
    }); return true;
}
bool UpdateService::CancelDownload() { std::lock_guard gate(downloadGate_); std::lock_guard lock(mutex_); if (state_ == "installing") return false; if (downloading_) { downloadWorker_.request_stop(); return true; } if (state_ == "ready") { std::error_code ignored; std::filesystem::remove_all(transaction_,ignored); transaction_.clear(); state_ = "cancelled"; return true; } return false; }
bool UpdateService::RequestInstall() { std::lock_guard lock(mutex_); if (state_ != "ready") return false; state_ = "installing"; installRequested_ = true; return true; }
std::filesystem::path UpdateService::TakeInstall() { std::lock_guard lock(mutex_); if (!installRequested_) return {}; installRequested_ = false; return transaction_; }
void UpdateService::InstallFailed(std::string error) { std::lock_guard lock(mutex_); state_ = "download-error"; error_ = std::move(error); }
}
