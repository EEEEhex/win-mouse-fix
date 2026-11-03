#include "DiscreteActions.h"
#include <array>

namespace wmf::platform {
namespace {
UINT Send(void*,UINT count,const INPUT* inputs) { return SendInput(count,const_cast<INPUT*>(inputs),sizeof(INPUT)); }
bool Held(void*,std::uint32_t key) { return (GetAsyncKeyState(static_cast<int>(key))&0x8000) != 0; }
DWORD Error(void*) { const auto code = GetLastError(); return code ? code : ERROR_ACCESS_DENIED; }
bool Extended(std::uint32_t key) noexcept {
    return key == VK_RCONTROL || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN ||
        key == VK_INSERT || key == VK_DELETE || key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT ||
        key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_DIVIDE || key == VK_NUMLOCK;
}
INPUT Key(std::uint32_t key,bool up) noexcept {
    INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = static_cast<WORD>(key);
    i.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (Extended(key) ? KEYEVENTF_EXTENDEDKEY : 0);
    i.ki.dwExtraInfo = ReplayMarker; return i;
}
Result SendOwned(const ActionNative& api,const INPUT* inputs,UINT count) noexcept {
    if (!count) return {};
    SetLastError(ERROR_SUCCESS); const auto sent = api.send(api.context,count,inputs);
    if (sent == count) return {};
    Result result{Status::NativeFailure,api.error(api.context)};
    if (!result.nativeError) result.nativeError = ERROR_ACCESS_DENIED;
    // Determine only the keys actually pressed by the accepted prefix. Never
    // release a modifier observed as held before this explicitly bound action.
    std::array<INPUT,MaximumShortcutKeys> releases{}; UINT outstanding{};
    for (UINT j = 0; j < sent && j < count; ++j) {
        const auto& input = inputs[j];
        const bool up = input.type == INPUT_KEYBOARD ? (input.ki.dwFlags & KEYEVENTF_KEYUP) != 0 :
            (input.mi.dwFlags & (MOUSEEVENTF_XUP|MOUSEEVENTF_MIDDLEUP)) != 0;
        if (up) { if (outstanding) --outstanding; }
        else if (outstanding < releases.size()) {
            auto release = input;
            if (input.type == INPUT_KEYBOARD) release.ki.dwFlags |= KEYEVENTF_KEYUP;
            else release.mi.dwFlags = input.mi.dwFlags == MOUSEEVENTF_XDOWN ? MOUSEEVENTF_XUP : MOUSEEVENTF_MIDDLEUP;
            releases[outstanding++] = release;
        }
    }
    for (UINT i = 0; i < outstanding/2; ++i) std::swap(releases[i],releases[outstanding-i-1]);
    if (outstanding && api.send(api.context,outstanding,releases.data()) != outstanding) {
        result.cleanupError = api.error(api.context); if (!result.cleanupError) result.cleanupError = ERROR_ACCESS_DENIED;
    }
    return result;
}
}
ActionNative RealActionNative() noexcept { return {nullptr,Send,Held,Error}; }
Result ExecuteAction(const ActionRequest& request,const ActionNative& api) noexcept {
    if (request.size != sizeof(request) || !api.send || !api.held || !api.error || !request.repeatCount ||
        request.repeatCount > 32 || request.reserved || request.binding.reserved[0] || request.binding.reserved[1]) return {Status::InvalidArgument};
    if (request.version != AbiVersion) return {Status::VersionMismatch};
    const auto& binding = request.binding;
    std::array<INPUT,MaximumShortcutKeys*2> inputs{}; UINT count{};
    if (binding.action == Action::Back || binding.action == Action::Forward || binding.action == Action::MiddleClick) {
        if (binding.keyCount) return {Status::InvalidArgument};
        INPUT down{}; down.type = INPUT_MOUSE; down.mi.dwExtraInfo = ReplayMarker;
        down.mi.dwFlags = binding.action == Action::MiddleClick ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_XDOWN;
        down.mi.mouseData = binding.action == Action::Back ? XBUTTON1 : binding.action == Action::Forward ? XBUTTON2 : 0;
        inputs[0] = down; inputs[1] = down;
        inputs[1].mi.dwFlags = binding.action == Action::MiddleClick ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_XUP; count = 2;
    } else {
        std::array<std::uint32_t,MaximumShortcutKeys> keys{}; UINT keyCount{};
        if (binding.action == Action::Shortcut) {
            if (!binding.keyCount || binding.keyCount > MaximumShortcutKeys) return {Status::InvalidArgument};
            for (UINT i = 0; i < binding.keyCount; ++i) {
                const auto key = binding.keys[i]; if (key < 8 || key > 254) return {Status::InvalidArgument};
                for (UINT j = 0; j < i; ++j) if (binding.keys[j] == key) return {Status::InvalidArgument};
                keys[keyCount++] = key;
            }
        } else {
            if (binding.keyCount) return {Status::InvalidArgument};
            switch (binding.action) {
            case Action::VolumeUp: keys[keyCount++] = VK_VOLUME_UP; break;
            case Action::VolumeDown: keys[keyCount++] = VK_VOLUME_DOWN; break;
            case Action::Mute: keys[keyCount++] = VK_VOLUME_MUTE; break;
            case Action::PlayPause: keys[keyCount++] = VK_MEDIA_PLAY_PAUSE; break;
            case Action::PreviousTrack: keys[keyCount++] = VK_MEDIA_PREV_TRACK; break;
            case Action::NextTrack: keys[keyCount++] = VK_MEDIA_NEXT_TRACK; break;
            default: return {Status::InvalidArgument}; // Native gestures have no keyboard fallback.
            }
        }
        for (UINT i = 0; i < keyCount; ++i) if (!api.held(api.context,keys[i])) inputs[count++] = Key(keys[i],false);
        const auto downs = count;
        for (UINT i = 0; i < downs; ++i) { inputs[count] = inputs[downs-i-1]; inputs[count++].ki.dwFlags |= KEYEVENTF_KEYUP; }
    }
    for (UINT repeat = 0; repeat < request.repeatCount; ++repeat) {
        const auto result = SendOwned(api,inputs.data(),count); if (!result) return result;
    }
    return {};
}
}
extern "C" wmf::Result WmfExecuteAction(const wmf::ActionRequest* request) noexcept {
    return request ? wmf::platform::ExecuteAction(*request,wmf::platform::RealActionNative()) : wmf::Result{wmf::Status::InvalidArgument};
}
