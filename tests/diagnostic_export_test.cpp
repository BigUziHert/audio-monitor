#include "util/DiagnosticExport.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>

using namespace audiomon;

namespace {

std::wstring makeTempDirectory() {
    wchar_t root[MAX_PATH]{};
    wchar_t name[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, root) || !GetTempFileNameW(root, L"amd", 0, name))
        return {};
    if (!DeleteFileW(name) || !CreateDirectoryW(name, nullptr))
        return {};
    return name;
}

bool writeFile(const std::wstring& path, const std::string& contents) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const BOOL ok = contents.empty() ||
                    WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
                              &written, nullptr);
    const BOOL closed = CloseHandle(file);
    return ok && closed && written == contents.size();
}

bool readFile(const std::wstring& path, std::string& contents) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) >
            (std::numeric_limits<size_t>::max)()) {
        CloseHandle(file);
        return false;
    }
    contents.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD requested = static_cast<DWORD>(
            (std::min)(contents.size() - offset,
                       static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!ReadFile(file, contents.data() + offset, requested, &read, nullptr) || read == 0) {
            CloseHandle(file);
            return false;
        }
        offset += read;
    }
    const BOOL closed = CloseHandle(file);
    return closed && offset == contents.size();
}

bool isReportPath(const std::wstring& directory, const std::wstring& path) {
    std::wstring prefix = directory;
    if (!prefix.empty() && prefix.back() != L'\\')
        prefix.push_back(L'\\');
    constexpr wchar_t stem[] = L"audio-monitor-diagnostics-";
    return path.rfind(prefix + stem, 0) == 0 && path.size() > 4 &&
           path.compare(path.size() - 4, 4, L".txt") == 0;
}

} // namespace

int main() {
    int failed = 0;
    auto check = [&](bool condition, const char* label) {
        if (!condition) {
            std::printf("FAIL: %s\n", label);
            ++failed;
        }
    };

    const std::wstring base = makeTempDirectory();
    check(!base.empty(), "create temporary directory");
    if (base.empty())
        return 1;

    const std::wstring reports = base + L"\\reports";
    std::string report = "Audio Monitor diagnostics\nUTF-8: caf\xC3\xA9 \xF0\x9F\x8E\xA7\n";
    report.append("embedded\0byte", 13);

    const DiagnosticExportResult first = writeDiagnosticReport(reports, report);
    check(first.error.empty() && !first.path.empty(), "write first diagnostic report");
    check(isReportPath(reports, first.path), "timestamped report path is inside directory");
    DWORD reportAttributes = GetFileAttributesW(reports.c_str());
    check(reportAttributes != INVALID_FILE_ATTRIBUTES &&
              (reportAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
          "create missing report directory");
    std::string contents;
    check(readFile(first.path, contents) && contents == report, "preserve exact UTF-8 bytes");

    constexpr size_t kConcurrentReports = 8;
    std::array<DiagnosticExportResult, kConcurrentReports> concurrent{};
    std::array<std::thread, kConcurrentReports> workers;
    for (size_t i = 0; i < workers.size(); ++i)
        workers[i] = std::thread([&, i] { concurrent[i] = writeDiagnosticReport(reports, report); });
    for (auto& worker : workers)
        worker.join();
    for (size_t i = 0; i < concurrent.size(); ++i) {
        check(concurrent[i].error.empty() && isReportPath(reports, concurrent[i].path),
              "concurrent report succeeded with a valid path");
        std::string concurrentContents;
        check(readFile(concurrent[i].path, concurrentContents) && concurrentContents == report,
              "concurrent report preserved contents");
        for (size_t j = 0; j < i; ++j)
            check(concurrent[i].path != concurrent[j].path,
                  "concurrent reports use unique filenames");
        check(concurrent[i].path != first.path, "new report did not overwrite an existing file");
    }

    const DiagnosticExportResult empty = writeDiagnosticReport(reports, {});
    std::string emptyContents;
    check(empty.error.empty() && readFile(empty.path, emptyContents) && emptyContents.empty(),
          "write an exact empty report");

    const DiagnosticExportResult emptyDirectory = writeDiagnosticReport({}, report);
    check(emptyDirectory.path.empty() && !emptyDirectory.error.empty(),
          "reject an empty report directory");

    const DiagnosticExportResult missingParent =
        writeDiagnosticReport(base + L"\\missing-parent\\reports", report);
    check(missingParent.path.empty() && !missingParent.error.empty(),
          "report a missing parent directory");
    check(GetFileAttributesW((base + L"\\missing-parent").c_str()) == INVALID_FILE_ATTRIBUTES,
          "do not create an unexpected directory hierarchy");

    const std::wstring fileInsteadOfDirectory = base + L"\\not-a-directory";
    check(writeFile(fileInsteadOfDirectory, "keep"), "create directory-collision fixture");
    const DiagnosticExportResult invalidDirectory =
        writeDiagnosticReport(fileInsteadOfDirectory, report);
    check(invalidDirectory.path.empty() && !invalidDirectory.error.empty(),
          "reject a file used as the report directory");
    std::string markerContents;
    check(readFile(fileInsteadOfDirectory, markerContents) && markerContents == "keep",
          "do not modify a colliding path");

    if (!first.path.empty()) DeleteFileW(first.path.c_str());
    for (const auto& result : concurrent)
        if (!result.path.empty()) DeleteFileW(result.path.c_str());
    if (!empty.path.empty()) DeleteFileW(empty.path.c_str());
    DeleteFileW(fileInsteadOfDirectory.c_str());
    RemoveDirectoryW(reports.c_str());
    RemoveDirectoryW(base.c_str());

    if (!failed)
        std::printf("diagnostic export tests passed\n");
    return failed ? 1 : 0;
}
