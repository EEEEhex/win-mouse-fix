#include "../WinMouseFix.Host/UpdateService.h"
#include "../WinMouseFix.Host/ProductVersion.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <Windows.h>
namespace {
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
constexpr auto Releases = R"([{"name":"v2.9.0","tag_name":"v99.0.0","html_url":"https://github.com/EEEEhex/win-mouse-fix/releases/tag/old","body":"old"},{"name":"v2.10.0 更新","html_url":"https://github.com/EEEEhex/win-mouse-fix/releases/tag/new","body":"# 更新日志\n- 更流畅"},{"name":"v9.0.0","prerelease":true},{"name":"v8.0.0","draft":true},{"name":"invalid","tag_name":"v100.0.0"}])";
auto State(UpdateService& updates) { return ParseJsonObject(updates.StatusJson()); }
bool Finished(UpdateService& updates,unsigned id) {
    const auto end = GetTickCount64()+3000;
    while (GetTickCount64() < end) { if (State(updates).GetNamedNumber(L"completedId") >= id) return true; Sleep(1); }
    return false;
}
void Versions() {
    ReleaseVersion a,b; VERIFY(ParseReleaseTitle("v2.10.0",a)); VERIFY(ParseReleaseTitle("v2.9.0",b)); VERIFY(a > b);
    VERIFY(ParseReleaseTitle("Win Mouse Fix v2.0.1 更新",b)); VERIFY(b.Text() == "2.0.1");
    for (auto invalid : {"v2.0","v2.0.0.1","v2.0.0-beta.1","v02.0.0","2026-11-03","v4294967296.0.0","xv2.0.0"}) VERIFY(!ParseReleaseTitle(invalid,b));
    VERIFY(ParseReleaseTitle("v2.0.0+build.1",b));
    const auto parsed = ParseReleasePage(Releases); VERIFY(parsed.count == 5 && parsed.found && parsed.best.version.Text() == "2.10.0");
    VERIFY(parsed.best.notes.find("更流畅") != std::string::npos);
    VERIFY(!ParseReleasePage("[]").found);
    VERIFY(!ParseReleasePage(R"([{"name":"","tag_name":"v9.0.0"}])").found);
    for (const auto text : {"{}",R"([{"name":"v3.0.0","html_url":"https://evil.example/releases/tag/a"}])",R"([{"name":"v3.0.0","html_url":"https://github.com/EEEEhex/win-mouse-fix/releases/tag/a","body":42}])"}) {
        bool rejected{}; try { ParseReleasePage(text); } catch (...) { rejected = true; } VERIFY(rejected);
    }
}
void Scheduling() {
    std::atomic<unsigned> calls{};
    const auto file = std::filesystem::temp_directory_path()/L"WinMouseFix.update-tests"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".txt");
    {
        UpdateService updates(true,file,[&](unsigned page,std::stop_token) { ++calls; if (page != 1) throw std::runtime_error("Unexpected page"); return std::string(Releases); });
        updates.Tick(1000,false,6); VERIFY(calls == 0);
        updates.Tick(1000,true,6); VERIFY(Finished(updates,1)); VERIFY(calls == 1 && State(updates).GetNamedString(L"state") == L"available");
        VERIFY(!updates.TakePrompt(false)); VERIFY(State(updates).GetNamedBoolean(L"notificationPending"));
        VERIFY(!updates.TakePrompt(false)); VERIFY(updates.TakePrompt()); VERIFY(!updates.TakePrompt()); updates.Acknowledge(999); VERIFY(State(updates).GetNamedBoolean(L"notificationPending"));
        updates.Acknowledge(1); VERIFY(!State(updates).GetNamedBoolean(L"notificationPending"));
        updates.Tick(1000+6*3600000-1,true,6); VERIFY(calls == 1);
        updates.Tick(1000+6*3600000,true,6); VERIFY(Finished(updates,2)); VERIFY(calls == 2 && !updates.TakePrompt());
        updates.Tick(1000+7*3600000,true,1); VERIFY(Finished(updates,3)); VERIFY(calls == 3);
        updates.Tick(1000+20*3600000,false,1); VERIFY(calls == 3);
        updates.Request(1000+20*3600000); VERIFY(Finished(updates,4)); VERIFY(calls == 4 && !updates.TakePrompt());
    }
    {
        UpdateService reopened(true,file,[](unsigned,std::stop_token) { return std::string(Releases); }); reopened.Tick(1,true,6); VERIFY(Finished(reopened,1)); VERIFY(!reopened.TakePrompt());
    }
    {
        UpdateService newer(true,file,[](unsigned,std::stop_token) { return std::string(R"([{"name":"v3.0.0","html_url":"https://github.com/EEEEhex/win-mouse-fix/releases/tag/v3","body":null}])"); });
        newer.Tick(1,true,6); VERIFY(Finished(newer,1)); VERIFY(newer.TakePrompt());
    }
    std::error_code ignored; std::filesystem::remove(file,ignored);
    {
        UpdateService empty(true,{},[](unsigned,std::stop_token) { return std::string("[]"); }); empty.Request(1); VERIFY(Finished(empty,1)); VERIFY(State(empty).GetNamedString(L"state") == L"no-releases");
    }
    {
        UpdateService current(true,{},[](unsigned,std::stop_token) { return std::string("[{\"name\":\"v")+std::string(ProductVersion)+R"(","html_url":"https://github.com/EEEEhex/win-mouse-fix/releases/tag/current","body":"same"}])"; }); current.Request(1); VERIFY(Finished(current,1)); VERIFY(State(current).GetNamedString(L"state") == L"current");
    }
    {
        UpdateService failed(true,{},[](unsigned,std::stop_token)->std::string { throw std::runtime_error("rate-limit"); }); failed.Tick(1,true,6); VERIFY(Finished(failed,1)); VERIFY(State(failed).GetNamedString(L"state") == L"error" && !failed.TakePrompt());
        failed.Tick(3600001,true,6); VERIFY(State(failed).GetNamedNumber(L"checkId") == 1); failed.Request(3600002); VERIFY(Finished(failed,2));
    }
    const auto started = GetTickCount64();
    {
        std::string full = "["; for (unsigned i = 0; i < 100; ++i) { if (i) full += ','; full += R"({"name":"invalid"})"; } full += ']';
        std::atomic<unsigned> pages{};
        UpdateService paged(true,{},[&](unsigned page,std::stop_token) { ++pages; return page == 1 ? full : std::string(Releases); });
        paged.Tick(2000,true,6); VERIFY(Finished(paged,1)); VERIFY(pages == 2 && State(paged).GetNamedString(L"version") == L"2.10.0");
        paged.Tick(1,true,6); VERIFY(State(paged).GetNamedNumber(L"checkId") == 1);
        UpdateService excessive(true,{},[&](unsigned,std::stop_token) { return full; }); excessive.Request(1); VERIFY(Finished(excessive,1)); VERIFY(State(excessive).GetNamedString(L"state") == L"error");
    }
    {
        UpdateService cancellable(true,{},[](unsigned,std::stop_token stop) { std::mutex mutex; std::condition_variable_any ready; std::unique_lock lock(mutex); ready.wait(lock,stop,[] { return false; }); return std::string("[]"); });
        cancellable.Request(1); cancellable.Request(2); VERIFY(State(cancellable).GetNamedNumber(L"checkId") == 1);
    }
    VERIFY(GetTickCount64()-started < 1000);
    bool isolated{}; try { UpdateService illegal(true,{}); } catch (...) { isolated = true; } VERIFY(isolated);
}
}
int RunUpdateTests(int& total) { Versions(); Scheduling(); total += checks; return failures; }
