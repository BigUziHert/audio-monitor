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

namespace audiomon::startup {

bool isEnabled();
bool setEnabled(bool enable);

// A manual launch of an updated/moved copy adopts an existing enabled startup
// registration. Never creates a missing entry or overrides a Windows disable.
bool refreshRegistration();

std::wstring executablePath();

namespace detail {
// Shared parsing rules used by registry reads and regression tests.
std::wstring commandForExecutable(const std::wstring& path);
bool commandMatchesExecutable(const std::wstring& command, const std::wstring& path);
bool approvalAllowsStartup(std::span<const uint8_t> data);
}

} // namespace audiomon::startup
