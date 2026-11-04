#include "../WinMouseFix.Host/CapabilityEvidence.h"
#include "../WinMouseFix.Host/ResidentTray.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include <fstream>
#include <iostream>
namespace {
using namespace wmf;
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
void Evidence() {
    const auto production = ProductEnvironment::Production(); const auto env = ProductEnvironment::Simulation(production.executableDirectory.parent_path().parent_path().parent_path());
    Capabilities caps; caps.apiBits = RequiredApis|ActionApi; caps.osBuild = 26300; caps.osRevision = 1; caps.revisionKnown = 1;
    CapabilityEvidence evidence(env,caps); VERIFY(!evidence.Flags());
    const auto confirm = [&](bool zoom,unsigned raw,bool observed) { try { evidence.Confirm(zoom,10,raw,10,10,20,observed,"Isolated fake application observation"); return true; } catch (...) { return false; } };
    VERIFY(!confirm(false,0,true) && !confirm(false,10,false));
    VERIFY(confirm(true,10,true) && evidence.Flags() == NativeZoomValidated);
    VERIFY(confirm(false,10,true) && evidence.Flags() == (WheelSourceValidated|NativeZoomValidated));
    VERIFY(confirm(true,10,true) && evidence.Flags() == (WheelSourceValidated|NativeZoomValidated));
    const auto path = env.dataDirectory/L"native-validation.json"; std::ifstream in(path); const auto root = ParseJsonObject(std::string(std::istreambuf_iterator<char>(in),{})); in.close();
    VERIFY(root.HasKey(L"sourceObservation") && root.HasKey(L"pinchObservation"));
    VERIFY(root.HasKey(L"pinchScope"));
    using namespace winrt::Windows::Data::Json;
    const auto baseline=winrt::to_string(root.Stringify());
    const auto write=[&](const JsonObject& value) { std::ofstream out(path,std::ios::binary|std::ios::trunc); out << winrt::to_string(value.Stringify()); };
    const auto rebuilt=[&] {
        auto value=ParseJsonObject(baseline); const auto identity=winrt::to_string(value.GetNamedString(L"identity"));
        std::size_t end{}; for (unsigned i=0;i<6;++i) end=identity.find('/',end)+1;
        value.SetNamedValue(L"identity",JsonValue::CreateStringValue(winrt::to_hstring(identity.substr(0,end)+"1/2/3"))); return value;
    };
    auto compatible=rebuilt(); write(compatible);
    VERIFY(evidence.Flags()==NativeZoomValidated); // A rebuild never grants ordinary-wheel acceptance.
    compatible.Remove(L"pinchScope"); write(compatible);
    VERIFY(evidence.Flags()==NativeZoomValidated); // Original ABI-v4 user observations survive a compatible rebuild.
    { std::ifstream before(path,std::ios::binary); const std::string saved(std::istreambuf_iterator<char>(before),{}); before.close();
      VERIFY(evidence.Flags()==NativeZoomValidated);
      std::ifstream after(path,std::ios::binary); VERIFY(saved==std::string(std::istreambuf_iterator<char>(after),{})); }
    for (const auto invalidScope : {JsonValue::CreateStringValue(L"incompatible/pinch/99"),JsonValue::CreateNullValue()}) {
        auto value=rebuilt(); value.SetNamedValue(L"pinchScope",invalidScope); write(value); VERIFY(!evidence.Flags());
    }
    for (const auto invalidIdentity : {L"262143/0/0/26300/1/1/1/2/3",L"262144/0/0/26300/2/1/1/2/3",L"262144/invalid",L"262144/0/0/26300/1/1/1/2/x"}) {
        auto value=rebuilt(); value.Remove(L"pinchScope"); value.SetNamedValue(L"identity",JsonValue::CreateStringValue(invalidIdentity)); write(value); VERIFY(!evidence.Flags());
    }
    { auto value=rebuilt(); value.SetNamedValue(L"simulation",JsonValue::CreateBooleanValue(false)); write(value); VERIFY(!evidence.Flags()); }
    { auto value=rebuilt(); value.Remove(L"pinchObservation"); write(value); VERIFY(!evidence.Flags()); }
    { auto value=rebuilt(); value.SetNamedValue(L"flags",JsonValue::CreateNumberValue(0)); write(value); VERIFY(!evidence.Flags()); }
    for (unsigned invalid=0;invalid<5;++invalid) {
        auto value=rebuilt(); auto pinch=value.GetNamedObject(L"pinchObservation");
        if (invalid==0) pinch.SetNamedValue(L"visualConfirmed",JsonValue::CreateBooleanValue(false));
        if (invalid==1) pinch.SetNamedValue(L"visualOutcome",JsonValue::CreateStringValue(L"not-observed"));
        if (invalid==2) pinch.SetNamedValue(L"blocked",JsonValue::CreateNumberValue(9));
        if (invalid==3) pinch.SetNamedValue(L"accepted",JsonValue::CreateNumberValue(9));
        if (invalid==4) pinch.SetNamedValue(L"frames",JsonValue::CreateNumberValue(0));
        write(value); VERIFY(!evidence.Flags());
    }
    write(ParseJsonObject(baseline)); VERIFY(evidence.Flags()==(WheelSourceValidated|NativeZoomValidated));
    evidence.RecordFailure(true,10,10,10,12,"Excel: frames accepted but no visible pinch zoom");
    VERIFY(evidence.Flags() == WheelSourceValidated);
    { std::ifstream failed(path); const auto data = ParseJsonObject(std::string(std::istreambuf_iterator<char>(failed),{}));
      VERIFY(!data.GetNamedObject(L"pinchObservation").GetNamedBoolean(L"visualConfirmed"));
      VERIFY(data.GetNamedObject(L"sourceObservation").GetNamedBoolean(L"visualConfirmed")); }
    VERIFY(confirm(true,10,true) && evidence.Flags() == (WheelSourceValidated|NativeZoomValidated));
    ++caps.osRevision; CapabilityEvidence patched(env,caps); VERIFY(!patched.Flags());
    std::ofstream(path) << "corrupt evidence"; VERIFY(!evidence.Flags());
    auto config = DefaultBindingConfig();
    const std::string observation = R"({"action":"task-view","trigger":"pass","continuous":"limited","hold":"pass","reverse":"pass","release":"pass","notes":"Fake observation only; never a real Shell result"})";
    evidence.RecordShell(config,observation); auto observed = ParseJsonObject(evidence.ShellObservations(config));
    VERIFY(observed.GetNamedObject(L"task-view").GetNamedString(L"continuous") == L"limited");
    CapabilityEvidence reopened(env,Capabilities{caps}); VERIFY(reopened.ShellObservations(config) == "{}"); // OS patch differs.
    config.shellFingers = 3; VERIFY(evidence.ShellObservations(config) == "{}");
    bool rejected{}; try { evidence.RecordShell(config,"{\"action\":\"launchpad\"}"); } catch (...) { rejected = true; } VERIFY(rejected);
}
struct Shell {
    int fail{-1},adds{},modifies{},deletes{},loads{},destroys{},notices{}; DWORD noticeFlags{}; std::wstring noticeTitle,noticeText;
    TrayApi Api() { return {this,
        [](void* p,DWORD command,NOTIFYICONDATAW* data)->BOOL {
            auto& s = *static_cast<Shell*>(p); if (command == NIM_ADD) ++s.adds; if (command == NIM_MODIFY) ++s.modifies; if (command == NIM_DELETE) ++s.deletes;
            if (command == NIM_MODIFY && (data->uFlags & NIF_INFO)) { ++s.notices; s.noticeFlags = data->dwInfoFlags; s.noticeTitle = data->szInfoTitle; s.noticeText = data->szInfo; }
            return static_cast<int>(command) != s.fail;
        },[](void* p,HINSTANCE,int)->HICON { ++static_cast<Shell*>(p)->loads; return reinterpret_cast<HICON>(1); },
        [](void* p,HICON) { ++static_cast<Shell*>(p)->destroys; },true}; }
};
void Tray() {
    const auto environment = ProductEnvironment::Simulation(ProductEnvironment::Production().executableDirectory);
    for (const int failed : {-1,static_cast<int>(NIM_ADD),static_cast<int>(NIM_SETVERSION)}) {
        Shell shell; shell.fail = failed;
        { ResidentTray tray(environment,nullptr,nullptr,shell.Api()); const auto first = tray.Update(L"paused"); VERIFY(static_cast<bool>(first) == (failed == -1));
          if (first) { VERIFY(tray.Update(L"enabled")); VERIFY(tray.Update(L"enabled",true)); VERIFY(shell.adds == 2 && shell.modifies == 1); }
        }
        VERIFY(shell.loads == shell.destroys);
        VERIFY(shell.deletes == (failed == static_cast<int>(NIM_ADD) ? 0 : 1));
    }
    bool rejected{}; try { ResidentTray illegal(environment,nullptr,nullptr,RealTrayApi()); } catch (...) { rejected = true; } VERIFY(rejected);
    for (bool enabled : {false,true}) {
        Shell shell; ResidentTray tray(environment,nullptr,nullptr,shell.Api()); VERIFY(tray.Update(L"paused"));
        VERIFY(tray.NotifyStartup(enabled)); VERIFY(tray.NotifyStartup(enabled)); VERIFY(tray.Update(L"paused",true)); VERIFY(tray.NotifyStartup(enabled));
        VERIFY(shell.notices == (enabled ? 1 : 0));
        if (enabled) { VERIFY(shell.noticeFlags & NIIF_NOSOUND); VERIFY(shell.noticeTitle == L"Win Mouse Fix"); VERIFY(shell.noticeText.find(L"后台运行") != std::wstring::npos); }
    }
    Shell failedNotice; ResidentTray tray(environment,nullptr,nullptr,failedNotice.Api()); VERIFY(tray.Update(L"paused"));
    failedNotice.fail = NIM_MODIFY; VERIFY(!tray.NotifyStartup(true)); VERIFY(tray.NotifyStartup(true)); VERIFY(failedNotice.notices == 1);
}
}
int RunProductDeliveryTests(int& total) { Evidence(); Tray(); total += checks; return failures; }
