#include "ResidentTray.h"
#include "Resource.h"
#include <commctrl.h>
#include <stdexcept>

namespace wmf::host {
namespace {
constexpr GUID TrayGuid{0x46968cb3,0x54ee,0x414e,{0x90,0x21,0x82,0x7f,0x0e,0x6a,0x5d,0x76}};
BOOL Notify(void*,DWORD command,NOTIFYICONDATAW* data) { return Shell_NotifyIconW(command,data); }
HICON Load(void*,HINSTANCE module,int size) { HICON icon{}; return SUCCEEDED(LoadIconWithScaleDown(module,MAKEINTRESOURCEW(IDI_WINMOUSEFIXHOST),size,size,&icon)) ? icon : nullptr; }
void Destroy(void*,HICON icon) { DestroyIcon(icon); }
}
TrayApi RealTrayApi() noexcept { return {nullptr,Notify,Load,Destroy,false}; }
ResidentTray::ResidentTray(const ProductEnvironment& environment,HWND window,HINSTANCE module,TrayApi api) : api_(api),window_(window),module_(module) {
    if (!api.notify || !api.load || !api.destroy || api.simulation != environment.simulation ||
        (environment.simulation && (api.notify == Notify || api.load == Load || api.destroy == Destroy))) throw std::invalid_argument("Tray backend does not match isolation environment");
}
ResidentTray::~ResidentTray() { Remove(); }
Result ResidentTray::Update(std::wstring_view text,bool rebuild) {
    if (rebuild) added_ = false;
    auto dpi = GetDpiForWindow(window_); if (!dpi) dpi = 96;
    const auto size = GetSystemMetricsForDpi(SM_CXSMICON,dpi);
    HICON replacement{};
    if (!icon_ || size != size_) { replacement = api_.load(api_.context,module_,size); if (!replacement) return {Status::NativeFailure,ERROR_RESOURCE_DATA_NOT_FOUND}; }
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.guidItem = TrayGuid; data.uCallbackMessage = ResidentTrayMessage;
    data.uFlags = NIF_GUID|NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP; data.hIcon = replacement ? replacement : icon_;
    wcsncpy_s(data.szTip,text.data(),(std::min)(text.size(),std::size(data.szTip)-1));
    if (!api_.notify(api_.context,added_ ? NIM_MODIFY : NIM_ADD,&data)) {
        if (replacement) api_.destroy(api_.context,replacement); return {Status::NativeFailure,ERROR_GEN_FAILURE};
    }
    if (replacement) { if (icon_) api_.destroy(api_.context,icon_); icon_ = replacement; size_ = size; }
    if (!added_) { added_ = true; data.uVersion = NOTIFYICON_VERSION_4; if (!api_.notify(api_.context,NIM_SETVERSION,&data)) return {Status::NativeFailure,ERROR_GEN_FAILURE}; }
    return {};
}
Result ResidentTray::NotifyStartup(bool enabled) {
    if (!enabled || startupNotificationAttempted_) return {};
    if (!added_) return {Status::InvalidState};
    startupNotificationAttempted_ = true;
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.guidItem = TrayGuid;
    data.uFlags = NIF_GUID|NIF_INFO|NIF_REALTIME; data.dwInfoFlags = NIIF_INFO|NIIF_NOSOUND;
    wcscpy_s(data.szInfoTitle,L"Win Mouse Fix");
    wcscpy_s(data.szInfo,L"Win Mouse Fix 正在后台运行。单击托盘图标打开设置。");
    if (!api_.notify(api_.context,NIM_MODIFY,&data)) return {Status::NativeFailure,ERROR_GEN_FAILURE};
    return {};
}
Result ResidentTray::Remove() noexcept {
    Result result;
    if (added_) { NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = window_; data.guidItem = TrayGuid; data.uFlags = NIF_GUID;
        if (!api_.notify(api_.context,NIM_DELETE,&data)) result = {Status::NativeFailure,ERROR_GEN_FAILURE}; added_ = false; }
    if (icon_) { api_.destroy(api_.context,icon_); icon_ = nullptr; } return result;
}
}
