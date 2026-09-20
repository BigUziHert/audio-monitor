#include "util/Startup.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <windows.h>

using namespace audiomon::startup;

namespace {
detail::RegistryValue textValue(const std::wstring& text, uint32_t type = REG_SZ) {
    detail::RegistryValue value{true, type, {}};
    value.bytes.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.bytes.data(), text.c_str(), value.bytes.size());
    return value;
}
detail::RegistryValue approvalValue(uint8_t state) {
    detail::RegistryValue value{true, REG_BINARY, std::vector<uint8_t>(12)};
    value.bytes[0] = state;
    return value;
}
struct FakeRegistry final : detail::Registry {
    detail::RegistryValue run, approval;
    uint32_t runReadError = ERROR_SUCCESS, approvalReadError = ERROR_SUCCESS;
    uint32_t runWriteError = ERROR_SUCCESS, approvalWriteError = ERROR_SUCCESS;
    unsigned writes = 0;
    uint32_t read(detail::Entry entry, detail::RegistryValue& value) override {
        value = entry == detail::Entry::Run ? run : approval;
        return entry == detail::Entry::Run ? runReadError : approvalReadError;
    }
    uint32_t write(detail::Entry entry, const detail::RegistryValue& value) override {
        ++writes;
        const auto error = entry == detail::Entry::Run ? runWriteError : approvalWriteError;
        if (error != ERROR_SUCCESS) return error;
        (entry == detail::Entry::Run ? run : approval) = value;
        return ERROR_SUCCESS;
    }
};
}

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("FAIL: %s\n", label); ++failed; }
    };
    const std::wstring path = L"C:\\Users\\Test User\\Audio Monitor\\audio-monitor.exe";
    const auto command = detail::commandForExecutable(path);
    check(command == L"\"C:\\Users\\Test User\\Audio Monitor\\audio-monitor.exe\" --tray",
          "startup quotes paths containing spaces and launches in the tray");
    check(detail::commandMatchesExecutable(command, path), "registered current executable is enabled");
    check(detail::commandMatchesExecutable(command, L"c:\\users\\test user\\audio monitor\\AUDIO-MONITOR.EXE"),
          "Windows path comparison is case insensitive");
    check(!detail::commandMatchesExecutable(command, L"C:\\New Build\\audio-monitor.exe"),
          "a previous build cannot make the current build report startup enabled");
    check(!detail::commandMatchesExecutable(L"\"" + path + L"\"", path),
          "a command without the startup argument is not a valid registration");
    check(!detail::commandMatchesExecutable(command + L" --other", path),
          "unexpected startup arguments do not match our registration");
    check(!detail::commandMatchesExecutable(path + L" --tray", path),
          "unquoted paths with spaces cannot silently pass validation");
    check(!detail::commandMatchesExecutable(L"audio-monitor.exe --tray", path),
          "relative commands cannot be mistaken for the current executable");
    check(detail::commandForExecutable(L"").empty() &&
              detail::commandForExecutable(L"audio-monitor.exe").empty() &&
              detail::commandForExecutable(L"C:\\bad\"path.exe").empty(),
          "empty, relative, and malformed executable paths are rejected");
    const std::wstring embeddedNull = path + std::wstring(1, L'\0') + L".other";
    check(detail::commandForExecutable(embeddedNull).empty() &&
              !detail::commandMatchesExecutable(command, embeddedNull) &&
              !detail::commandMatchesExecutable(command + std::wstring(1, L'\0') + L" --other", path),
          "embedded nulls cannot conceal a different command");
    check(!detail::commandForExecutable(L"C:\\" + std::wstring(248, L'a')).empty() &&
              detail::commandForExecutable(L"C:\\" + std::wstring(249, L'a')).empty(),
          "registration enforces the Windows Run command's 260-character limit");
    std::array<uint8_t, 12> approval{};
    for (const uint8_t value : {uint8_t{2}, uint8_t{6}, uint8_t{8}}) {
        approval[0] = value;
        check(detail::approvalAllowsStartup(approval), "enabled Windows approval is recognized");
    }
    for (const uint8_t value : {uint8_t{1}, uint8_t{3}, uint8_t{7}, uint8_t{9}, uint8_t{99}}) {
        approval[0] = value;
        check(!detail::approvalAllowsStartup(approval), "disabled or unknown Windows approval stays disabled");
    }
    approval[0] = 2;
    approval[1] = 1;
    check(!detail::approvalAllowsStartup(approval) &&
              !detail::approvalAllowsStartup({approval.data(), 1}),
          "malformed approval does not report startup enabled");
    check(detail::approvalAllowsStartup({}), "absence of an approval value allows startup");

    check(detail::commandForExecutable(L"\\audio-monitor.exe").empty() &&
              detail::commandForExecutable(L"C:audio-monitor.exe").empty(),
          "root-relative and drive-relative executable paths are rejected");

    FakeRegistry registry;
    check(!detail::refreshRegistration(registry, path) && registry.writes == 0,
          "manual launch preserves a missing startup entry");
    registry.run = textValue(command);
    check(detail::isEnabled(registry, path), "real registration policy recognizes current command");
    const std::wstring moved = L"C:\\New Build\\audio-monitor.exe";
    check(detail::refreshRegistration(registry, moved) &&
              registry.run == textValue(detail::commandForExecutable(moved)),
          "manual launch repairs an existing enabled portable path");
    registry.approval = approvalValue(3);
    const auto disabledRun = registry.run;
    const auto disabledApproval = registry.approval;
    registry.writes = 0;
    check(!detail::refreshRegistration(registry, path) && registry.run == disabledRun &&
              registry.approval == disabledApproval && registry.writes == 0,
          "routine launch cannot undo Windows Startup Apps disable");
    check(detail::setEnabled(registry, path, true) && detail::isEnabled(registry, path) &&
              !registry.approval.exists,
          "an explicit enable registers current path and clears Windows disable");
    check(detail::setEnabled(registry, path, false) && !registry.run.exists,
          "explicit disable removes startup registration");

    registry = FakeRegistry{};
    registry.run = textValue(L"\"C:\\Custom Launcher.exe\" --custom");
    const auto customRun = registry.run;
    check(!detail::refreshRegistration(registry, path) && registry.run == customRun && registry.writes == 0,
          "manual launch preserves custom startup commands");
    registry.run = textValue(command + std::wstring(1, L'\0') + L" extra");
    check(!detail::isEnabled(registry, path) && !detail::refreshRegistration(registry, moved),
          "raw registry strings cannot hide a suffix after a null");
    registry.run = textValue(command);
    registry.run.bytes.pop_back();
    check(!detail::isEnabled(registry, path), "odd byte count cannot be decoded as a Run command");
    registry.run = textValue(command);
    registry.run.bytes.resize(registry.run.bytes.size() - sizeof(wchar_t));
    check(!detail::isEnabled(registry, path), "unterminated registry strings are rejected");
    wchar_t windowsDirectory[MAX_PATH]{};
    GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    registry.run = textValue(L"\"%SystemRoot%\\audio-monitor.exe\" --tray", REG_EXPAND_SZ);
    check(detail::isEnabled(registry, std::wstring(windowsDirectory) + L"\\audio-monitor.exe"),
          "expandable Run commands are expanded before path comparison");
    registry.run = textValue(command);
    registry.approval = {true, REG_BINARY, {}};
    check(!detail::isEnabled(registry, path), "empty existing approval is malformed, not missing");
    registry.approval = approvalValue(2);
    registry.approval.type = REG_SZ;
    check(!detail::isEnabled(registry, path), "approval requires its binary registry type");

    for (const bool existing : {false, true}) {
        registry = FakeRegistry{};
        if (existing) registry.run = textValue(L"\"C:\\Old\\audio-monitor.exe\" --tray", REG_EXPAND_SZ);
        registry.approval = approvalValue(3);
        registry.approvalWriteError = ERROR_ACCESS_DENIED;
        const auto beforeRun = registry.run, beforeApproval = registry.approval;
        check(!detail::setEnabled(registry, path, true) && registry.run == beforeRun &&
                  registry.approval == beforeApproval,
              "failed approval reset restores the exact original Run type/value or absence");
    }
    registry = FakeRegistry{};
    registry.approvalReadError = ERROR_ACCESS_DENIED;
    check(!detail::setEnabled(registry, path, true) && registry.writes == 0,
          "approval read failure cannot partially enable startup");
    registry.run = textValue(command);
    check(detail::setEnabled(registry, path, false) && !registry.run.exists,
          "explicit disable does not require access to the approval key");
    registry = FakeRegistry{};
    registry.runReadError = ERROR_ACCESS_DENIED;
    check(!detail::setEnabled(registry, path, true) && registry.writes == 0,
          "Run read failure causes no writes");
    registry = FakeRegistry{};
    registry.run = textValue(command);
    registry.runWriteError = ERROR_ACCESS_DENIED;
    check(!detail::setEnabled(registry, moved, true) && registry.run == textValue(command),
          "failed Run write preserves the old executable");

    registry = FakeRegistry{};
    registry.run = textValue(command);
    registry.approval = approvalValue(3); // Disabled externally after Settings opened.
    bool effective = true;
    check(detail::applyPreference(registry, path, true, true, effective) && !effective &&
              registry.writes == 0 && registry.run.exists,
          "saving unrelated preferences preserves an external disable and reports it");
    registry.approval = {}; // Re-enabled externally after Settings opened disabled.
    check(detail::applyPreference(registry, path, false, false, effective) && effective &&
              registry.writes == 0,
          "saving unrelated preferences also preserves an external enable");
    registry.approval = approvalValue(3);
    check(detail::applyPreference(registry, path, false, true, effective) && effective,
          "changing the refreshed checkbox explicitly re-enables Windows startup");
    registry = FakeRegistry{};
    check(!detail::setEnabled(registry, L"C:\\" + std::wstring(249, L'a'), true) && registry.writes == 0,
          "overlong startup paths fail before any registry mutation");
    std::printf("startup: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
