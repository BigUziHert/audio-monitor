#pragma once

#include <string>

namespace audiomon {

struct DiagnosticExportResult {
    std::wstring path;
    std::string error;
};

// Writes the supplied UTF-8 bytes to a newly-created report file. The final
// directory is created when its parent already exists; existing reports are
// never replaced. On failure, error is non-empty and path is empty.
DiagnosticExportResult writeDiagnosticReport(const std::wstring& directory,
                                              const std::string& report);

} // namespace audiomon
