#include "NativeModules.h"
#include "NativeApi.h"
#include "ContentDragApi.h"
#include "ProductApi.h"
#include <delayimp.h>
#include <array>

namespace wmf::host {
namespace {
bool CheckModule(const std::filesystem::path& file, const char* versionName,
    const char* const* entries, std::size_t count, HMODULE& out, std::wstring& error) {
    out = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!out) {
        error = file.filename().wstring() + L": load failed, Win32=" + std::to_wstring(GetLastError());
        return false;
    }
    const auto address = GetProcAddress(out, versionName);
    if (!address) { error = file.filename().wstring() + L": missing version entry"; return false; }
#pragma warning(push)
#pragma warning(disable: 4191)
    const auto version = reinterpret_cast<std::uint32_t(*)() noexcept>(address);
#pragma warning(pop)
    if (version() != AbiVersion) { error = file.filename().wstring() + L": ABI version mismatch"; return false; }
    for (std::size_t i = 0; i < count; ++i) {
        if (!GetProcAddress(out, entries[i])) {
            error = file.filename().wstring() + L": missing ABI entry at index " + std::to_wstring(i);
            return false;
        }
    }
    return true;
}
}
bool NativeModules::Load(const std::filesystem::path& directory, std::wstring& error) {
    if (core_ || platform_) { error = L"Modules already loaded"; return false; }
    constexpr const char* coreEntries[]{"WmfCoreCreate", "WmfCoreNext", "WmfCoreControl", "WmfCoreDestroy",
        "WmfDragCreate", "WmfDragFeed", "WmfDragReset", "WmfDragDestroy",
        "WmfValidateBindings", "WmfValidateInput", "WmfTranslateContacts",
        "WmfBindingsCreate", "WmfBindingsFeed", "WmfBindingsDeadline", "WmfBindingsDestroy",
        "WmfWheelCreate", "WmfWheelFeed", "WmfWheelSample", "WmfWheelDestroy"};
    constexpr const char* platformEntries[]{"WmfPlatformCapabilities", "WmfPlatformCreate", "WmfPlatformOpen", "WmfPlatformMap",
        "WmfPlatformSubmit", "WmfPlatformStopInertia", "WmfPlatformClose", "WmfPlatformDestroy",
        "WmfCaptureCreate", "WmfCapturePost", "WmfCaptureReadStats", "WmfCaptureDestroy",
        "WmfPlatformMapContacts", "WmfPlatformSubmitContacts", "WmfExecuteAction",
        "WmfProductCaptureCreate", "WmfProductCaptureUpdate", "WmfProductCapturePolicy", "WmfProductCaptureRecording", "WmfProductCaptureRevision"};
    const auto root = std::filesystem::absolute(directory);
    return CheckModule(root / L"WinMouseFix.Core.dll", "WmfCoreVersion", coreEntries, std::size(coreEntries), core_, error) &&
        CheckModule(root / L"WinMouseFix.Platform.Win32.dll", "WmfPlatformVersion", platformEntries,
            std::size(platformEntries), platform_, error);
}
NativeModules::~NativeModules() {
    // The caller has already joined the input thread and destroyed all DLL-owned instances.
    __FUnloadDelayLoadedDLL2("WinMouseFix.Platform.Win32.dll");
    __FUnloadDelayLoadedDLL2("WinMouseFix.Core.dll");
    if (platform_) FreeLibrary(platform_);
    if (core_) FreeLibrary(core_);
}
}
