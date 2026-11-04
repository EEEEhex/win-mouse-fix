#include "../WinMouseFix.Host/DataLocation.h"
#include "../WinMouseFix.Host/ConfigStore.h"
#include <Windows.h>
#include <fstream>
#include <iostream>

namespace {
using namespace wmf;
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
std::string Read(const std::filesystem::path& path) { std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
void Write(const std::filesystem::path& path,std::string_view value) { std::filesystem::create_directories(path.parent_path()); std::ofstream(path,std::ios::binary) << value; }
DataPaths Paths(std::wstring_view name) {
    static unsigned id{};
    const auto root = std::filesystem::temp_directory_path()/L"WinMouseFix.storage-tests"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++id))/name;
    std::filesystem::create_directories(root/L"Program");
    return {root/L"AppData",root/L"Program/Data",root/L"Program/WinMouseFix.storage.json"};
}
void RoundTrip() {
    const auto paths = Paths(L"往返 中文 路径"); VERIFY(ReadDataLocation(paths) == DataLocation::AppData);
    auto config = DefaultProductConfig(false); config.enabled = true; config.automaticUpdates = true; config.updateIntervalHours = 12;
    const auto bytes = SerializeConfig(config);
    Write(paths.appData/L"config.json",bytes); Write(paths.appData/L"config.backup.json",bytes);
    Write(paths.appData/L"Settings/ui.json",R"({"Language":"en","Width":900})");
    Write(paths.appData/L"native-validation.json",R"({"flags":2,"simulation":true})");
    Write(paths.appData/L"logs/runtime.jsonl","fake log"); Write(paths.appData/L"validation/run/trace.bin",std::string("\0\1\2",3));
    Write(paths.appData/L"update-reminder.txt","9.0.0"); Write(paths.programData/L"config.json","old target");
    auto result = MigrateData(paths,DataLocation::AppData,DataLocation::Program);
    VERIFY(result.result && !result.sourceRetained); VERIFY(ReadDataLocation(paths) == DataLocation::Program);
    VERIFY(Read(paths.programData/L"config.json") == bytes && Read(result.sourceBackup/L"config.json") == bytes);
    VERIFY(Read(result.targetBackup/L"config.json") == "old target"); VERIFY(!std::filesystem::exists(paths.appData));
    VERIFY(Read(paths.programData/L"Settings/ui.json").find("en") != std::string::npos);
    VERIFY(Read(paths.programData/L"validation/run/trace.bin") == std::string("\0\1\2",3));
    VERIFY(Read(paths.programData/L"update-reminder.txt") == "9.0.0");
    config.updateIntervalHours = 24; Write(paths.programData/L"config.json",SerializeConfig(config));
    result = MigrateData(paths,DataLocation::Program,DataLocation::AppData);
    VERIFY(result.result && ReadDataLocation(paths) == DataLocation::AppData);
    VERIFY(Read(paths.appData/L"config.json") == SerializeConfig(config)); VERIFY(!std::filesystem::exists(paths.programData));
    ConfigStore store(std::make_unique<NativeConfigFiles>(paths.appData)); VERIFY(store.Load()); VERIFY(store.Current()->config.enabled && store.Current()->config.updateIntervalHours == 24);
    VERIFY(Read(paths.appData/L"logs/runtime.jsonl") == "fake log");
}
void Failures() {
    for (const auto checkpoint : {std::string("copied"),std::string("before-marker")}) {
        const auto paths = Paths(L"rollback"); Write(paths.appData/L"config.json","source"); Write(paths.programData/L"config.json","target");
        const auto result = MigrateData(paths,DataLocation::AppData,DataLocation::Program,[&](std::string_view stage) { if (stage == checkpoint) throw std::runtime_error("Injected migration interruption"); });
        VERIFY(!result.result); VERIFY(ReadDataLocation(paths) == DataLocation::AppData);
        VERIFY(Read(paths.appData/L"config.json") == "source" && Read(paths.programData/L"config.json") == "target");
        VERIFY(result.result.cleanupError == 0);
        const auto retry = MigrateData(paths,DataLocation::AppData,DataLocation::Program); VERIFY(retry.result && ReadDataLocation(paths) == DataLocation::Program);
    }
    {
        const auto paths = Paths(L"locked-marker"); Write(paths.appData/L"config.json","source"); Write(paths.programData/L"config.json","target");
        Write(paths.marker,R"({"schemaVersion":1,"location":"appdata"})");
        HANDLE lock = CreateFileW(paths.marker.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); VERIFY(lock != INVALID_HANDLE_VALUE);
        auto result = MigrateData(paths,DataLocation::AppData,DataLocation::Program); CloseHandle(lock);
        VERIFY(!result.result && ReadDataLocation(paths) == DataLocation::AppData); VERIFY(Read(paths.appData/L"config.json") == "source" && Read(paths.programData/L"config.json") == "target");
    }
    {
        const auto paths = Paths(L"locked-source"); Write(paths.appData/L"config.json","source");
        HANDLE lock = CreateFileW((paths.appData/L"config.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        const auto result = MigrateData(paths,DataLocation::AppData,DataLocation::Program); CloseHandle(lock);
        VERIFY(result.result && result.sourceRetained && ReadDataLocation(paths) == DataLocation::Program);
        VERIFY(Read(paths.programData/L"config.json") == "source" && Read(paths.appData/L"config.json") == "source");
    }
    {
        const auto paths = Paths(L"overlap"); auto overlap = paths; overlap.programData = paths.appData/L"Data";
        Write(paths.appData/L"config.json","source"); const auto result = MigrateData(overlap,DataLocation::AppData,DataLocation::Program);
        VERIFY(!result.result && Read(paths.appData/L"config.json") == "source" && !std::filesystem::exists(paths.marker));
    }
    {
        const auto paths = Paths(L"read-error"); Write(paths.appData/L"config.json","source");
        HANDLE lock = CreateFileW((paths.appData/L"config.json").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        const auto result = MigrateData(paths,DataLocation::AppData,DataLocation::Program); CloseHandle(lock);
        VERIFY(!result.result && ReadDataLocation(paths) == DataLocation::AppData && Read(paths.appData/L"config.json") == "source");
    }
    {
        const auto paths = Paths(L"invalid-marker");
        for (const auto bytes : {"{}",R"({"schemaVersion":2,"location":"program"})",R"({"schemaVersion":1,"location":"other"})",R"({"schemaVersion":1,"location":true})"}) {
            Write(paths.marker,bytes); bool rejected{}; try { ReadDataLocation(paths); } catch (...) { rejected = true; } VERIFY(rejected);
        }
    }
    {
        const auto paths = Paths(L"initial-defaults"); ConfigStore store(std::make_unique<NativeConfigFiles>(paths.appData)); VERIFY(store.Load()); const auto revision = store.Current()->config.input.revision;
        VERIFY(store.EnsureSaved()); VERIFY(std::filesystem::exists(paths.appData/L"config.json") && store.Current()->config.input.revision == revision);
        const auto result = MigrateData(paths,DataLocation::AppData,DataLocation::Program); VERIFY(result.result);
        ConfigStore restarted(std::make_unique<NativeConfigFiles>(paths.programData)); VERIFY(restarted.Load()); VERIFY(SerializeConfig(store.Current()->config) == SerializeConfig(restarted.Current()->config));
    }
    {
        const auto paths = Paths(L"readonly-source"); Write(paths.appData/L"config.json",SerializeConfig(DefaultProductConfig(false)));
        VERIFY(SetFileAttributesW((paths.appData/L"config.json").c_str(),FILE_ATTRIBUTE_READONLY));
        const auto result = MigrateData(paths,DataLocation::AppData,DataLocation::Program); VERIFY(result.result);
        ConfigStore target(std::make_unique<NativeConfigFiles>(paths.programData)); VERIFY(target.Load()); auto candidate = target.Current()->config; candidate.updateIntervalHours = 18;
        VERIFY(target.Commit(candidate.input.revision,candidate)); VERIFY(target.Current()->config.updateIntervalHours == 18);
    }
}
}
int RunDataLocationTests(int& total) { RoundTrip(); Failures(); total += checks; return failures; }
