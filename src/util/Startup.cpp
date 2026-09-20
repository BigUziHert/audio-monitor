#include "util/Startup.h"
#include "util/Log.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <cstring>
#include <cwchar>

namespace audiomon::startup {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovalKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"AudioMonitor";
constexpr size_t kMaxCommandLength = 260;

class WindowsRegistry final : public detail::Registry {
public:
    uint32_t read(detail::Entry entry, detail::RegistryValue& value) override {
        value = {};
        HKEY key = nullptr;
        LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath(entry), 0, KEY_QUERY_VALUE, &key);
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
        if (status != ERROR_SUCCESS) return status;
        DWORD type = 0, bytes = 0;
        status = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes);
        if (status == ERROR_FILE_NOT_FOUND) { RegCloseKey(key); return ERROR_SUCCESS; }
        if (status == ERROR_SUCCESS && bytes > 65536) status = ERROR_MORE_DATA;
        if (status == ERROR_SUCCESS) {
            value.bytes.resize(bytes);
            status = RegQueryValueExW(key, kValueName, nullptr, &type, value.bytes.data(), &bytes);
            if (status == ERROR_SUCCESS && bytes > value.bytes.size()) status = ERROR_MORE_DATA;
            if (status == ERROR_SUCCESS) {
                value.exists = true;
                value.type = type;
                value.bytes.resize(bytes);
            }
        }
        RegCloseKey(key);
        return status;
    }

    uint32_t write(detail::Entry entry, const detail::RegistryValue& value) override {
        HKEY key = nullptr;
        LSTATUS status = value.exists
            ? RegCreateKeyExW(HKEY_CURRENT_USER, keyPath(entry), 0, nullptr, 0,
                              KEY_SET_VALUE, nullptr, &key, nullptr)
            : RegOpenKeyExW(HKEY_CURRENT_USER, keyPath(entry), 0, KEY_SET_VALUE, &key);
        if (!value.exists && (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND))
            return ERROR_SUCCESS;
        if (status != ERROR_SUCCESS) return status;
        if (value.exists) {
            status = RegSetValueExW(key, kValueName, 0, value.type, value.bytes.data(),
                                     static_cast<DWORD>(value.bytes.size()));
        } else {
            status = RegDeleteValueW(key, kValueName);
            if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
        }
        RegCloseKey(key);
        return status;
    }
private:
    static const wchar_t* keyPath(detail::Entry entry) {
        return entry == detail::Entry::Run ? kRunKey : kApprovalKey;
    }
};

detail::RegistryValue stringValue(const std::wstring& text) {
    detail::RegistryValue value{true, REG_SZ, {}};
    value.bytes.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.bytes.data(), text.c_str(), value.bytes.size());
    return value;
}

std::wstring registeredCommand(const detail::RegistryValue& value) {
    if (!value.exists || (value.type != REG_SZ && value.type != REG_EXPAND_SZ) ||
        value.bytes.empty() || value.bytes.size() % sizeof(wchar_t)) return {};
    std::wstring command(value.bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(command.data(), value.bytes.data(), value.bytes.size());
    if (command.back() != L'\0') return {};
    command.pop_back();
    if (command.find(L'\0') != std::wstring::npos) return {};
    if (value.type == REG_EXPAND_SZ) {
        const DWORD length = ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
        if (!length || length > kMaxCommandLength + 1) return {};
        std::wstring expanded(length, L'\0');
        if (ExpandEnvironmentStringsW(command.c_str(), expanded.data(), length) != length) return {};
        expanded.pop_back();
        command = std::move(expanded);
    }
    return command;
}

bool windowsAllowsStartup(detail::Registry& registry) {
    detail::RegistryValue approval;
    if (registry.read(detail::Entry::Approval, approval) != ERROR_SUCCESS) return false;
    if (!approval.exists) return true;
    return approval.type == REG_BINARY && !approval.bytes.empty() &&
           detail::approvalAllowsStartup(approval.bytes);
}

bool absoluteExecutable(const std::wstring& path) {
    // PathIsRelative alone also accepts root-relative paths.
    const bool drive = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path.size() > 2 && path[0] == L'\\' && path[1] == L'\\';
    return drive || unc;
}

std::wstring commandExecutable(const std::wstring& command) {
    if (command.empty() || command.size() > kMaxCommandLength ||
        command.find_first_of(L"\r\n") != std::wstring::npos ||
        command.find(L'\0') != std::wstring::npos) return {};
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command.c_str(), &argc);
    if (!argv) return {};
    std::wstring path;
    if (argc == 2 && std::wcscmp(argv[1], L"--tray") == 0 &&
        absoluteExecutable(argv[0])) path = argv[0];
    LocalFree(argv);
    return path;
}

bool writeRegistration(detail::Registry& registry, const std::wstring& path,
                       bool enable, bool resetApproval) {
    const auto command = enable ? detail::commandForExecutable(path) : std::wstring{};
    if (enable && command.empty()) {
        LOG_WARN("startup: executable path cannot be registered (empty, invalid, or too long)");
        return false;
    }
    detail::RegistryValue oldRun, approval;
    uint32_t status = registry.read(detail::Entry::Run, oldRun);
    if (status == ERROR_SUCCESS && enable && resetApproval)
        status = registry.read(detail::Entry::Approval, approval);
    if (status != ERROR_SUCCESS) {
        LOG_WARN("startup: cannot read existing registration (%lu)", static_cast<unsigned long>(status));
        return false;
    }
    status = registry.write(detail::Entry::Run, enable ? stringValue(command) : detail::RegistryValue{});
    if (status != ERROR_SUCCESS) {
        LOG_WARN("startup: registry write failed (%lu)", static_cast<unsigned long>(status));
        return false;
    }
    // Routine path repair preserves Task Manager's disabled state. Explicit
    // enable restores the original Run value if clearing approval fails.
    if (enable && resetApproval && approval.exists) {
        status = registry.write(detail::Entry::Approval, {});
        if (status != ERROR_SUCCESS) {
            const auto rollback = registry.write(detail::Entry::Run, oldRun);
            LOG_WARN("startup: cannot reset Windows approval (%lu); registration rollback=%lu",
                     static_cast<unsigned long>(status), static_cast<unsigned long>(rollback));
            return false;
        }
    }
    LOG_INFO("startup: %s", enable ? "registered current executable for sign-in" : "disabled");
    return true;
}

} // namespace

namespace detail {

std::wstring commandForExecutable(const std::wstring& path) {
    if (!absoluteExecutable(path) || path.find_first_of(L"\"\r\n") != std::wstring::npos ||
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
    if (data.empty()) return true;
    if (data.size() != 12 || data[1] || data[2] || data[3]) return false;
    return data[0] == 2 || data[0] == 6 || data[0] == 8;
}

bool isEnabled(Registry& registry, const std::wstring& path) {
    RegistryValue run;
    return windowsAllowsStartup(registry) && registry.read(Entry::Run, run) == ERROR_SUCCESS &&
           commandMatchesExecutable(registeredCommand(run), path);
}

bool setEnabled(Registry& registry, const std::wstring& path, bool enable) {
    return writeRegistration(registry, path, enable, true);
}

bool refreshRegistration(Registry& registry, const std::wstring& path) {
    if (!windowsAllowsStartup(registry)) return false;
    RegistryValue run;
    if (registry.read(Entry::Run, run) != ERROR_SUCCESS) return false;
    const auto command = registeredCommand(run);
    if (commandMatchesExecutable(command, path)) return true;
    const auto previous = commandExecutable(command);
    // Missing registration is an intentional opt-out. Custom launchers also
    // remain untouched until an explicit change in Settings.
    if (previous.empty() || _wcsicmp(PathFindFileNameW(previous.c_str()), L"audio-monitor.exe"))
        return false;
    return writeRegistration(registry, path, true, false);
}

bool applyPreference(Registry& registry, const std::wstring& path,
                     bool initial, bool desired, bool& enabled) {
    if (initial != desired && !setEnabled(registry, path, desired)) return false;
    enabled = isEnabled(registry, path);
    return true;
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
    WindowsRegistry registry;
    return detail::isEnabled(registry, executablePath());
}

bool setEnabled(bool enable) {
    WindowsRegistry registry;
    return detail::setEnabled(registry, executablePath(), enable);
}

bool refreshRegistration() {
    WindowsRegistry registry;
    return detail::refreshRegistration(registry, executablePath());
}

bool applyPreference(bool initial, bool desired, bool& enabled) {
    WindowsRegistry registry;
    return detail::applyPreference(registry, executablePath(), initial, desired, enabled);
}

} // namespace audiomon::startup
