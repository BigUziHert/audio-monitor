#include "util/DiagnosticExport.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace audiomon {
namespace {

constexpr unsigned kMaximumNameAttempts = 10000;

std::string windowsError(const char* action, DWORD error) {
    char message[512]{};
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, 0, message,
                                        static_cast<DWORD>(sizeof(message)), nullptr);
    size_t end = length;
    while (end && (message[end - 1] == '\r' || message[end - 1] == '\n' ||
                   message[end - 1] == ' ' || message[end - 1] == '.'))
        --end;

    char prefix[192]{};
    std::snprintf(prefix, sizeof(prefix), "%s failed (Windows error %lu)", action,
                  static_cast<unsigned long>(error));
    std::string result(prefix);
    if (end) {
        result += ": ";
        result.append(message, end);
    }
    return result;
}

bool ensureDirectory(const std::wstring& directory, DWORD& error) noexcept {
    if (directory.empty()) {
        error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            return false;
    }

    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error = ERROR_DIRECTORY;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

std::wstring joinPath(const std::wstring& directory, const wchar_t* name) {
    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path.push_back(L'\\');
    path += name;
    return path;
}

DiagnosticExportResult createdFileFailure(HANDLE file, const std::wstring& path,
                                          const char* action, DWORD primaryError) {
    DWORD closeError = ERROR_SUCCESS;
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file))
        closeError = GetLastError();

    DWORD cleanupError = ERROR_SUCCESS;
    if (!DeleteFileW(path.c_str()))
        cleanupError = GetLastError();

    DiagnosticExportResult result;
    result.error = windowsError(action, primaryError);
    if (closeError != ERROR_SUCCESS)
        result.error += "; " + windowsError("closing the incomplete report", closeError);
    if (cleanupError != ERROR_SUCCESS && cleanupError != ERROR_FILE_NOT_FOUND)
        result.error += "; " + windowsError("removing the incomplete report", cleanupError);
    return result;
}

} // namespace

DiagnosticExportResult writeDiagnosticReport(const std::wstring& directory,
                                              const std::string& report) {
    try {
        DWORD directoryError = ERROR_SUCCESS;
        if (!ensureDirectory(directory, directoryError))
            return {{}, windowsError("creating the report directory", directoryError)};

        SYSTEMTIME time{};
        GetLocalTime(&time);

        HANDLE file = INVALID_HANDLE_VALUE;
        std::wstring path;
        for (unsigned attempt = 0; attempt < kMaximumNameAttempts; ++attempt) {
            wchar_t name[128]{};
            const int nameLength = std::swprintf(
                name, std::size(name),
                L"audio-monitor-diagnostics-%04u%02u%02u-%02u%02u%02u-%03u-%04u.txt",
                static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
                static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
                static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
                static_cast<unsigned>(time.wMilliseconds), attempt);
            if (nameLength <= 0)
                return {{}, "Could not format a diagnostic report filename"};
            path = joinPath(directory, name);
            file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
                break;

            const DWORD createError = GetLastError();
            if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS)
                return {{}, windowsError("creating the diagnostic report", createError)};
        }
        if (file == INVALID_HANDLE_VALUE)
            return {{}, "Could not create a unique diagnostic report filename"};

        size_t offset = 0;
        while (offset < report.size()) {
            const size_t remaining = report.size() - offset;
            const DWORD requested = static_cast<DWORD>(std::min<size_t>(
                remaining,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (!WriteFile(file, report.data() + offset, requested, &written, nullptr)) {
                const DWORD writeError = GetLastError();
                return createdFileFailure(file, path, "writing the diagnostic report", writeError);
            }
            if (written == 0)
                return createdFileFailure(file, path, "writing the diagnostic report",
                                          ERROR_WRITE_FAULT);
            offset += written;
        }

        if (!FlushFileBuffers(file)) {
            const DWORD flushError = GetLastError();
            return createdFileFailure(file, path, "flushing the diagnostic report", flushError);
        }
        if (!CloseHandle(file)) {
            const DWORD closeError = GetLastError();
            return createdFileFailure(INVALID_HANDLE_VALUE, path,
                                      "closing the diagnostic report", closeError);
        }

        return {std::move(path), {}};
    } catch (const std::exception& exception) {
        try {
            return {{}, std::string("Could not write the diagnostic report: ") + exception.what()};
        } catch (...) {
            return {{}, "Could not write the diagnostic report"};
        }
    } catch (...) {
        return {{}, "Could not write the diagnostic report"};
    }
}

} // namespace audiomon
