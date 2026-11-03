#include "../WinMouseFix.Host/TimingMetrics.h"
#include "../WinMouseFix.Host/ProductLog.h"
#include "../WinMouseFix.Host/ProductSimulation.h"
#include "../WinMouseFix.Host/JsonSupport.h"
#include "../WinMouseFix.Host/DiagnosticText.h"
#include "../WinMouseFix.Host/Report.h"
#include <fstream>
#include <iostream>
namespace {
using namespace wmf;
using namespace wmf::host;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
void Timing() {
    TimingMetrics m(1000); ProductTrace t; t.kind = ProductTraceKind::Frame; t.frame.phase = Phase::Update;
    t.input.revision = 7; t.action = Action::ContentDrag;
    t.input.firstQpc = 100; t.input.lastQpc = 110; t.processStarted = 120; t.submitStarted = 125;
    for (unsigned i = 1; i <= 100; ++i) { t.returned = 125+i; m.Observe(t); }
    t.boundary = 1; m.Observe(t); t.boundary = 0;
    t.action = Action::NativeZoom; t.input.button = Button::Back; t.input.kind = InputKind::Wheel; m.Observe(t);
    t.stationary = 1; for (int i = 0; i < 60; ++i) m.Observe(t); t.stationary = 0;
    t.action = Action::None; t.input.button = Button::None; m.Observe(t);
    t.returned = 1; m.Observe(t);
    auto report = ParseJsonObject(m.Json()); VERIFY(report.GetNamedNumber(L"invalidSamples") == 1);
    const auto groups = report.GetNamedArray(L"groups"); VERIFY(groups.Size() == 3);
    const auto direct = groups.GetObjectAt(0);
    VERIFY(direct.GetNamedString(L"group") == L"content-drag/7" && direct.GetNamedNumber(L"seen") == 100);
    VERIFY(direct.GetNamedNumber(L"boundaryFrames") == 1);
    VERIFY(direct.GetNamedNumber(L"earliestP50Ms") == 75 && direct.GetNamedNumber(L"earliestP95Ms") == 120);
    VERIFY(direct.GetNamedNumber(L"latestP95Ms") == 110 && direct.GetNamedNumber(L"queueP95Ms") == 20 && direct.GetNamedNumber(L"processingP95Ms") == 100);
    VERIFY(groups.GetObjectAt(1).GetNamedString(L"group") == L"pinch/7" && groups.GetObjectAt(2).GetNamedString(L"group") == L"wheel-plan/7");
    VERIFY(groups.GetObjectAt(1).GetNamedNumber(L"seen") == 1);
    t.returned = 140; for (unsigned i = 0; i < 5000; ++i) m.Observe(t);
    report = ParseJsonObject(m.Json()); VERIFY(report.GetNamedArray(L"groups").GetObjectAt(2).GetNamedNumber(L"retained") == 4096);
    for (unsigned i = 8; i < 80; ++i) { t.input.revision = i; m.Observe(t); }
    report = ParseJsonObject(m.Json()); VERIFY(report.GetNamedArray(L"groups").Size() == 64 && report.GetNamedNumber(L"evictedGroups") == 11);
}
void LogFiles() {
    const auto exe = ProductEnvironment::Production().executableDirectory;
    const auto root = exe.parent_path().parent_path().parent_path()/L"artifacts/product-validation/log-smoke";
    auto env = ProductEnvironment::Simulation(root); ProductSimulation simulation;
    auto config = DefaultBindingConfig(); LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); config.qpcFrequency = frequency.QuadPart;
    ProductRuntime runtime(env,config,false,simulation.RuntimeApi());
    const auto logs = env.dataDirectory/L"logs";
    {
        ProductLog log(logs,runtime,config.qpcFrequency,true);
        const std::string message(100000,'m');
        for (unsigned round = 0; round < 6; ++round) {
            for (unsigned i = 0; i < 1000; ++i) log.Record("bounded-test",message,1);
            Sleep(280);
        }
        log.Record("fault","First native failure and cleanup error are separate",1); Sleep(300);
        const auto summary = ParseJsonObject(log.Summary()); VERIFY(summary.GetNamedNumber(L"lostControlRecords") > 0 && log.Error() == 0);
        const auto zip = log.Export("{\"simulation\":true}","{\"schemaVersion\":1}"); VERIFY(std::filesystem::exists(zip));
        std::filesystem::create_directories(root);
        std::ofstream(root/L"export.json",std::ios::binary) << "{\"simulation\":true,\"zip\":" << Json(Utf8(zip.wstring())) << '}';
    }
    std::uintmax_t total{}; unsigned count{};
    for (const auto& file : std::filesystem::directory_iterator(logs)) { ++count; const auto bytes = file.file_size(); total += bytes; VERIFY(bytes <= 1024*1024); }
    VERIFY(count <= 4 && total <= 4*1024*1024 && simulation.captures == 0 && simulation.frames == 0);
    runtime.Stop();
}
}
int RunProductLogTests(int& total) { Timing(); LogFiles(); total += checks; return failures; }
