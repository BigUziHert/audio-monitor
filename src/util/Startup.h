#pragma once
//
// "Start with Windows" via HKCU\...\CurrentVersion\Run.
//
// HKCU rather than HKLM deliberately: it needs no administrator rights, and
// this is a per-user tool. The registered command carries --tray so a boot
// launch goes straight to the notification area.
//
#include <string>

namespace audiomon::startup {

bool isEnabled();
bool setEnabled(bool enable);

std::wstring executablePath();

} // namespace audiomon::startup
