#include "util/Startup.h"
#include "util/Log.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <array>
#include <cwchar>

namespace audiomon::startup {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovalKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"AudioMonitor";
// Run commands are limited to 260 characters by Windows, even when the
// executable itself can be opened successfully through a longer path.
constexpr size_t kMaxCommandLength = 260;

std::wstring registeredCommand() {
    std::array<wchar_t, kMaxCommandLength + 2> data{};
    DWORD bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName,
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, data.data(), &bytes);
    if (status != ERROR_SUCCESS) return {};
    return data.data();
}

bool windowsAllowsStartup() {
    std::array<uint8_t, 12> data{};
    DWORD bytes = static_cast<DWORD>(data.size());
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kApprovalKey, kValueName,
        RRF_RT_REG_BINARY, nullptr, data.data(), &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS || bytes == 0) return false;
    return detail::approvalAllowsStartup({data.data(), bytes});
}

std::wstring commandExecutable(const std::wstring& command) {
    if (command.empty() || command.size() > kMaxCommandLength ||
        command.find(L'\0') != std::wstring::npos) return {};
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command.c_str(), &argc);
    if (!argv) return {};
    std::wstring path;
    if (argc == 2 && std::wcscmp(argv[1], L"--tray") == 0 &&
        !PathIsRelativeW(argv[0])) path = argv[0];
    LocalFree(argv);
    return path;
}

bool writeRegistration(bool enable, bool resetApproval) {
    const std::wstring command = enable ? detail::commandForExecutable(executablePath())
                                        : std::wstring{};
    if (enable && command.empty()) {
        LOG_WARN("startup: executable path cannot be registered (empty, invalid, or too long)");
        return false;
    }
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                    KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        LOG_WARN("startup: cannot open Run key (%ld)", status);
        return false;
    }
    if (enable) {
        status = RegSetValueExW(key, kValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kValueName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        LOG_WARN("startup: registry write failed (%ld)", status);
        return false;
    }
    // Only a deliberate settings change clears Task Manager's remembered
    // disable. Routine launch/update repair must respect that user choice.
    if (enable && resetApproval) {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, kApprovalKey, 0, KEY_SET_VALUE, &key);
        if (status == ERROR_SUCCESS) {
            status = RegDeleteValueW(key, kValueName);
            RegCloseKey(key);
        }
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND &&
            status != ERROR_PATH_NOT_FOUND) {
            LOG_WARN("startup: cannot reset Windows startup approval (%ld)", status);
            return false;
        }
    }
    LOG_INFO("startup: %s", enable ? "registered current executable for sign-in" : "disabled");
    return true;
}

} // namespace

namespace detail {

std::wstring commandForExecutable(const std::wstring& path) {
    if (path.empty() || PathIsRelativeW(path.c_str()) ||
        path.find_first_of(L"\"\r\n") != std::wstring::npos ||
        path.find(L'\0') != std::wstring::npos) return {};
    std::wstring command = L"\"" + path + L"\" --tray";
    return command.size() <= kMaxCommandLength ? command : std::wstring{};
}

bool commandMatchesExecutable(const std::wstring& command, const std::wstring& path) {
    const auto registered = commandExecutable(command);
    return !registered.empty() && !commandForExecutable(path).empty() &&
           CompareStringOrdinal(registered.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool approvalAllowsStartup(std::span<const uint8_t> data) {
    // Missing approval values allow a Run entry. Unknown/malformed values are
    // conservative: never claim startup is enabled or silently undo a disable.
    if (data.empty()) return true;
    if (data.size() != 12 || data[1] || data[2] || data[3]) return false;
    return data[0] == 2 || data[0] == 6 || data[0] == 8;
}

} // namespace detail

std::wstring executablePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return buf; }
        buf.resize(buf.size() * 2);
    }
}

bool isEnabled() {
    return windowsAllowsStartup() &&
           detail::commandMatchesExecutable(registeredCommand(), executablePath());
}

bool setEnabled(bool enable) {
    return writeRegistration(enable, true);
}

bool refreshRegistration() {
    if (!windowsAllowsStartup()) {
        LOG_INFO("startup: disabled in Windows; preserving that choice");
        return false;
    }
    const auto command = registeredCommand();
    const auto current = executablePath();
    if (detail::commandMatchesExecutable(command, current)) return true;
    const auto previous = commandExecutable(command);
    // Only repair our own existing command. A missing registration is an
    // intentional opt-out; a custom launcher is left for an explicit Save.
    if (previous.empty() || _wcsicmp(PathFindFileNameW(previous.c_str()), L"audio-monitor.exe"))
        return false;
    LOG_INFO("startup: updating the registered path after a manual launch of another copy");
    return writeRegistration(true, false);
}

} // namespace audiomon::startup
