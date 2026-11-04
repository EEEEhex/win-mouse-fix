#include "../Shared/Update/UpdatePackage.h"
#include "../WinMouseFix.Host/UpdateService.h"
#include "../WinMouseFix.Host/ProductVersion.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include "../ThirdParty/miniz/miniz.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#undef GetObject
namespace {
namespace fs=std::filesystem;
using namespace wmf::update;
using namespace wmf::host;
using namespace winrt::Windows::Data::Json;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
template<class F> bool Rejects(F f) { try { f(); return false; } catch (...) { return true; } }
struct Fixture {
    fs::path root=fs::temp_directory_path()/L"WinMouseFix updater tests"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Fixture() { fs::create_directories(root); } ~Fixture() { std::error_code error; fs::remove_all(root,error); }
};
void MakePackage(const fs::path& root,std::string version,bool old=false) {
    fs::create_directories(root); JsonArray files;
    for (const auto name : {L"WinMouseFix.Host.exe",L"WinMouseFix.Core.dll",L"WinMouseFix.Platform.Win32.dll",L"WinMouseFix.Updater.exe",L"Settings/WinMouseFix.Settings.exe",L"Settings/WinMouseFix.Settings.dll",L"licenses/notice.txt"}) {
        const auto path=fs::path(name); WriteText(root/path,version+winrt::to_string(name)); JsonObject entry; entry.SetNamedValue(L"path",JsonValue::CreateStringValue(name)); entry.SetNamedValue(L"bytes",JsonValue::CreateNumberValue(static_cast<double>(fs::file_size(root/path)))); entry.SetNamedValue(L"sha256",JsonValue::CreateStringValue(winrt::to_hstring(Sha256(root/path)))); files.Append(entry);
    }
    if (old) { WriteText(root/L"obsolete.dll","old"); JsonObject file; file.SetNamedValue(L"path",JsonValue::CreateStringValue(L"obsolete.dll")); file.SetNamedValue(L"bytes",JsonValue::CreateNumberValue(3)); file.SetNamedValue(L"sha256",JsonValue::CreateStringValue(winrt::to_hstring(Sha256(root/L"obsolete.dll")))); files.Append(file); }
    JsonObject object; object.SetNamedValue(L"schemaVersion",JsonValue::CreateNumberValue(1)); object.SetNamedValue(L"architecture",JsonValue::CreateStringValue(L"x64")); object.SetNamedValue(L"version",JsonValue::CreateStringValue(winrt::to_hstring(version))); object.SetNamedValue(L"files",files); WriteText(root/L"manifest.json",winrt::to_string(object.Stringify()));
}
void RefreshManifest(const fs::path& root) {
    auto object=JsonObject::Parse(winrt::to_hstring(ReadText(root/L"manifest.json"))); auto files=object.GetNamedArray(L"files");
    for (const auto& value : files) { auto entry=value.GetObject(); auto path=root/fs::path(entry.GetNamedString(L"path").c_str()); entry.SetNamedValue(L"bytes",JsonValue::CreateNumberValue(static_cast<double>(fs::file_size(path)))); entry.SetNamedValue(L"sha256",JsonValue::CreateStringValue(winrt::to_hstring(Sha256(path)))); } WriteText(root/L"manifest.json",winrt::to_string(object.Stringify()));
}
void Put(std::string& data,std::uint32_t value,unsigned bytes) { for(unsigned i=0;i<bytes;++i) data+=static_cast<char>(value>>(i*8)); }
void Zip(const fs::path& path,const std::vector<std::pair<std::string,std::string>>& files,std::uint32_t attributes=0) {
    std::string data,central; unsigned count{};
    for(const auto& [name,body] : files) {
        auto offset=static_cast<std::uint32_t>(data.size()),crc=static_cast<std::uint32_t>(mz_crc32(0,reinterpret_cast<const unsigned char*>(body.data()),body.size()));
        Put(data,0x04034b50,4); Put(data,20,2); Put(data,0,2); Put(data,0,2); Put(data,0,4); Put(data,crc,4); Put(data,static_cast<std::uint32_t>(body.size()),4); Put(data,static_cast<std::uint32_t>(body.size()),4); Put(data,static_cast<std::uint32_t>(name.size()),2); Put(data,0,2); data+=name; data+=body;
        Put(central,0x02014b50,4); Put(central,0x0314,2); Put(central,20,2); Put(central,0,2); Put(central,0,2); Put(central,0,4); Put(central,crc,4); Put(central,static_cast<std::uint32_t>(body.size()),4); Put(central,static_cast<std::uint32_t>(body.size()),4); Put(central,static_cast<std::uint32_t>(name.size()),2); Put(central,0,2); Put(central,0,2); Put(central,0,2); Put(central,0,2); Put(central,attributes,4); Put(central,offset,4); central+=name; ++count;
    }
    auto offset=static_cast<std::uint32_t>(data.size()); data+=central; Put(data,0x06054b50,4); Put(data,0,4); Put(data,count,2); Put(data,count,2); Put(data,static_cast<std::uint32_t>(central.size()),4); Put(data,offset,4); Put(data,0,2); WriteText(path,data);
}
void Validation() {
    Fixture f; VERIFY(SafeRelative("Settings/test.dll")==fs::path(L"Settings/test.dll"));
    for(auto name : {"../escape","/absolute","C:\\evil","\\\\server\\share","file:stream","Data/config.json","data.backup/test","WinMouseFix.storage.json",".wmf-update-x/file","CON.txt","aux","COM1.dll","name. ","a/../b"}) VERIFY(Rejects([&] { SafeRelative(name); }));
    const auto package=f.root/L"package"; MakePackage(package,"2.1.0"); VERIFY(VerifyPackage(package,"2.1.0").files.size()==7);
    VERIFY(Rejects([&] { VerifyPackage(package,"2.2.0"); }));
    WriteText(package/L"extra.dll","extra"); VERIFY(Rejects([&] { VerifyPackage(package,"2.1.0"); })); fs::remove(package/L"extra.dll");
    WriteText(package/L"WinMouseFix.Core.dll","tampered"); VERIFY(Rejects([&] { VerifyPackage(package,"2.1.0"); }));
    std::stop_source stop; stop.request_stop(); VERIFY(Rejects([&] { Sha256(package/L"manifest.json",stop.get_token()); }));
    Zip(f.root/L"good.zip",{{"nested/file.txt","hello"}}); ExtractPackage(f.root/L"good.zip",f.root/L"expanded",{}); VERIFY(ReadText(f.root/L"expanded/nested/file.txt")=="hello");
    for(const auto name : {"../outside.txt","Data/config.json","nested/../../escape","nul.txt","C:/escape","/absolute"}) { Zip(f.root/L"bad.zip",{{name,"bad"}}); VERIFY(Rejects([&] { ExtractPackage(f.root/L"bad.zip",f.root/L"bad-expanded",{}); })); VERIFY(!fs::exists(f.root/L"bad-expanded")); }
    Zip(f.root/L"duplicate.zip",{{"same.dll","a"},{"SAME.dll","b"}}); VERIFY(Rejects([&] { ExtractPackage(f.root/L"duplicate.zip",f.root/L"duplicate",{}); }));
    Zip(f.root/L"link.zip",{{"link","outside"}},0120777u<<16); VERIFY(Rejects([&] { ExtractPackage(f.root/L"link.zip",f.root/L"link",{}); }));
}
void Transactions() {
    Fixture f;
    for(bool portable : {false,true}) {
        const auto root=f.root/(portable?L"portable":L"appdata"),data=portable?root/L"Data":f.root/L"separate user data",tx=root/L".wmf-update-case";
        MakePackage(root,"2.0.0",true); MakePackage(tx/L"payload","2.1.0"); WriteText(data/L"config.json","configuration unchanged"); WriteText(data/L"Settings/ui.json","preferences"); WriteText(root/L"WinMouseFix.storage.json",portable?"program":"appdata"); WriteText(root/L"user-file.txt","keep");
        PrepareTransaction(tx,data); VERIFY(TransactionPending(root)); VERIFY(fs::equivalent(PendingTransaction(root),tx));
        ApplyTransaction(tx); VERIFY(ReadManifest(root).version=="2.1.0"); VERIFY(!fs::exists(root/L"obsolete.dll")); VERIFY(ReadText(data/L"config.json")=="configuration unchanged"); VERIFY(ReadText(root/L"user-file.txt")=="keep"); VERIFY(ReadText(root/L"WinMouseFix.storage.json")== (portable?"program":"appdata"));
        WriteText(data/L"config.json","new format"); RollbackTransaction(tx); VERIFY(!TransactionPending(root)); VERIFY(ReadManifest(root).version=="2.0.0"); VERIFY(ReadText(root/L"obsolete.dll")=="old"); VERIFY(ReadText(data/L"config.json")=="configuration unchanged"); VERIFY(ReadText(data/L"Settings/ui.json")=="preferences");
        fs::remove_all(tx); MakePackage(tx/L"payload","2.1.0"); PrepareTransaction(tx,data);
        const auto locked=CreateFileW((root/L"WinMouseFix.Core.dll").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr); VERIFY(locked!=INVALID_HANDLE_VALUE);
        VERIFY(Rejects([&] { ApplyTransaction(tx); }));
        // Simulate restart recovery after a partially applied update; repeat rollback is harmless.
        RollbackTransaction(PendingTransaction(root)); CloseHandle(locked); RollbackTransaction(tx); VERIFY(ReadManifest(root).version=="2.0.0"); VERIFY(ReadText(data/L"config.json")=="configuration unchanged");
        fs::remove_all(tx); MakePackage(tx/L"payload","2.1.0"); PrepareTransaction(tx,data); ApplyTransaction(tx); CommitTransaction(tx); VERIFY(!TransactionPending(root)); VERIFY(ReadManifest(root).version=="2.1.0"); CleanupTransactions(root); VERIFY(!fs::exists(tx)); VERIFY(ReadText(data/L"config.json")=="configuration unchanged");
    }
}
void Delivery() {
    Fixture f; ReleaseVersion next; ParseReleaseTitle(ProductVersion,next); ++next.parts[2]; const auto version=next.Text();
    const auto installation=f.root/L"install",source=f.root/L"source"; MakePackage(installation,"2.0.0"); MakePackage(source,version);
    std::vector<std::pair<std::string,std::string>> files; for(const auto& item : fs::recursive_directory_iterator(source)) if(item.is_regular_file()) files.emplace_back(winrt::to_string(fs::relative(item.path(),source).generic_wstring()),ReadText(item.path())); Zip(f.root/L"package.zip",files);
    const auto filename="WinMouseFix-v"+version+"-win-x64.zip";
    JsonObject asset; asset.SetNamedValue(L"name",JsonValue::CreateStringValue(winrt::to_hstring(filename)));
    asset.SetNamedValue(L"browser_download_url",JsonValue::CreateStringValue(winrt::to_hstring("https://github.com/EEEEhex/win-mouse-fix/releases/download/v"+version+"/"+filename)));
    asset.SetNamedValue(L"size",JsonValue::CreateNumberValue(100)); asset.SetNamedValue(L"digest",JsonValue::CreateStringValue(L"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    JsonArray assets; assets.Append(asset); JsonObject item; item.SetNamedValue(L"name",JsonValue::CreateStringValue(winrt::to_hstring("v"+version)));
    item.SetNamedValue(L"html_url",JsonValue::CreateStringValue(winrt::to_hstring("https://github.com/EEEEhex/win-mouse-fix/releases/tag/v"+version))); item.SetNamedValue(L"assets",assets);
    JsonArray releases; releases.Append(item); const auto release=winrt::to_string(releases.Stringify());
    std::atomic<unsigned> attempts{}; std::atomic<bool> cancelCase{};
    UpdateService updates(true,f.root/L"reminder.txt",[&](unsigned,std::stop_token) { return std::string(release); },installation,[&](const ReleaseInfo&,const fs::path& tx,std::stop_token stop,Progress progress) {
        ++attempts; if(cancelCase) { while(!stop.stop_requested()) Sleep(1); throw std::runtime_error("cancelled"); }
        if(attempts==1) throw std::runtime_error("network-error"); fs::copy_file(f.root/L"package.zip",tx/L"package.zip"); progress(100,100);
    });
    auto state=[&] { return ParseJsonObject(updates.StatusJson()); };
    auto wait=[&](std::wstring_view desired) { auto deadline=GetTickCount64()+5000; while(GetTickCount64()<deadline) { if(state().GetNamedString(L"state")==desired) return true; Sleep(1); } return false; };
    updates.Request(1); VERIFY(wait(L"available")); VERIFY(state().GetNamedBoolean(L"canInstall")); VERIFY(!updates.Download(999)); VERIFY(updates.Download(1)); VERIFY(wait(L"download-error")); VERIFY(!updates.Busy());
    cancelCase=true; VERIFY(updates.Download(1)); VERIFY(!updates.Download(1)); VERIFY(updates.CancelDownload()); VERIFY(wait(L"cancelled"));
    cancelCase=false; VERIFY(updates.Download(1)); VERIFY(wait(L"ready")); VERIFY(updates.Busy()); VERIFY(state().GetNamedNumber(L"receivedBytes")==100); updates.Request(2); VERIFY(state().GetNamedNumber(L"checkId")==1);
    VERIFY(updates.RequestInstall()); auto tx=updates.TakeInstall(); VERIFY(!tx.empty() && fs::exists(tx/L"payload/manifest.json")); VERIFY(updates.TakeInstall().empty()); VERIFY(!updates.CancelDownload()); updates.InstallFailed("fake-start-failed"); VERIFY(!updates.Busy());
}
void HelperProcesses() {
    Fixture f; wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768); const auto updater=fs::path(executable).parent_path()/L"WinMouseFix.Updater.exe";
    VERIFY(fs::is_regular_file(updater)); if (!fs::is_regular_file(updater)) return;
    for (const auto scenario : {L"success",L"failure",L"recovery"}) {
        const auto root=f.root/scenario,tx=root/L".wmf-update-process",data=root/L"Data"; MakePackage(root,"2.0.0",true); MakePackage(tx/L"payload","2.1.0");
        fs::copy_file(executable,root/L"WinMouseFix.Host.exe",fs::copy_options::overwrite_existing); RefreshManifest(root);
        fs::copy_file(executable,tx/L"payload/WinMouseFix.Host.exe",fs::copy_options::overwrite_existing); RefreshManifest(tx/L"payload");
        WriteText(root/L".wmf-test-updater",winrt::to_string(scenario)); WriteText(data/L"config.json","user configuration"); WriteText(tx/L"data-directory.txt",winrt::to_string(data.wstring())); fs::copy_file(updater,tx/L"worker.exe");
        const bool recovery=std::wstring_view(scenario)==L"recovery";
        if (recovery) { PrepareTransaction(tx,data); ApplyTransaction(tx); }
        auto command=L"\""+(tx/L"worker.exe").wstring()+L"\" "+(recovery?L"--recover":L"--transaction")+L" \""+tx.wstring()+L"\" --test-report";
        STARTUPINFOW info{sizeof(info)}; PROCESS_INFORMATION process{}; const bool launched=CreateProcessW((tx/L"worker.exe").c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&info,&process)!=0; VERIFY(launched); if (!launched) continue;
        VERIFY(WaitForSingleObject(process.hProcess,20000)==WAIT_OBJECT_0); DWORD code{}; GetExitCodeProcess(process.hProcess,&code); if(code==STILL_ACTIVE) { TerminateProcess(process.hProcess,1); WaitForSingleObject(process.hProcess,5000); } CloseHandle(process.hThread); CloseHandle(process.hProcess);
        const bool success=std::wstring_view(scenario)==L"success"; VERIFY(code==(success||recovery?0u:1u)); VERIFY(ReadManifest(root).version==(success?"2.1.0":"2.0.0")); VERIFY(!TransactionPending(root)); VERIFY(ReadText(data/L"config.json")=="user configuration");
        if (success) VERIFY(fs::exists(tx/L"commit.ok")); else { const auto deadline=GetTickCount64()+3000; while(!fs::exists(root/L"old-restarted") && GetTickCount64()<deadline) Sleep(10); VERIFY(fs::exists(root/L"old-restarted")); }
        // Trial fixtures exit after commit/abort; let their process handles close before fixture cleanup.
        Sleep(200);
    }
}
void AcceptedEvidence() {
    Fixture f; const auto root=f.root/L"installation",tx=root/L".wmf-update-evidence",data=root/L"Data"; MakePackage(root,"2.0.0"); MakePackage(tx/L"payload","2.1.0");
    auto fingerprint=[](const fs::path& path) { std::uint64_t hash=14695981039346656037ull; for (unsigned char byte : ReadText(path)) { hash^=byte; hash*=1099511628211ull; } return std::to_string(hash); };
    const auto identity="262144/10/0/26300/9711/1/"+fingerprint(root/L"WinMouseFix.Host.exe")+"/"+fingerprint(root/L"WinMouseFix.Core.dll")+"/"+fingerprint(root/L"WinMouseFix.Platform.Win32.dll");
    JsonObject record; record.SetNamedValue(L"schemaVersion",JsonValue::CreateNumberValue(1)); record.SetNamedValue(L"simulation",JsonValue::CreateBooleanValue(false)); record.SetNamedValue(L"identity",JsonValue::CreateStringValue(winrt::to_hstring(identity))); record.SetNamedValue(L"flags",JsonValue::CreateNumberValue(2));
    const auto original=winrt::to_string(record.Stringify()); WriteText(data/L"native-validation.json",original); WriteText(data/L"config.backup.json","previous configuration");
    for (const auto name : {L"WinMouseFix.Core.dll",L"WinMouseFix.Platform.Win32.dll"}) fs::copy_file(root/name,tx/L"payload"/name,fs::copy_options::overwrite_existing); RefreshManifest(tx/L"payload");
    PrepareTransaction(tx,data); ApplyTransaction(tx); const auto carried=ParseJsonObject(ReadText(data/L"native-validation.json"));
    VERIFY(carried.GetNamedNumber(L"flags")==2 && carried.GetNamedBoolean(L"updateCarryForward")); VERIFY(carried.GetNamedString(L"identity")!=winrt::to_hstring(identity));
    WriteText(data/L"config.backup.json","migrated backup"); RollbackTransaction(tx); VERIFY(ReadText(data/L"native-validation.json")==original); VERIFY(ReadText(data/L"config.backup.json")=="previous configuration");
    fs::remove_all(tx); MakePackage(tx/L"payload","2.1.0"); PrepareTransaction(tx,data); ApplyTransaction(tx); VERIFY(ReadText(data/L"native-validation.json")==original); RollbackTransaction(tx);
}
}
int RunUpdatePackageTests(int& total) { Validation(); Transactions(); Delivery(); HelperProcesses(); AcceptedEvidence(); total+=checks; return failures; }
bool TryRunUpdateTestChild(int argc,char** argv,int& result) {
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768); const auto root=fs::path(executable).parent_path();
    if (fs::path(executable).filename()!=L"WinMouseFix.Host.exe" || !fs::exists(root/L".wmf-test-updater")) return false;
    if (argc==2 && std::string_view(argv[1])=="--settings") { WriteText(root/L"old-restarted","ok"); result=0; return true; }
    if (argc==3 && std::string_view(argv[1])=="--update-verify") {
        const fs::path tx{winrt::to_hstring(argv[2]).c_str()};
        if (ReadText(root/L".wmf-test-updater")=="failure") { result=1; return true; }
        WriteText(tx/L"health.ready",ReadManifest(root).version);
        const auto end=GetTickCount64()+10000; while(!fs::exists(tx/L"commit.ok") && !fs::exists(tx/L"abort") && GetTickCount64()<end) Sleep(10); result=0; return true;
    }
    result=2; return true;
}
