#include "util/UpdateInstaller.h"

#include "util/Text.h"
#include "UpdateScript.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace audiomon::updates {
namespace {

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

std::wstring fullPath(const std::wstring& path) {
    if (path.empty() || path.find(L'\0') != std::wstring::npos) return {};
    std::array<wchar_t, 32768> buffer{};
    const DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!size || size >= buffer.size()) return {};
    std::wstring result(buffer.data(), size);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

bool noReparsePoints(const std::wstring& path) {
    auto cursor = std::filesystem::path(path);
    for (;;) {
        const DWORD attributes = GetFileAttributesW(cursor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = cursor.parent_path();
        if (parent.empty() || parent == cursor) return true;
        cursor = parent;
    }
}

bool validStage(const std::wstring& directory, std::wstring& normalized) {
    std::array<wchar_t, 32768> temporary{};
    const DWORD count = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
    if (!count || count >= temporary.size()) return false;
    const auto base = fullPath(temporary.data());
    normalized = fullPath(directory);
    if (base.empty() || normalized.empty()) return false;
    const std::filesystem::path stage(normalized);
    if (_wcsicmp(stage.parent_path().c_str(), base.c_str()) != 0) return false;
    const auto name = stage.filename().wstring();
    constexpr std::wstring_view prefix = L"AudioMonitorUpdate-";
    if (name.size() != prefix.size() + 32 || name.compare(0, prefix.size(), prefix) != 0 ||
        !std::all_of(name.begin() + prefix.size(), name.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
        })) return false;
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && noReparsePoints(normalized);
}

std::wstring child(const std::wstring& directory, const wchar_t* name) {
    return (std::filesystem::path(directory) / name).wstring();
}

bool regularFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

std::string readSmallFile(const std::wstring& path, size_t maximum) {
    if (!regularFile(path)) return {};
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    LARGE_INTEGER size{};
    if (!file.valid() || !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
        static_cast<uint64_t>(size.QuadPart) > maximum) return {};
    std::string content(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &read, nullptr) ||
        read != content.size()) return {};
    return content;
}

bool writeHelper(const std::wstring& stage) {
    const auto path = child(stage, L"Update.ps1");
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid()) return false;
    constexpr DWORD size = static_cast<DWORD>(sizeof(detail::kUpdateInstallerScript) - 1);
    DWORD written = 0;
    return WriteFile(file.get(), detail::kUpdateInstallerScript, size, &written, nullptr) && written == size;
}

std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}

std::wstring powershellPath() {
    std::array<wchar_t, MAX_PATH> system{};
    const UINT size = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (!size || size >= system.size()) return {};
    return child(system.data(), L"WindowsPowerShell\\v1.0\\powershell.exe");
}

bool startHelper(const std::wstring& stage, const std::wstring& arguments,
                 PROCESS_INFORMATION& process, std::string& error) {
    const auto powershell = powershellPath();
    if (powershell.empty() || !regularFile(powershell)) {
        error = "Windows PowerShell is unavailable. The app has not been changed.";
        return false;
    }
    std::wstring command = quote(powershell) + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
        quote(child(stage, L"Update.ps1")) + L" -Directory " + quote(stage) + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, stage.c_str(), &startup, &process)) {
        error = "Could not start the update helper (Windows error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    return true;
}

void helperFailure(const std::wstring& stage, const char* fallback, std::string& error) {
    error = readSmallFile(child(stage, L"error.txt"), 8192);
    if (error.empty()) error = fallback;
}

std::wstring executablePath() {
    std::array<wchar_t, 32768> path{};
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return size && size < path.size() ? std::wstring(path.data(), size) : std::wstring{};
}

std::wstring eventToken() {
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) return {};
    constexpr wchar_t hex[] = L"0123456789abcdef";
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    std::wstring token;
    for (size_t index = 0; index < sizeof(value); ++index) {
        token += hex[bytes[index] >> 4];
        token += hex[bytes[index] & 15];
    }
    return token;
}

bool knownFile(const std::wstring& name, bool payload) {
    if (payload) {
        return name == L"audio-monitor.exe" || name == L"audiomon-cli.exe" || name == L"LICENSE" ||
               name == L"Install.ps1" || name == L"Install.cmd";
    }
    return name == L"update.zip" || name == L"Update.ps1" || name == L"prepared.json" ||
           name == L"update.log" || name == L"error.txt";
}

bool listCleanupFiles(const std::wstring& directory, bool payload, std::vector<std::wstring>& files,
                      bool& hasPayload) {
    std::error_code failure;
    const auto entries = std::filesystem::directory_iterator(directory, failure);
    if (failure) return false;
    for (const auto& entry : entries) {
        const auto path = entry.path().wstring();
        const auto name = entry.path().filename().wstring();
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (!payload && name == L"payload" && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            hasPayload = true;
            if (!listCleanupFiles(path, true, files, hasPayload)) return false;
        } else {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) || !knownFile(name, payload)) return false;
            files.push_back(path);
        }
    }
    return true;
}

} // namespace

bool prepareUpdatePackage(const std::wstring& directory, std::string& error) {
    error.clear();
    try {
        std::wstring stage;
        if (!validStage(directory, stage) || !regularFile(child(stage, L"update.zip"))) {
            error = "The private update download is missing or unsafe.";
            return false;
        }
        if (!writeHelper(stage)) {
            error = "Could not stage the trusted update helper.";
            return false;
        }
        PROCESS_INFORMATION process{};
        if (!startHelper(stage, L" -Mode Prepare", process, error)) return false;
        Handle processHandle(process.hProcess), threadHandle(process.hThread);
        if (WaitForSingleObject(processHandle.get(), 60000) != WAIT_OBJECT_0) {
            // This is our own extraction-only helper; no app/installation work
            // has begun, so aborting it cannot interrupt the running mixer.
            TerminateProcess(processHandle.get(), 1);
            WaitForSingleObject(processHandle.get(), 5000);
            error = "Preparing the update timed out. Download it again and retry.";
            return false;
        }
        DWORD code = 1;
        if (!GetExitCodeProcess(processHandle.get(), &code) || code != 0 ||
            !regularFile(child(stage, L"prepared.json"))) {
            helperFailure(stage, "The update package could not be prepared.", error);
            return false;
        }
        return true;
    } catch (...) {
        error = "The update package could not be prepared. The app has not been changed.";
        return false;
    }
}

bool launchUpdateInstaller(const PreparedUpdate& update, std::string& error) {
    error.clear();
    try {
        std::wstring stage;
        if (!validStage(update.directory, stage) ||
            readSmallFile(child(stage, L"Update.ps1"), sizeof(detail::kUpdateInstallerScript)) != detail::kUpdateInstallerScript) {
            error = "The trusted update helper is missing or changed. Download the update again.";
            return false;
        }
        const auto original = executablePath();
        FILETIME created{}, exited{}, kernel{}, user{};
        if (original.empty() || !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            error = "Could not identify the running app for a safe restart.";
            return false;
        }
        const uint64_t creation = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const auto token = eventToken();
        if (token.empty()) { error = "Could not create an update handoff."; return false; }
        const auto readyName = L"Local\\AudioMonitorUpdateReady-" + token;
        const auto cancelName = L"Local\\AudioMonitorUpdateCancel-" + token;
        Handle ready(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
        if (!ready.valid() || GetLastError() == ERROR_ALREADY_EXISTS) { error = "Could not create an update handoff."; return false; }
        Handle cancel(CreateEventW(nullptr, TRUE, FALSE, cancelName.c_str()));
        if (!cancel.valid() || GetLastError() == ERROR_ALREADY_EXISTS) { error = "Could not create an update handoff."; return false; }
        // Do not mistake an earlier failed preparation message for this launch.
        const auto previousError = child(stage, L"error.txt");
        if (regularFile(previousError)) DeleteFileW(previousError.c_str());
        const auto arguments = L" -Mode Apply -ParentProcessId " + std::to_wstring(GetCurrentProcessId()) +
            L" -ParentStartedFileTime " + std::to_wstring(creation) + L" -OriginalExecutable " + quote(original) +
            L" -ReadyEvent " + quote(readyName) + L" -CancelEvent " + quote(cancelName);
        PROCESS_INFORMATION process{};
        if (!startHelper(stage, arguments, process, error)) return false;
        Handle processHandle(process.hProcess), threadHandle(process.hThread);
        const HANDLE handles[] = {ready.get(), processHandle.get()};
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 30000);
        DWORD code = 1;
        if (wait == WAIT_OBJECT_0 && GetExitCodeProcess(processHandle.get(), &code) && code == STILL_ACTIVE)
            return true;
        // A failed handshake must never make a later unrelated app exit trigger
        // an installation. Cancel, then stop only the helper we just created.
        SetEvent(cancel.get());
        if (WaitForSingleObject(processHandle.get(), 2000) != WAIT_OBJECT_0) {
            TerminateProcess(processHandle.get(), 1);
            WaitForSingleObject(processHandle.get(), 5000);
        }
        helperFailure(stage, "The update helper did not become ready. The app is still running; please retry.", error);
        return false;
    } catch (...) {
        error = "The update installer could not start. The app has not been changed.";
        return false;
    }
}

void discardPreparedUpdate(const PreparedUpdate& update) {
    try {
        std::wstring stage;
        if (!validStage(update.directory, stage)) return;
        std::vector<std::wstring> files;
        bool hasPayload = false;
        if (!listCleanupFiles(stage, false, files, hasPayload)) return;
        // No recursive delete: remove only the checked flat allowlist, then the
        // two directories if empty. Unknown files and links are left untouched.
        for (const auto& file : files) {
            if (!regularFile(file) || !DeleteFileW(file.c_str())) return;
        }
        if (hasPayload && !RemoveDirectoryW(child(stage, L"payload").c_str())) return;
        RemoveDirectoryW(stage.c_str());
    } catch (...) { }
}

} // namespace audiomon::updates
