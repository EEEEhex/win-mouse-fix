#pragma once
#include "ProductRuntime.h"
#include "StartupRegistration.h"

namespace wmf::host {
// Backend is reachable only from an explicitly isolated smoke mode. There is
// no hook installation, native contact submission, SendInput or HKCU write.
struct ProductSimulation {
    std::atomic<unsigned> platforms{},captures{},frames{},actions{},completions{},destroyed{},shellFrames{};
    std::uint64_t revision{1},active{},epoch{1},session{},sequence{};
    ProductCaptureConfig capture;
    bool scripted{},enabled{},recording{},present{};
    std::wstring runValue;
    ProductRuntimeApi RuntimeApi();
    StartupApi Startup();
    void Emit();
    static ProductSimulation* current;
};
}
