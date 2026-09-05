#include "util/Startup.h"
#include "util/Log.h"

#include <windows.h>

namespace audiomon::startup {
namespace {

constexpr wchar_t kRunKey[]    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"AudioMonitor";

// The stored command is quoted: an unquoted path with a space in it is both a
// launch failure and a classic privilege-escalation footgun.
std::wstring quotedCommand() {
    return L"\"" + executablePath() + L"\" --tray";
}

} // namespace

std::wstring executablePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return buf; }
        buf.resize(buf.size() * 2);            // path longer than MAX_PATH
    }
}

bool isEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, size = 0;
    const LSTATUS st = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool setEnabled(bool enable) {
    HKEY key = nullptr;
    const LSTATUS createStatus =
        RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (createStatus != ERROR_SUCCESS) {
        LOG_WARN("startup: cannot open Run key (%ld)", createStatus);
        return false;
    }

    LSTATUS st;
    if (enable) {
        const std::wstring cmd = quotedCommand();
        st = RegSetValueExW(key, kValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kValueName);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;   // already absent
    }
    RegCloseKey(key);

    if (st != ERROR_SUCCESS) { LOG_WARN("startup: registry write failed (%ld)", st); return false; }
    return true;
}

} // namespace audiomon::startup
