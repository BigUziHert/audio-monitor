#include "util/Startup.h"

#include <array>
#include <cstdio>

using namespace audiomon::startup;

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
    std::printf("startup: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
