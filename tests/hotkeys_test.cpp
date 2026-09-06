#include "util/Hotkeys.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace audiomon;

namespace {

LPARAM chordMessage(const Keybind& keybind) {
    return static_cast<LPARAM>((keybind.key << 16) | keybind.modifiers);
}

int idFor(const Hotkeys& hotkeys, const Keybind& keybind, Hotkeys::Action* action = nullptr) {
    Hotkeys::Action found;
    for (int id = 1; id <= 0xBFFF; ++id) {
        if (!hotkeys.actionFor(id, chordMessage(keybind), found)) continue;
        if (action) *action = found;
        return id;
    }
    return 0;
}

bool matches(const Hotkeys& hotkeys, const Keybind& keybind,
             Hotkeys::ActionKind kind, size_t index = 0) {
    Hotkeys::Action action;
    return idFor(hotkeys, keybind, &action) != 0 && action.kind == kind && action.index == index;
}

std::vector<Keybind> availableChords(HWND window, size_t count) {
    std::vector<Keybind> result;
    for (const UINT modifiers : {0u, UINT(MOD_CONTROL | MOD_SHIFT), UINT(MOD_ALT | MOD_SHIFT)}) {
        for (UINT key = VK_F24; key >= VK_F13 && result.size() < count; --key) {
            if (!RegisterHotKey(window, 0x7000, modifiers | MOD_NOREPEAT, key)) continue;
            UnregisterHotKey(window, 0x7000);
            result.push_back({modifiers, key});
        }
        if (result.size() == count) break;
    }
    return result;
}

// Only inject a function key already reserved by our hidden test window, so
// neither text nor commands are sent to the foreground application.
bool sendKey(UINT key, bool release) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(key);
    input.ki.dwFlags = release ? KEYEVENTF_KEYUP : 0;
    return SendInput(1, &input, sizeof(input)) == 1;
}

int drainHotkeyMessages(HWND window, const Hotkeys& hotkeys, int expectedId,
                        bool& dispatchValid) {
    int count = 0;
    MSG message{};
    while (PeekMessageW(&message, window, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
        Hotkeys::Action action;
        if (message.wParam != static_cast<WPARAM>(expectedId) ||
            !hotkeys.actionFor(static_cast<int>(message.wParam), message.lParam, action) ||
            action.kind != Hotkeys::ActionKind::Monitoring)
            dispatchValid = false;
        ++count;
    }
    return count;
}

} // namespace

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) {
            std::printf("FAIL: %s\n", label);
            ++failed;
        }
    };
    check(validKeybind({}) && keybindLabel({}).empty(), "blank keybind is valid and has no label");
    check(validKeybind({MOD_CONTROL | MOD_ALT, 'M'}) && validKeybind({0, VK_F8}) &&
              validKeybind({MOD_WIN | MOD_SHIFT, VK_OEM_1}),
          "keyboard chords support modifier combinations and unmodified keys");
    check(!validKeybind({MOD_CONTROL, 0}) && !validKeybind({MOD_NOREPEAT, 'M'}) &&
              !validKeybind({0, VK_CONTROL}) && !validKeybind({0, VK_LWIN}) &&
              !validKeybind({0, VK_LBUTTON}) && !validKeybind({0, VK_F12}) &&
              !validKeybind({0, 256}),
          "partial, modifier-only, mouse, reserved and out-of-range chords are rejected");
    check(keybindLabel({MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, 'M'}) ==
              "Ctrl + Alt + Shift + Win + M" && keybindLabel({0, VK_F24}) == "F24" &&
              keybindLabel({MOD_CONTROL, VK_NUMPAD4}) == "Ctrl + Num 4" &&
              !keybindLabel({0, VK_LEFT}).empty(),
          "key labels distinguish modifiers, function keys, numpad and navigation keys");

    Hotkeys validation;
    std::string error;
    Config config = Config::defaults();
    config.sources.resize(2);
    check(validation.apply(config, error) && error.empty(), "default config needs no hotkey registrations");
    config.monitoringKeybind = {MOD_CONTROL, 'M'};
    config.sources[0].muteKeybind = config.monitoringKeybind;
    check(!validation.apply(config, error) && !error.empty(), "duplicate actions fail validation without a window");
    config.sources[0].muteKeybind = {MOD_ALT, 'A'};
    config.sources[1].muteKeybind = {0, VK_SHIFT};
    check(!validation.apply(config, error) && !error.empty(), "invalid memory config is rejected");
    config.sources[1].muteKeybind = {};
    check(validation.apply(config, error) && error.empty() && idFor(validation, config.monitoringKeybind) == 0,
          "validation-only mode never produces dispatchable global hotkeys");

    const HWND window = CreateWindowExW(0, L"STATIC", L"Audio Monitor hotkey tests", 0,
                                        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND other = CreateWindowExW(0, L"STATIC", L"Audio Monitor hotkey conflict test", 0,
                                       0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window && other, "hidden hotkey test windows created");
    if (window && other) {
        const auto chords = availableChords(other, 7);
        check(chords.size() == 7, "enough unused function key chords available for isolated registry tests");
        if (chords.size() == 7) {
            Hotkeys hotkeys;
            hotkeys.init(window);
            config.monitoringKeybind = chords[0];
            config.sources[0].muteKeybind = chords[1];
            config.sources[1].muteKeybind = chords[2];
            ChannelConfig primary;
            primary.deviceId = L"test-primary";
            primary.muteKeybind = chords[3];
            ChannelConfig secondary;
            secondary.deviceId = L"test-secondary";
            secondary.muteKeybind = chords[4];
            check(config.addOutput(primary) && config.addOutput(secondary), "test output channels added");
            check(hotkeys.apply(config, error) && error.empty(), "monitoring and individual device chords register");
            check(matches(hotkeys, chords[0], Hotkeys::ActionKind::Monitoring) &&
                      matches(hotkeys, chords[1], Hotkeys::ActionKind::SourceMute, 0) &&
                      matches(hotkeys, chords[2], Hotkeys::ActionKind::SourceMute, 1) &&
                      matches(hotkeys, chords[3], Hotkeys::ActionKind::OutputMute, 0) &&
                      matches(hotkeys, chords[4], Hotkeys::ActionKind::OutputMute, 1),
                  "registered chords dispatch to the correct monitoring/source/output action");
            const int monitoringId = idFor(hotkeys, chords[0]);
            const int secondSourceId = idFor(hotkeys, chords[2]);
            const int secondOutputId = idFor(hotkeys, chords[4]);
            Hotkeys::Action action;
            check(!hotkeys.actionFor(monitoringId, chordMessage(chords[1]), action) &&
                      !hotkeys.actionFor(0x7000, chordMessage(chords[0]), action) &&
                      !hotkeys.actionFor(monitoringId, chordMessage(chords[0]) | MOD_NOREPEAT, action),
                  "dispatch checks both registered id and exact message chord");
            check(hotkeys.apply(config, error) && idFor(hotkeys, chords[0]) == monitoringId,
                  "unchanged settings retain registration ids");
            Config duplicate = config;
            duplicate.outputAt(1).muteKeybind = config.sources[0].muteKeybind;
            check(!hotkeys.apply(duplicate, error) && !error.empty() &&
                      matches(hotkeys, chords[4], Hotkeys::ActionKind::OutputMute, 1),
                  "duplicate source/output keybinds preserve prior registrations");

            check(RegisterHotKey(other, 0x7001, chords[6].modifiers | MOD_NOREPEAT, chords[6].key) != FALSE,
                  "reserve conflicting chord in another window");
            Config conflict = config;
            ChannelConfig extra;
            extra.muteKeybind = chords[5];
            conflict.sources.push_back(extra);
            conflict.outputAt(1).muteKeybind = chords[6];
            check(!hotkeys.apply(conflict, error) && !error.empty() &&
                      matches(hotkeys, chords[4], Hotkeys::ActionKind::OutputMute, 1) &&
                      idFor(hotkeys, chords[5]) == 0,
                  "OS conflict rolls back newly added chords and preserves the previous action map");
            check(RegisterHotKey(other, 0x7002, chords[5].modifiers | MOD_NOREPEAT, chords[5].key) != FALSE,
                  "rollback releases the provisional registration");
            UnregisterHotKey(other, 0x7002);
            check(!RegisterHotKey(other, 0x7003, chords[4].modifiers | MOD_NOREPEAT, chords[4].key),
                  "failed update retains existing Windows registrations");
            UnregisterHotKey(other, 0x7003);
            UnregisterHotKey(other, 0x7001);

            // Exercise real WM_HOTKEY delivery and NOREPEAT. These injected
            // function keys are intercepted by the registered test window.
            if (chords[0].modifiers == 0 && GetAsyncKeyState(VK_CONTROL) >= 0 &&
                GetAsyncKeyState(VK_SHIFT) >= 0 && GetAsyncKeyState(VK_MENU) >= 0 &&
                GetAsyncKeyState(VK_LWIN) >= 0 && GetAsyncKeyState(VK_RWIN) >= 0 &&
                GetAsyncKeyState(static_cast<int>(chords[0].key)) >= 0) {
                bool dispatched = true;
                drainHotkeyMessages(window, hotkeys, monitoringId, dispatched);
                const bool injected = sendKey(chords[0].key, false);
                Sleep(40);
                const int first = drainHotkeyMessages(window, hotkeys, monitoringId, dispatched);
                const bool repeated = sendKey(chords[0].key, false) && sendKey(chords[0].key, false);
                Sleep(40);
                const int repeats = drainHotkeyMessages(window, hotkeys, monitoringId, dispatched);
                const bool released = sendKey(chords[0].key, true);
                Sleep(30);
                const bool pressedAgain = sendKey(chords[0].key, false);
                Sleep(40);
                const int nextPress = drainHotkeyMessages(window, hotkeys, monitoringId, dispatched);
                const bool releasedAgain = sendKey(chords[0].key, true);
                if (injected && repeated && released && pressedAgain && releasedAgain)
                    check(first == 1 && repeats == 0 && nextPress == 1 && dispatched,
                          "Windows delivers the keybind once per press and suppresses held-key repeats");
                else
                    std::printf("SKIP: input injection unavailable for repeat test\n");
            } else {
                std::printf("SKIP: active user modifiers or no free plain key for repeat test\n");
            }

            config.sources.erase(config.sources.begin());
            config.removeOutput(0);
            check(hotkeys.apply(config, error) &&
                      matches(hotkeys, chords[2], Hotkeys::ActionKind::SourceMute, 0) &&
                      matches(hotkeys, chords[4], Hotkeys::ActionKind::OutputMute, 0) &&
                      idFor(hotkeys, chords[2]) == secondSourceId &&
                      idFor(hotkeys, chords[4]) == secondOutputId &&
                      idFor(hotkeys, chords[1]) == 0 && idFor(hotkeys, chords[3]) == 0,
                  "deleting channels keeps each remaining chord attached to its original device");

            std::swap(config.monitoringKeybind, config.sources[0].muteKeybind);
            check(hotkeys.apply(config, error) &&
                      matches(hotkeys, chords[2], Hotkeys::ActionKind::Monitoring) &&
                      matches(hotkeys, chords[0], Hotkeys::ActionKind::SourceMute, 0),
                  "swapping assigned chords retargets actions without registration conflicts");
            config.sources[0].muteKeybind = chords[5];
            check(hotkeys.apply(config, error) &&
                      !hotkeys.actionFor(monitoringId, chordMessage(chords[0]), action),
                  "queued messages from retired chords cannot trigger replacement bindings");
            config.sources[0].muteKeybind = chords[0];
            check(hotkeys.apply(config, error) && idFor(hotkeys, chords[0]) != monitoringId,
                  "reassigning a retired chord allocates a fresh message id");

            hotkeys.clear();
            check(idFor(hotkeys, chords[0]) == 0 &&
                      RegisterHotKey(other, 0x7004, chords[0].modifiers | MOD_NOREPEAT, chords[0].key) != FALSE,
                  "clear disables dispatch and releases Windows registrations");
            UnregisterHotKey(other, 0x7004);
            {
                Hotkeys temporary;
                temporary.init(window);
                check(temporary.apply(config, error), "temporary owner registers shortcuts");
            }
            check(RegisterHotKey(other, 0x7005, chords[0].modifiers | MOD_NOREPEAT, chords[0].key) != FALSE,
                  "destruction releases registrations");
            UnregisterHotKey(other, 0x7005);
        }
    }
    if (other) DestroyWindow(other);
    if (window) DestroyWindow(window);
    std::printf("Hotkey tests: %d failures\n", failed);
    return failed ? 1 : 0;
}
