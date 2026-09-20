#pragma once

#include <string>

namespace audiomon::updates {

struct PreparedUpdate {
    std::wstring directory;
    std::string error;
};

// Runs off the UI thread. Extracts only the expected release payload into the
// private download directory, without modifying the installed application.
bool prepareUpdatePackage(const std::wstring& directory, std::string& error);

// Runs off the UI thread. Returns true only once the independent installer is
// ready to wait for this process to exit. The caller must then quit normally.
bool launchUpdateInstaller(const PreparedUpdate& update, std::string& error);

// Best-effort removal of this updater's private staging directory.
void discardPreparedUpdate(const PreparedUpdate& update);

} // namespace audiomon::updates
