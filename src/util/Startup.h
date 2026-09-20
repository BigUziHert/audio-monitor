#pragma once
//
// "Start with Windows" via HKCU\...\CurrentVersion\Run.
//
// HKCU rather than HKLM deliberately: it needs no administrator rights, and
// this is a per-user tool. The registered command carries --tray so a boot
// launch goes straight to the notification area.
//
#include <string>
#include <span>
#include <cstdint>
#include <vector>

namespace audiomon::startup {

bool isEnabled();
bool setEnabled(bool enable);
// Apply only a user edit; an unchanged dialog preserves external changes.
bool applyPreference(bool initial, bool desired, bool& enabled);

// A manual launch of an updated/moved copy adopts an existing enabled startup
// registration. Never creates a missing entry or overrides a Windows disable.
bool refreshRegistration();

std::wstring executablePath();

namespace detail {
enum class Entry { Run, Approval };
struct RegistryValue {
    bool exists = false;
    uint32_t type = 0;
    std::vector<uint8_t> bytes;
    bool operator==(const RegistryValue&) const = default;
};
// Tests inject registry failures without touching the user's startup settings.
struct Registry {
    virtual ~Registry() = default;
    virtual uint32_t read(Entry entry, RegistryValue& value) = 0;
    virtual uint32_t write(Entry entry, const RegistryValue& value) = 0;
};
bool isEnabled(Registry& registry, const std::wstring& path);
bool setEnabled(Registry& registry, const std::wstring& path, bool enable);
bool refreshRegistration(Registry& registry, const std::wstring& path);
bool applyPreference(Registry& registry, const std::wstring& path,
                     bool initial, bool desired, bool& enabled);
// Shared parsing rules used by registry reads and regression tests.
std::wstring commandForExecutable(const std::wstring& path);
bool commandMatchesExecutable(const std::wstring& command, const std::wstring& path);
bool approvalAllowsStartup(std::span<const uint8_t> data);
}

} // namespace audiomon::startup
