#pragma once

#include "config/Config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audiomon {

bool validKeybind(const Keybind& keybind) noexcept;
// Empty keybinds have an empty label so the UI can present its own placeholder.
std::string keybindLabel(const Keybind& keybind);

// Owned and used by the window thread. Updating registrations is transactional:
// a duplicate/unavailable chord leaves every existing shortcut working.
class Hotkeys {
public:
    enum class ActionKind { Monitoring, SourceMute, OutputMute };
    struct Action {
        ActionKind kind = ActionKind::Monitoring;
        size_t index = 0;
    };

    Hotkeys() = default;
    ~Hotkeys();
    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    void init(void* hwnd);
    // A null window validates only, for rendering/settings tests.
    bool apply(const Config& config, std::string& error);
    void clear();
    bool actionFor(int id, intptr_t lparam, Action& action) const;

private:
    struct Binding {
        int id = 0;
        Keybind keybind;
        Action action;
    };
    void* hwnd_ = nullptr;
    int nextId_ = 1;
    std::vector<Binding> bindings_;
};

} // namespace audiomon
