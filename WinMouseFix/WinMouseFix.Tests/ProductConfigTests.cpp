#include "../WinMouseFix.Host/ProductConfig.h"
#include "../WinMouseFix.Host/ConfigStore.h"
#include "../WinMouseFix.Host/ProductEnvironment.h"
#include "../WinMouseFix.Host/InputOwner.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace wmf;
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
std::string Replace(std::string text,std::string_view from,std::string_view to) {
    const auto at = text.find(from); if (at == std::string::npos) throw std::runtime_error("Test JSON replacement missing");
    text.replace(at,from.size(),to); return text;
}
void Config() {
    auto config = DefaultProductConfig(false); VERIFY(!config.enabled && !config.startup && config.startupNotification && config.input.bindingCount == 4);
    const auto json = SerializeConfig(config); ProductConfig parsed;
    auto error = ParseConfig(json,parsed);
    if (!error) std::cerr << "JSON parse: " << error.field << ' ' << error.message << ' ' << error.result.nativeError << '\n';
    VERIFY(error); VERIFY(SerializeConfig(parsed) == json);
    auto legacy = ParseJsonObject(json); legacy.Remove(L"startupNotification"); legacy.Remove(L"automaticUpdates"); legacy.Remove(L"updateIntervalHours");
    VERIFY(ParseConfig(winrt::to_string(legacy.Stringify()),parsed) && parsed.startupNotification);
    VERIFY(!parsed.automaticUpdates && parsed.updateIntervalHours == 6);
    auto updateConfig = config; updateConfig.automaticUpdates = true; updateConfig.updateIntervalHours = 24;
    VERIFY(ParseConfig(SerializeConfig(updateConfig),parsed) && parsed.automaticUpdates && parsed.updateIntervalHours == 24);
    for (const auto value : {"0","169","1.5","\"6\""}) VERIFY(!ParseConfig(Replace(json,"\"updateIntervalHours\":6",std::string("\"updateIntervalHours\":")+value),parsed));
    auto silent = config; silent.startupNotification = false;
    VERIFY(ParseConfig(SerializeConfig(silent),parsed) && !parsed.startupNotification);
    auto invalidNotice = ParseJsonObject(json); invalidNotice.SetNamedValue(L"startupNotification",winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"false"));
    VERIFY(!ParseConfig(winrt::to_string(invalidNotice.Stringify()),parsed));
    config.enabled = config.startup = true; config.input.revision = 9007199254740991ull; config.input.mmPerCountX = -0.25;
    config.input.shellFingers = 3; config.input.wheelMode = WheelMode::High;
    auto& shortcut = config.input.bindings[config.input.bindingCount++]; shortcut = {5,Button::Middle,Trigger::Click,ControlModifier,Action::Shortcut};
    shortcut.keyCount = 2; shortcut.keys[0] = 0x11; shortcut.keys[1] = 'T';
    const auto custom = SerializeConfig(config); VERIFY(ParseConfig(custom,parsed));
    VERIFY(SerializeConfig(parsed) == custom && parsed.input.bindings[4].keys[1] == 'T' && parsed.input.mmPerCountX == -0.25);
    for (const auto invalid : {std::string{},std::string("[]"),std::string("null"),std::string("{\"schemaVersion\":1}"),
        Replace(json,"\"back\"","\"unknown-action\""),Replace(json,"\"thresholdCounts\":8","\"thresholdCounts\":0"),
        Replace(json,"\"fingers\":4","\"fingers\":5"),Replace(json,"\"revision\":1","\"revision\":1.5"),
        Replace(json,"\"enabled\":false","\"enabled\":\"false\""),Replace(json,"\"mode\":\"off\"","\"mode\":\"infinite\""),
        std::string("{\"schemaVersion\":1,\"bad\":\"")+static_cast<char>(0xff)+"\"}",std::string(65537,'x')}) {
        const auto old = SerializeConfig(parsed); VERIFY(!ParseConfig(invalid,parsed)); VERIFY(SerializeConfig(parsed) == old);
    }
    const auto newer = Replace(json,"\"schemaVersion\":1","\"schemaVersion\":2");
    error = ParseConfig(newer,parsed); VERIFY(error.problem == ConfigProblem::NewerSchema && error.result.code == Status::VersionMismatch);
    config = DefaultProductConfig(false); config.input.bindings[1].trigger = Trigger::Click; config.input.bindings[1].action = Action::Mute;
    error = ValidateConfig(config); VERIFY(!error && error.bindingId == 2 && error.conflictingId == 1);
    config = DefaultProductConfig(false); config.input.mmPerCountY = NAN; VERIFY(!ValidateConfig(config));
}
std::filesystem::path Repository() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,static_cast<DWORD>(std::size(path)));
    return std::filesystem::path(path).parent_path().parent_path().parent_path().parent_path();
}
std::string ReadText(const std::filesystem::path& file) {
    std::ifstream input(file,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}};
}
void Goldens() {
    const auto repo = Repository(); const auto output = repo/L"artifacts/product-validation/config-goldens";
    std::filesystem::create_directories(output);
    for (const auto name : {L"default",L"custom"}) {
        ProductConfig c; VERIFY(ParseConfig(ReadText(repo/L"WinMouseFix/Shared/Configuration"/(std::wstring(name)+L".json")),c));
        const auto normalized = SerializeConfig(c);
        std::ofstream(output/(L"cpp-"+std::wstring(name)+L".json"),std::ios::binary) << normalized;
        const auto managed = output/(L"csharp-"+std::wstring(name)+L".json");
        if (std::filesystem::exists(managed)) { ProductConfig other; VERIFY(ParseConfig(ReadText(managed),other)); VERIFY(SerializeConfig(other) == normalized); }
        VERIFY(c.input.bindingCount == 4);
    }
}
struct MemoryFiles : ConfigFiles {
    std::string active,backup,preserved;
    int failStage{},writes{}; bool crashAfterSave{};
    ConfigRead Read(bool b) override { auto text = b ? backup : active; return {{},!text.empty(),text}; }
    Result Save(std::string_view bytes,std::optional<std::string_view> old,bool preserve) override {
        ++writes;
        if (failStage) return {Status::NativeFailure,static_cast<unsigned>(100+failStage)};
        if (preserve) preserved = active;
        if (old) backup = *old; active = bytes;
        if (crashAfterSave) throw std::runtime_error("Simulated process loss before ACK");
        return {};
    }
};
struct Publication : PreparedPublication {
    unsigned& count;
    explicit Publication(unsigned& c) : count(c) {}
    void Publish() noexcept override { ++count; }
};
void Store() {
    auto memory = std::make_unique<MemoryFiles>(); const auto disk = memory.get(); unsigned published{};
    ConfigStore store(std::move(memory),[&](const ProductConfig&) { return std::make_unique<Publication>(published); });
    VERIFY(store.Load()); VERIFY(store.Current()->initialFileMissing && disk->active.empty());
    auto c = store.Current()->config; c.enabled = true; VERIFY(store.Commit(1,c));
    VERIFY(store.Current()->config.input.revision == 2 && published == 1 && !disk->active.empty());
    const auto previous = disk->active;
    for (int failure = 1; failure <= 4; ++failure) {
        disk->failStage = failure; const auto result = store.Commit(2,c);
        VERIFY(!result && result.problem == ConfigProblem::Storage && disk->active == previous);
        VERIFY(store.Current()->config.input.revision == 2 && published == 1);
    }
    disk->failStage = 0; VERIFY(store.Commit(2,c)); VERIFY(disk->backup == previous && published == 2);
    VERIFY(store.Commit(2,c).problem == ConfigProblem::Conflict && published == 2);
    c.input.bindings[1].trigger = Trigger::Click; c.input.bindings[1].action = Action::Mute;
    VERIFY(store.Commit(3,c).problem == ConfigProblem::Validation && published == 2);
    c = store.Current()->config;
    std::atomic<unsigned> succeeded{};
    std::thread a([&] { if (store.Commit(3,c)) ++succeeded; });
    std::thread b([&] { if (store.Commit(3,c)) ++succeeded; }); a.join(); b.join();
    VERIFY(succeeded == 1 && store.Current()->config.input.revision == 4);
    disk->active = "{corrupted"; VERIFY(!store.Load());
    VERIFY(store.Current()->recoveryRequired && store.Current()->fromBackup && disk->active == "{corrupted");
    const auto revision = store.Current()->config.input.revision;
    VERIFY(store.Commit(revision,c).problem == ConfigProblem::RecoveryRequired);
    VERIFY(store.Commit(revision,c,true)); VERIFY(disk->preserved == "{corrupted" && !store.Current()->recoveryRequired);
    disk->active = "{\"schemaVersion\":99}"; VERIFY(!store.Load());
    VERIFY(store.Current()->warning.problem == ConfigProblem::NewerSchema && disk->active.find("99") != std::string::npos);
    auto crashFiles = std::make_unique<MemoryFiles>(); const auto crash = crashFiles.get();
    ConfigStore restart(std::move(crashFiles)); VERIFY(restart.Load()); crash->crashAfterSave = true;
    VERIFY(!restart.Commit(1,DefaultProductConfig(false))); crash->crashAfterSave = false;
    VERIFY(restart.Load() && restart.Current()->config.input.revision == 2);
}
void NativeFiles() {
    const auto directory = Repository()/L"artifacts/product-validation/config-store"/
        (std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    auto files = std::make_unique<NativeConfigFiles>(directory); auto* disk = files.get(); ConfigStore store(std::move(files));
    VERIFY(store.Load()); auto c = store.Current()->config; VERIFY(store.Commit(1,c));
    const auto first = disk->Read(false); VERIFY(first.result && first.exists);
    c.enabled = true; VERIFY(store.Commit(2,c)); VERIFY(disk->Read(true).bytes == first.bytes);
    VERIFY(store.Load() && store.Current()->config.enabled);
    { std::ofstream output(directory/L"config.json",std::ios::binary); output << "{\"schemaVersion\":98}"; }
    VERIFY(!store.Load()); VERIFY(disk->Read(false).bytes.find("98") != std::string::npos);
    VERIFY(store.Commit(store.Current()->config.input.revision,DefaultProductConfig(false),true));
    bool preserved{};
    for (const auto& item : std::filesystem::directory_iterator(directory))
        if (item.path().filename().wstring().starts_with(L"config.preserved.")) preserved = ReadText(item.path()).find("98") != std::string::npos;
    VERIFY(preserved); VERIFY(!store.Current()->config.enabled);
}
void Environments() {
    const auto production = ProductEnvironment::Production();
    const auto a = ProductEnvironment::Simulation(Repository()),b = ProductEnvironment::Simulation(Repository());
    VERIFY(a.simulation && !production.simulation && !production.sid.empty());
    VERIFY(a.pipe != production.pipe && a.pipe != b.pipe && a.hostMutex != production.hostMutex && a.settingsMutex != production.settingsMutex);
    VERIFY(a.dataDirectory != production.dataDirectory && a.inputOwnerSuffix != production.inputOwnerSuffix);
    VERIFY(a.dataDirectory.wstring().find(L"artifacts") != std::wstring::npos);
    InputOwner one(a.inputOwnerSuffix),same(a.inputOwnerSuffix),other(b.inputOwnerSuffix);
    VERIFY(one.Acquire()); VERIFY(same.Acquire().code == Status::Busy); VERIFY(other.Acquire());
    UserSecurity security(production.sid); BOOL present{},defaulted{}; PACL acl{};
    VERIFY(GetSecurityDescriptorDacl(security.Attributes()->lpSecurityDescriptor,&present,&acl,&defaulted) && present && acl && acl->AceCount == 2);
}
}
int RunProductConfigTests(int& total) { Config(); Goldens(); Store(); NativeFiles(); Environments(); total += checks; return failures; }
