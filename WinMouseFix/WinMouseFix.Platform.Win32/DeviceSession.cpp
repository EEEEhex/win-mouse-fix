#include "DeviceSession.h"
#include <algorithm>
#include <cmath>
#include <new>

namespace wmf::platform {
namespace {
struct SystemFunctions {
    decltype(&CreateSyntheticPointerDevice2) create{};
    decltype(&InjectSyntheticPointerInput) inject{};
    decltype(&DestroySyntheticPointerDevice) destroy{};
    decltype(&InjectTouchpadAction) action{};
};
#pragma warning(push)
#pragma warning(disable: 4191) // Win32 explicitly exposes dynamically resolved function addresses.
SystemFunctions& Functions() noexcept {
    static SystemFunctions functions = [] {
        SystemFunctions f;
        const auto module = GetModuleHandleW(L"user32.dll");
        if (!module) return f;
        f.create = reinterpret_cast<decltype(f.create)>(GetProcAddress(module, "CreateSyntheticPointerDevice2"));
        f.inject = reinterpret_cast<decltype(f.inject)>(GetProcAddress(module, "InjectSyntheticPointerInput"));
        f.destroy = reinterpret_cast<decltype(f.destroy)>(GetProcAddress(module, "DestroySyntheticPointerDevice"));
        f.action = reinterpret_cast<decltype(f.action)>(GetProcAddress(module, "InjectTouchpadAction"));
        return f;
    }();
    return functions;
}
#pragma warning(pop)
HSYNTHETICPOINTERDEVICE Create(void*, SYNTHETIC_DEVICE_CREATION_PARAMS* p) { return Functions().create(p); }
BOOL Inject(void*, HSYNTHETICPOINTERDEVICE d, const POINTER_TYPE_INFO* p, UINT32 n) { return Functions().inject(d, p, n); }
void Destroy(void*, HSYNTHETICPOINTERDEVICE d) { Functions().destroy(d); }
BOOL Action(void*, HSYNTHETICPOINTERDEVICE d, TOUCHPAD_ACTION a) { return Functions().action(d, a); }
DWORD Error(void*) { return GetLastError(); }
}

NativeApi LoadNativeApi() noexcept {
    auto& f = Functions();
    return {nullptr, f.create ? Create : nullptr, f.inject ? Inject : nullptr,
        f.destroy ? Destroy : nullptr, f.action ? Action : nullptr, Error};
}
std::uint32_t ApiMask(const NativeApi& api) noexcept {
    return (api.create ? CreateApi : 0u) | (api.inject ? InjectApi : 0u) |
        (api.destroy ? DestroyApi : 0u) | (api.action ? ActionApi : 0u);
}

Result MapContacts(const DeviceConfig& cfg, const GestureFrame& frame, MappedFrame& out) noexcept {
    out = {};
    if (frame.size != sizeof(GestureFrame) || cfg.size != sizeof(DeviceConfig) ||
        frame.fingers < 2 || frame.fingers > 4 || frame.phase > Phase::Cancel ||
        !std::isfinite(frame.offsetXmm) || !std::isfinite(frame.offsetYmm) ||
        !std::isfinite(frame.logicalSeconds) || frame.logicalSeconds < 0) return {Status::InvalidArgument};
    if (frame.version != AbiVersion || cfg.version != AbiVersion) return {Status::VersionMismatch};
    if (cfg.widthHimetric < 4000 || cfg.heightHimetric < 2000 ||
        cfg.widthHimetric > 100000 || cfg.heightHimetric > 100000) return {Status::OutOfRange};
    for (std::uint32_t i = 0; i < frame.fingers; ++i) {
        const double x = cfg.widthHimetric / 2.0 + (i - (frame.fingers - 1) / 2.0) * 1000.0 + frame.offsetXmm * 100.0;
        const double y = cfg.heightHimetric / 2.0 + frame.offsetYmm * 100.0;
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 ||
            x >= cfg.widthHimetric || y >= cfg.heightHimetric) return {Status::OutOfRange};
        const auto rx = std::llround(x), ry = std::llround(y);
        if (rx >= cfg.widthHimetric || ry >= cfg.heightHimetric) return {Status::OutOfRange};
        out.contacts[i] = {i + 1, static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry)};
    }
    out.count = frame.fingers;
    return {};
}

Result MapContacts(const DeviceConfig& cfg,const ContactFrame& frame,MappedFrame& out) noexcept {
    out = {};
    if (cfg.size != sizeof(cfg) || frame.size != sizeof(frame) || frame.count < 2 || frame.count > 4 ||
        frame.phase > Phase::Cancel || !std::isfinite(frame.logicalSeconds) || frame.logicalSeconds < 0) return {Status::InvalidArgument};
    if (cfg.version != AbiVersion || frame.version != AbiVersion) return {Status::VersionMismatch};
    if (cfg.widthHimetric < 4000 || cfg.heightHimetric < 2000 || cfg.widthHimetric > 100000 || cfg.heightHimetric > 100000) return {Status::OutOfRange};
    for (std::uint32_t i = 0; i < frame.count; ++i) {
        const auto& p = frame.contacts[i];
        if (!p.id || p.reserved || !std::isfinite(p.xMm) || !std::isfinite(p.yMm)) return {Status::InvalidArgument};
        for (std::uint32_t j = 0; j < i; ++j) if (frame.contacts[j].id == p.id) return {Status::InvalidArgument};
        const double x = cfg.widthHimetric/2.0+p.xMm*100.0, y = cfg.heightHimetric/2.0+p.yMm*100.0;
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= cfg.widthHimetric || y >= cfg.heightHimetric) return {Status::OutOfRange};
        const auto rx = std::llround(x),ry = std::llround(y);
        if (rx >= cfg.widthHimetric || ry >= cfg.heightHimetric) return {Status::OutOfRange};
        out.contacts[i] = {p.id,static_cast<std::int32_t>(rx),static_cast<std::int32_t>(ry)};
    }
    out.count = frame.count; return {};
}

Result DeviceSession::Open(const DeviceConfig& cfg) noexcept {
    if (!OnOwner()) return {Status::WrongThread};
    if (device_ || faulted_) return {Status::InvalidState};
    if (cfg.size != sizeof(DeviceConfig)) return {Status::InvalidArgument};
    if (cfg.version != AbiVersion) return {Status::VersionMismatch};
    if (cfg.widthHimetric < 4000 || cfg.heightHimetric < 2000 ||
        cfg.widthHimetric > 100000 || cfg.heightHimetric > 100000) return {Status::OutOfRange};
    if ((ApiMask(api_) & RequiredApis) != RequiredApis) return {Status::Unsupported, ERROR_PROC_NOT_FOUND};
    const auto clockReady = clock_.Initialize();
    if (!clockReady) return clockReady;
    SYNTHETIC_DEVICE_CREATION_PARAMS p{};
    p.pointerType = PT_TOUCHPAD;
    p.maxCount = 5;
    p.feedbackMode = POINTER_FEEDBACK_NONE;
    p.deviceWidth = cfg.widthHimetric;
    p.deviceHeight = cfg.heightHimetric;
    p.options = SDCO_PHYSICAL_SIZE | SDCO_TOUCHPAD_GESTURE_ONLY;
    device_ = api_.create(api_.context, &p);
    if (!device_) return {Status::NativeFailure, api_.error ? api_.error(api_.context) : ERROR_GEN_FAILURE};
    config_ = cfg;
    return {};
}

Result DeviceSession::Release(bool cancel) noexcept {
    if (!active_) return {};
    POINTER_TYPE_INFO pointers[5]{};
    std::uint64_t timestamp{};
    const auto stamp = clock_.Stamp(timestamp);
    if (!stamp) return stamp;
    for (std::uint32_t i = 0; i < active_; ++i) {
        pointers[i].type = PT_TOUCHPAD;
        auto& p = pointers[i].touchInfo.pointerInfo;
        p.pointerType = PT_TOUCHPAD;
        p.pointerId = last_.contacts[i].id;
        p.pointerFlags = POINTER_FLAG_CONFIDENCE | (cancel ? POINTER_FLAG_CANCELED : 0u);
        p.ptHimetricLocation = {last_.contacts[i].x, last_.contacts[i].y};
        p.PerformanceCount = timestamp;
    }
    const BOOL ok = api_.inject(api_.context, device_, pointers, active_);
    const DWORD error = ok ? ERROR_SUCCESS : (api_.error ? api_.error(api_.context) : ERROR_GEN_FAILURE);
    if (ok) active_ = 0;
    return ok ? Result{} : Result{Status::NativeFailure, error};
}

Result DeviceSession::Fail(DWORD error) noexcept {
    if (firstError_) firstError_ = {Status::NativeFailure, error};
    faulted_ = true;
    const auto cleanup = Release(true);
    if (!cleanup) firstError_.cleanupError = cleanup.nativeError;
    if (device_) api_.destroy(api_.context, device_);
    device_ = nullptr;
    active_ = 0;
    return firstError_;
}

Result DeviceSession::Submit(const GestureFrame& frame, MappedFrame& mapped) noexcept {
    mapped = {};
    if (!OnOwner()) return {Status::WrongThread};
    if (!device_ || faulted_) return {Status::InvalidState};
    auto result = MapContacts(config_, frame, mapped);
    if (!result) return result;
    return SubmitMapped(frame.phase,frame.sequence,frame.logicalSeconds,mapped);
}

Result DeviceSession::Submit(const ContactFrame& frame,MappedFrame& mapped) noexcept {
    mapped = {};
    if (!OnOwner()) return {Status::WrongThread};
    if (!device_ || faulted_) return {Status::InvalidState};
    const auto result = MapContacts(config_,frame,mapped);
    if (!result) return result;
    return SubmitMapped(frame.phase,frame.sequence,frame.logicalSeconds,mapped);
}

Result DeviceSession::SubmitMapped(Phase phase,std::uint64_t sequence,double logicalSeconds,const MappedFrame& mapped) noexcept {
    if (phase == Phase::Begin) {
        if (active_ || sequence != 0) return {Status::InvalidState};
    } else {
        if (!active_ || mapped.count != active_ || lastSequence_ == UINT64_MAX || sequence != lastSequence_+1 || logicalSeconds < lastLogicalSeconds_)
            return {Status::InvalidState};
        for (std::uint32_t i = 0; i < active_; ++i) if (mapped.contacts[i].id != last_.contacts[i].id) return {Status::InvalidState};
    }
    POINTER_TYPE_INFO pointers[5]{};
    std::uint64_t timestamp{};
    const auto stamp = clock_.Stamp(timestamp);
    if (!stamp) return Fail(stamp.nativeError);
    const bool ending = phase == Phase::End || phase == Phase::Cancel;
    for (std::uint32_t i = 0; i < mapped.count; ++i) {
        pointers[i].type = PT_TOUCHPAD;
        auto& p = pointers[i].touchInfo.pointerInfo;
        p.pointerType = PT_TOUCHPAD;
        p.pointerId = mapped.contacts[i].id;
        // Touchpad injection uses contact state, not touchscreen DOWN/UPDATE/UP flags.
        p.pointerFlags = POINTER_FLAG_CONFIDENCE;
        if (!ending) p.pointerFlags |= POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
        if (phase == Phase::Cancel) p.pointerFlags |= POINTER_FLAG_CANCELED;
        p.ptHimetricLocation = {mapped.contacts[i].x, mapped.contacts[i].y};
        p.PerformanceCount = timestamp;
    }
    // Track the attempted contacts too, so partial native failures receive best-effort cleanup.
    last_ = mapped;
    active_ = mapped.count;
    if (!api_.inject(api_.context, device_, pointers, mapped.count))
        return Fail(api_.error ? api_.error(api_.context) : ERROR_GEN_FAILURE);
    lastSequence_ = sequence; lastLogicalSeconds_ = logicalSeconds;
    if (ending) active_ = 0;
    return {};
}

Result DeviceSession::StopInertia(bool end) noexcept {
    if (!OnOwner()) return {Status::WrongThread};
    if (!device_ || faulted_) return {Status::InvalidState};
    if (!api_.action) return {Status::Unsupported, ERROR_PROC_NOT_FOUND};
    if (!api_.action(api_.context, device_, end ? TA_INERTIA_END : TA_INERTIA_STOP))
        return {Status::NativeFailure, api_.error ? api_.error(api_.context) : ERROR_GEN_FAILURE};
    return {};
}

Result DeviceSession::Close() noexcept {
    if (!OnOwner()) return {Status::WrongThread};
    if (!device_) return firstError_;
    const auto release = Release(true);
    if (!release && firstError_) firstError_ = release;
    else if (!release) firstError_.cleanupError = release.nativeError;
    if (api_.action) {
        if (!api_.action(api_.context, device_, TA_INERTIA_END)) {
            const auto error = api_.error ? api_.error(api_.context) : ERROR_GEN_FAILURE;
            if (firstError_) firstError_ = {Status::NativeFailure, error};
            else firstError_.cleanupError = error;
        }
    }
    api_.destroy(api_.context, device_);
    device_ = nullptr;
    active_ = 0;
    return firstError_;
}
}

namespace wmf {
struct PlatformInstance {
    explicit PlatformInstance(platform::NativeApi api) noexcept : session(api) {}
    platform::DeviceSession session;
};
}
extern "C" std::uint32_t WmfPlatformVersion() noexcept { return wmf::AbiVersion; }
extern "C" wmf::Result WmfPlatformCapabilities(wmf::Capabilities* caps) noexcept {
    using namespace wmf;
    if (!caps || caps->size != sizeof(Capabilities)) return {Status::InvalidArgument};
    if (caps->version != AbiVersion) return {Status::VersionMismatch};
    *caps = {};
    caps->apiBits = platform::ApiMask(platform::LoadNativeApi());
    OSVERSIONINFOW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    using VersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
#pragma warning(push)
#pragma warning(disable: 4191)
    const auto version = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
#pragma warning(pop)
    if (version && version(&os) == 0) {
        caps->osMajor = os.dwMajorVersion; caps->osMinor = os.dwMinorVersion; caps->osBuild = os.dwBuildNumber;
    }
    DWORD revision{}, bytes = sizeof(revision);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR",
        RRF_RT_REG_DWORD, nullptr, &revision, &bytes) == ERROR_SUCCESS) {
        caps->osRevision = revision; caps->revisionKnown = 1;
    }
    return {};
}
extern "C" wmf::Result WmfPlatformCreate(wmf::PlatformHandle* out) noexcept {
    if (!out) return {wmf::Status::InvalidArgument};
    if (*out) return {wmf::Status::InvalidState};
    *out = new (std::nothrow) wmf::PlatformInstance{wmf::platform::LoadNativeApi()};
    return *out ? wmf::Result{} : wmf::Result{wmf::Status::AllocationFailure};
}
extern "C" wmf::Result WmfPlatformOpen(wmf::PlatformHandle h, const wmf::DeviceConfig* c) noexcept {
    return h && c ? h->session.Open(*c) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfPlatformMap(const wmf::DeviceConfig* c, const wmf::GestureFrame* f, wmf::MappedFrame* m) noexcept {
    return c && f && m ? wmf::platform::MapContacts(*c, *f, *m) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfPlatformSubmit(wmf::PlatformHandle h, const wmf::GestureFrame* f, wmf::MappedFrame* m) noexcept {
    return h && f && m ? h->session.Submit(*f, *m) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfPlatformMapContacts(const wmf::DeviceConfig* c,const wmf::ContactFrame* f,wmf::MappedFrame* m) noexcept {
    return c && f && m ? wmf::platform::MapContacts(*c,*f,*m) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfPlatformSubmitContacts(wmf::PlatformHandle h,const wmf::ContactFrame* f,wmf::MappedFrame* m) noexcept {
    return h && f && m ? h->session.Submit(*f,*m) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfPlatformStopInertia(wmf::PlatformHandle h, std::uint32_t end) noexcept {
    if (end > 1) return {wmf::Status::InvalidArgument};
    return h ? h->session.StopInertia(end != 0) : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfPlatformClose(wmf::PlatformHandle h) noexcept {
    return h ? h->session.Close() : wmf::Result{wmf::Status::InvalidArgument};
}
extern "C" wmf::Result WmfPlatformDestroy(wmf::PlatformHandle* h) noexcept {
    if (!h) return {wmf::Status::InvalidArgument};
    if (!*h) return {};
    auto result = (*h)->session.Close();
    if (result.code == wmf::Status::WrongThread) return result;
    delete *h; *h = nullptr;
    return result;
}
