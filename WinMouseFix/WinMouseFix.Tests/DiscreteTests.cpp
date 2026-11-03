#include "../WinMouseFix.Platform.Win32/DiscreteActions.h"
#include <array>
#include <vector>
#include <iostream>

namespace {
using namespace wmf;
using namespace wmf::platform;
int checks{},failures{};
#define VERIFY(...) do { ++checks; if (!(__VA_ARGS__)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__ << '\n'; } } while(false)
struct Fake {
    std::array<bool,256> held{};
    std::vector<std::vector<INPUT>> calls;
    UINT accept{UINT_MAX}; bool cleanupFails{};
    ActionNative Api() {
        return {this,
            [](void* p,UINT count,const INPUT* inputs) -> UINT {
                auto& f = *static_cast<Fake*>(p); f.calls.emplace_back(inputs,inputs+count);
                return f.calls.size() == 1 ? (std::min)(count,f.accept) : f.cleanupFails ? 0 : count;
            },
            [](void* p,std::uint32_t key) { return static_cast<Fake*>(p)->held[key]; },
            [](void*) -> DWORD { return 5; }};
    }
};
void Discrete() {
    for (const auto action : {Action::Back,Action::Forward,Action::MiddleClick,Action::VolumeUp,Action::VolumeDown,
        Action::Mute,Action::PlayPause,Action::PreviousTrack,Action::NextTrack}) {
        Fake f; ActionRequest r; r.binding.action = action; r.repeatCount = 3;
        VERIFY(ExecuteAction(r,f.Api())); VERIFY(f.calls.size() == 3);
        for (const auto& call : f.calls) {
            VERIFY(call.size() == 2);
            VERIFY(call[0].type == call[1].type);
            if (call[0].type == INPUT_MOUSE) {
                VERIFY(call[0].mi.dwExtraInfo == ReplayMarker && call[1].mi.dwExtraInfo == ReplayMarker);
                VERIFY(call[0].mi.mouseData == static_cast<DWORD>(action == Action::Back ? XBUTTON1 : action == Action::Forward ? XBUTTON2 : 0));
            } else {
                VERIFY(call[0].ki.dwExtraInfo == ReplayMarker && call[1].ki.dwExtraInfo == ReplayMarker);
                VERIFY((call[1].ki.dwFlags & KEYEVENTF_KEYUP) && !(call[0].ki.dwFlags & KEYEVENTF_KEYUP));
            }
        }
    }
    ActionRequest r; r.binding.action = Action::Shortcut; r.binding.keyCount = 3;
    r.binding.keys[0] = VK_CONTROL; r.binding.keys[1] = VK_SHIFT; r.binding.keys[2] = 'T';
    for (UINT accepted = 0; accepted <= 6; ++accepted) {
        Fake f; f.accept = accepted; auto result = ExecuteAction(r,f.Api());
        VERIFY(static_cast<bool>(result) == (accepted == 6));
        std::array<bool,256> owned{};
        for (UINT j = 0; j < accepted; ++j) {
            const auto& key = f.calls[0][j].ki; owned[key.wVk] = !(key.dwFlags & KEYEVENTF_KEYUP);
        }
        if (f.calls.size() == 2) for (const auto& input : f.calls[1]) {
            VERIFY(owned[input.ki.wVk] && (input.ki.dwFlags & KEYEVENTF_KEYUP)); owned[input.ki.wVk] = false;
        }
        for (const auto key : owned) VERIFY(!key);
    }
    Fake held; held.held[VK_CONTROL] = true; held.held[VK_SHIFT] = true;
    VERIFY(ExecuteAction(r,held.Api())); VERIFY(held.calls[0].size() == 2 && held.calls[0][0].ki.wVk == 'T');
    held.calls.clear(); held.held['T'] = true; VERIFY(ExecuteAction(r,held.Api()) && held.calls.empty());
    Fake failed; failed.accept = 2; failed.cleanupFails = true;
    const auto result = ExecuteAction(r,failed.Api()); VERIFY(result.nativeError == 5 && result.cleanupError == 5);
    for (const auto native : {Action::ContentDrag,Action::ShellGestures,Action::NativeZoom,Action::None}) {
        Fake f; ActionRequest gesture; gesture.binding.action = native;
        VERIFY(!ExecuteAction(gesture,f.Api()) && f.calls.empty());
    }
    Fake invalid; r.binding.keys[2] = VK_SHIFT; VERIFY(!ExecuteAction(r,invalid.Api()) && invalid.calls.empty());
    r.binding.keys[2] = 'T'; r.version = 0; VERIFY(ExecuteAction(r,invalid.Api()).code == Status::VersionMismatch);
}
}
int RunDiscreteTests(int& total) { Discrete(); total += checks; return failures; }
