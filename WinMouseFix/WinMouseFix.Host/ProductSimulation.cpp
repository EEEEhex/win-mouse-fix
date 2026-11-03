#include "ProductSimulation.h"

namespace wmf::host {
ProductSimulation* ProductSimulation::current{};
void ProductSimulation::Emit() {
    ++session; sequence = 0; active = revision;
    InputEvent event; event.button = Button::Back; event.device = 7; event.target = 9; event.revision = revision; event.generation = epoch; event.session = session;
    const auto send = [&](InputKind kind,std::int64_t y = 0) {
        LARGE_INTEGER q{}; QueryPerformanceCounter(&q); event.firstQpc = event.lastQpc = static_cast<std::uint64_t>(q.QuadPart);
        event.sequence = ++sequence; event.kind = kind; event.dy = y; event.flags = recording ? RecordedInput : 0; capture.sink(capture.context,&event);
    };
    send(InputKind::Down); for (int i = 0; i < 40; ++i) send(InputKind::Move,-1); send(InputKind::Up);
}
ProductRuntimeApi ProductSimulation::RuntimeApi() {
    current = this; ProductRuntimeApi api; api.simulation = true;
    api.engine.capabilities = [](Capabilities* caps) noexcept -> Result { caps->apiBits = RequiredApis|ActionApi; return {}; };
    api.engine.createPlatform = [](PlatformHandle* handle) noexcept -> Result { ++current->platforms; *handle = reinterpret_cast<PlatformHandle>(current); return {}; };
    api.engine.openPlatform = [](PlatformHandle,const DeviceConfig*) noexcept -> Result { return {}; };
    api.engine.submit = [](PlatformHandle,const ContactFrame* frame,MappedFrame*) noexcept -> Result { ++current->frames; if (frame->count > 2) ++current->shellFrames; return {}; };
    api.engine.stopInertia = [](PlatformHandle,std::uint32_t) noexcept -> Result { return {}; };
    api.engine.destroyPlatform = [](PlatformHandle* handle) noexcept -> Result { *handle = nullptr; return {}; };
    api.engine.execute = [](const ActionRequest*) noexcept -> Result { ++current->actions; return {}; };
    api.engine.targetValid = [](void*,const InputEvent&,TargetPolicy,bool) noexcept { return true; };
    api.createCapture = [](const ProductCaptureConfig* config,CaptureHandle* handle) noexcept -> Result { current->capture = *config; current->revision = config->bindings.revision; ++current->captures; *handle = reinterpret_cast<CaptureHandle>(current); return {}; };
    api.post = [](CaptureHandle handle,CaptureCommand command,std::uint64_t value) noexcept -> Result {
        auto& state = *reinterpret_cast<ProductSimulation*>(handle);
        if (command == CaptureCommand::Enable) { state.enabled = true; state.epoch = value; if (state.scripted) { state.scripted = false; state.Emit(); } }
        if (command == CaptureCommand::Disable) state.enabled = false;
        if (command == CaptureCommand::CompleteDrag || command == CaptureCommand::Cancel) ++state.completions;
        return {};
    };
    api.stats = [](CaptureHandle,CaptureStats*) noexcept -> Result { return {}; };
    api.destroyCapture = [](CaptureHandle* handle) noexcept -> Result { ++current->destroyed; *handle = nullptr; return {}; };
    api.update = [](CaptureHandle,const BindingConfig* config) noexcept -> Result { current->capture.bindings = *config; current->revision = config->revision; return {}; };
    api.policy = [](CaptureHandle,std::uint64_t,TargetPolicy) noexcept -> Result { return {}; };
    api.revision = [](CaptureHandle,CaptureRevision* revision) noexcept -> Result { revision->applied = current->revision; revision->active = 0; return {}; };
    api.recording = [](CaptureHandle,std::uint32_t value) noexcept -> Result { current->recording = value != 0; return {}; };
    return api;
}
StartupApi ProductSimulation::Startup() {
    return {this,
        [](void* p,std::wstring& value,bool& present) -> Result { auto& s = *static_cast<ProductSimulation*>(p); value = s.runValue; present = s.present; return {}; },
        [](void* p,const std::wstring& value) -> Result { auto& s = *static_cast<ProductSimulation*>(p); s.runValue = value; s.present = true; return {}; },
        [](void* p) -> Result { auto& s = *static_cast<ProductSimulation*>(p); s.present = false; s.runValue.clear(); return {}; },true};
}
}
