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
UpdateService::UpdateService(bool simulation,std::filesystem::path file,UpdateFetch fetch) : fetch_(std::move(fetch)),reminderFile_(std::move(file)) {
    if (simulation && !fetch_) throw std::invalid_argument("Simulated updater requires a fake transport");
    if (!fetch_) fetch_ = FetchGithubReleases;
    try { if (std::filesystem::exists(reminderFile_) && std::filesystem::file_size(reminderFile_) <= 512) { std::ifstream in(reminderFile_); std::getline(in,lastNotified_); } } catch (...) {}
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
UpdateService::~UpdateService() { worker_.request_stop(); ready_.notify_all(); if (worker_.joinable()) worker_.join(); }
void UpdateService::Queue(std::uint64_t now,bool automatic) {
    if (checking_) return;
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
    return winrt::to_string(object.Stringify());
}
}
