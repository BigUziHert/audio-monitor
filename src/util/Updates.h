#pragma once

#include <functional>
#include <string>

namespace audiomon::updates {

struct BuildInfo {
    std::string version;
    std::string channel; // "dev" or "main"
    std::string commit;  // full Git commit SHA; empty for unidentified local builds
};

enum class UpdateStatus { Available, Current, NoRelease, Error };

struct UpdateResult {
    UpdateStatus status = UpdateStatus::Error;
    std::string message;
    std::string releaseName;
    std::string commit;
    std::string downloadUrl;
};

BuildInfo currentBuildInfo();

// Synchronous, bounded network operation. Call on a worker, never the audio/UI
// thread. Checks only the channel embedded in this executable; no background
// downloads, credentials, installation, or executable replacement.
UpdateResult checkForUpdates();

// Opens a validated official ZIP download in the user's default browser. Call
// only after the user explicitly presses the download button.
bool openUpdateDownload(const UpdateResult& update, std::string& error);

// Injectable transport keeps release/channel/commit handling testable offline.
// Paths are relative to https://api.github.com, not arbitrary URLs.
struct UpdateHttpResponse {
    unsigned status = 0;
    std::string body;
    std::string error;
};
using UpdateHttpGet = std::function<UpdateHttpResponse(const std::string& path)>;
UpdateResult checkForUpdates(const BuildInfo& build, const UpdateHttpGet& get);

} // namespace audiomon::updates
