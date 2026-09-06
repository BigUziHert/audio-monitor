#include "util/Hotkeys.h"
#include "util/Text.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace audiomon {

bool validKeybind(const Keybind& keybind) noexcept {
    constexpr uint32_t allowed = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    if (keybind.modifiers & ~allowed) return false;
    if (keybind.empty()) return keybind.modifiers == 0;
    const uint32_t key = keybind.key;
    // RegisterHotKey accepts virtual keyboard keys; modifier keys themselves
    // are captured as modifiers, and F12 is permanently reserved for debugging.
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LCONTROL ||
        key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU ||
        key == VK_LWIN || key == VK_RWIN || key == VK_F12) return false;
    return key == VK_BACK || key == VK_TAB || key == VK_CLEAR || key == VK_RETURN ||
           key == VK_PAUSE || key == VK_CAPITAL || key == VK_ESCAPE ||
           (key >= VK_SPACE && key <= VK_HELP) ||
           (key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') ||
           key == VK_APPS || key == VK_SLEEP ||
           (key >= VK_NUMPAD0 && key <= VK_DIVIDE) ||
           (key >= VK_F1 && key <= VK_F24) || key == VK_NUMLOCK || key == VK_SCROLL ||
           (key >= VK_BROWSER_BACK && key <= VK_LAUNCH_APP2) ||
           (key >= VK_OEM_1 && key <= VK_OEM_3) ||
           (key >= VK_OEM_4 && key <= VK_OEM_8) || key == VK_OEM_102;
}

std::string keybindLabel(const Keybind& keybind) {
    if (keybind.empty()) return {};
    std::string label;
    if (keybind.modifiers & MOD_CONTROL) label += "Ctrl + ";
    if (keybind.modifiers & MOD_ALT) label += "Alt + ";
    if (keybind.modifiers & MOD_SHIFT) label += "Shift + ";
    if (keybind.modifiers & MOD_WIN) label += "Win + ";
    const UINT key = keybind.key;
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
        return label + static_cast<char>(key);
    if (key >= VK_F1 && key <= VK_F24)
        return label + "F" + std::to_string(key - VK_F1 + 1);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return label + "Num " + std::to_string(key - VK_NUMPAD0);
    switch (key) {
    case VK_MULTIPLY: return label + "Num *";
    case VK_ADD: return label + "Num +";
    case VK_SUBTRACT: return label + "Num -";
    case VK_DECIMAL: return label + "Num .";
    case VK_DIVIDE: return label + "Num /";
    default: break;
    }
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);
    switch (key) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_NUMLOCK: case VK_SNAPSHOT:
    case VK_APPS: case VK_DIVIDE:
        scan |= 0x100;
        break;
    default: break;
    }
    wchar_t name[128]{};
    if (scan && GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 128) > 0)
        return label + toUtf8(name);
    char fallback[24]{};
    std::snprintf(fallback, sizeof(fallback), "Key 0x%02X", key);
    return label + fallback;
}

Hotkeys::~Hotkeys() { clear(); }

void Hotkeys::init(void* hwnd) {
    clear();
    hwnd_ = hwnd;
}

bool Hotkeys::apply(const Config& config, std::string& error) {
    error.clear();
    std::vector<Binding> desired;
    auto add = [&](const Keybind& keybind, ActionKind kind, size_t index,
                   const std::string& name) {
        if (!validKeybind(keybind)) {
            error = "Choose a valid keyboard shortcut for " + name + ".";
            return false;
        }
        if (keybind.empty()) return true;
        if (std::any_of(desired.begin(), desired.end(), [&](const Binding& binding) {
                return binding.keybind == keybind;
            })) {
            error = keybindLabel(keybind) + " is assigned more than once. Choose a different shortcut.";
            return false;
        }
        desired.push_back({0, keybind, {kind, index}});
        return true;
    };
    if (!add(config.monitoringKeybind, ActionKind::Monitoring, 0, "Start/Stop Monitoring"))
        return false;
    for (size_t i = 0; i < config.sources.size(); ++i)
        if (!add(config.sources[i].muteKeybind, ActionKind::SourceMute, i,
                 config.sources[i].label.empty() ? "source mute" : config.sources[i].label))
            return false;
    for (size_t i = 0; i < config.outputCount(); ++i)
        if (!add(config.outputAt(i).muteKeybind, ActionKind::OutputMute, i,
                 config.outputAt(i).label.empty() ? "output mute" : config.outputAt(i).label))
            return false;
    if (!hwnd_) return true;

    std::vector<int> added;
    const HWND window = static_cast<HWND>(hwnd_);
    for (auto& binding : desired) {
        const auto existing = std::find_if(bindings_.begin(), bindings_.end(),
            [&](const Binding& old) { return old.keybind == binding.keybind; });
        if (existing != bindings_.end()) {
            binding.id = existing->id;
            continue;
        }
        // IDs are never recycled during this object's lifetime, including after
        // clear/init, so queued messages from a retired chord stay retired.
        if (nextId_ > 0xBFFF) {
            error = "Shortcut registration limit reached. Restart Audio Monitor to add more shortcuts.";
        } else {
            binding.id = nextId_++;
            if (RegisterHotKey(window, binding.id,
                               binding.keybind.modifiers | MOD_NOREPEAT, binding.keybind.key)) {
                added.push_back(binding.id);
                continue;
            }
            error = keybindLabel(binding.keybind) +
                    " is unavailable or used by another app. Choose a different shortcut.";
        }
        for (const int id : added) UnregisterHotKey(window, id);
        return false;
    }
    for (const auto& old : bindings_)
        if (std::none_of(desired.begin(), desired.end(),
                        [&](const Binding& binding) { return binding.id == old.id; }))
            UnregisterHotKey(window, old.id);
    bindings_ = std::move(desired);
    return true;
}

void Hotkeys::clear() {
    if (hwnd_)
        for (const auto& binding : bindings_)
            UnregisterHotKey(static_cast<HWND>(hwnd_), binding.id);
    bindings_.clear();
}

bool Hotkeys::actionFor(int id, intptr_t lparam, Action& action) const {
    if (!hwnd_) return false;
    const auto binding = std::find_if(bindings_.begin(), bindings_.end(),
                                    [&](const Binding& item) { return item.id == id; });
    if (binding == bindings_.end()) return false;
    const auto packed = static_cast<uintptr_t>(lparam);
    if ((packed & 0xffff) != binding->keybind.modifiers ||
        ((packed >> 16) & 0xffff) != binding->keybind.key) return false;
    action = binding->action;
    return true;
}

} // namespace audiomon
