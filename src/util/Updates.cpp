#include "util/Updates.h"

#include "config/Json.h"
#include "util/Text.h"

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <utility>

#ifndef AUDIOMON_VERSION
#define AUDIOMON_VERSION "unknown"
#endif
#ifndef AUDIOMON_BUILD_CHANNEL
#define AUDIOMON_BUILD_CHANNEL "dev"
#endif
#ifndef AUDIOMON_BUILD_COMMIT
#define AUDIOMON_BUILD_COMMIT ""
#endif

namespace audiomon::updates {
namespace {

constexpr size_t kMaximumResponseBytes = 2 * 1024 * 1024;
constexpr char kApiRoot[] = "/repos/BigUziHert/audio-monitor/";
constexpr char kDownloadRoot[] = "https://github.com/BigUziHert/audio-monitor/releases/download/";
constexpr char kAssetName[] = "audio-monitor-win64.zip";

bool validChannel(const std::string& channel) {
    return channel == "dev" || channel == "main";
}

bool validCommit(const std::string& commit) {
    return commit.size() == 40 &&
           std::all_of(commit.begin(), commit.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::string downloadUrl(const std::string& channel) {
    return std::string(kDownloadRoot) + channel + "/" + kAssetName;
}

bool validDownloadUrl(const std::string& url) {
    return url == downloadUrl("dev") || url == downloadUrl("main");
}

std::string stringField(const JsonValue& object, const char* name) {
    const auto* value = object.find(name);
    return value ? value->asString({}) : std::string{};
}

bool boolField(const JsonValue& object, const char* name, bool& value) {
    const auto* field = object.find(name);
    if (!field || field->type() != JsonValue::Type::Bool)
        return false;
    value = field->asBool(false);
    return true;
}

UpdateResult failure(std::string message) {
    UpdateResult result;
    result.message = std::move(message);
    return result;
}

std::string httpError(const UpdateHttpResponse& response) {
    if (!response.error.empty())
        return response.error;
    if (response.status == 403 || response.status == 429)
        return "GitHub limited or denied update checks. Try again later.";
    if (response.status == 401)
        return "GitHub denied access to the public update feed.";
    if (response.status >= 500)
        return "GitHub is temporarily unavailable. Try again later.";
    return "GitHub returned HTTP " + std::to_string(response.status) +
           ". Try checking for updates again later.";
}

bool parseResponse(const UpdateHttpResponse& response, JsonValue& value) {
    if (response.body.empty() || response.body.size() > kMaximumResponseBytes)
        return false;
    std::string error;
    value = JsonValue::parse(response.body, &error);
    return error.empty() && value.isObject();
}

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET value) : value_(value) {}
    ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
private:
    HINTERNET value_;
};

UpdateHttpResponse networkError() {
    const DWORD code = GetLastError();
    std::string message;
    if (code == ERROR_WINHTTP_TIMEOUT)
        message = "The update check timed out. Check your connection and try again.";
    else if (code == ERROR_WINHTTP_SECURE_FAILURE)
        message = "A secure connection to GitHub could not be verified. Check your PC's date and network.";
    else
        message = "Could not connect to GitHub. Check your connection and try again (Windows error " +
                  std::to_string(code) + ").";
    return {0, {}, std::move(message)};
}

UpdateHttpResponse getFromGitHub(const std::string& path) {
    if (path.rfind(kApiRoot, 0) != 0 || path.find_first_of("\r\n") != std::string::npos)
        return {0, {}, "Invalid update request path."};

    InternetHandle session(WinHttpOpen(L"AudioMonitor-UpdateCheck/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        return networkError();
    if (!WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000))
        return networkError();

    InternetHandle connection(WinHttpConnect(session.get(), L"api.github.com",
                                              INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection)
        return networkError();
    const std::wstring widePath = toWide(path);
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", widePath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request)
        return networkError();
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirectPolicy, sizeof(redirectPolicy)))
        return networkError();
    constexpr wchar_t headers[] = L"Accept: application/vnd.github+json\r\n"
                                   L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1L),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr))
        return networkError();

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX))
        return networkError();
    UpdateHttpResponse response{static_cast<unsigned>(status), {}, {}};
    if (status != 200)
        return response;

    std::array<char, 16384> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline)
            return {0, {}, "The update response took too long. Try again later."};
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead))
            return networkError();
        if (bytesRead == 0)
            return response;
        if (bytesRead > kMaximumResponseBytes - response.body.size())
            return {0, {}, "GitHub returned an unexpectedly large update response."};
        response.body.append(buffer.data(), bytesRead);
    }
}

UpdateResult checkImpl(const BuildInfo& build, const UpdateHttpGet& get) {
    if (!validChannel(build.channel))
        return failure("Unknown update channel. Download a published dev or main build.");
    const auto response = get(std::string(kApiRoot) + "releases/tags/" + build.channel);
    if (response.error.empty() && response.status == 404) {
        UpdateResult result;
        result.status = UpdateStatus::NoRelease;
        result.message = "No " + build.channel +
                         " download yet. Try again after the GitHub build finishes.";
        return result;
    }
    if (!response.error.empty() || response.status != 200)
        return failure(httpError(response));

    JsonValue release;
    if (!parseResponse(response, release))
        return failure("GitHub returned invalid release information. Try again later.");
    bool draft = true;
    bool prerelease = false;
    if (!boolField(release, "draft", draft) || !boolField(release, "prerelease", prerelease) ||
        draft || prerelease != (build.channel == "dev") ||
        stringField(release, "tag_name") != build.channel)
        return failure("The release does not match this build's update channel.");
    const std::string releasedCommit = stringField(release, "target_commitish");
    if (!validCommit(releasedCommit))
        return failure("Release commit missing. Try again after publishing completes.");

    const auto* assets = release.find("assets");
    if (!assets || !assets->isArray())
        return failure("GitHub returned invalid release downloads.");
    bool hasDownload = false;
    for (const auto& asset : assets->items()) {
        if (stringField(asset, "name") != kAssetName)
            continue;
        if (stringField(asset, "state") != "uploaded" ||
            stringField(asset, "browser_download_url") != downloadUrl(build.channel))
            return failure("The release download is incomplete or has an unexpected address.");
        const auto* size = asset.find("size");
        if (!size || size->type() != JsonValue::Type::Number || size->asNumber(0) <= 0)
            return failure("The release download is empty or incomplete.");
        hasDownload = true;
        break;
    }
    if (!hasDownload) {
        UpdateResult result;
        result.status = UpdateStatus::NoRelease;
        result.message = "No Windows " + build.channel +
                         " download yet. Try again after the GitHub build finishes.";
        return result;
    }

    UpdateResult result;
    result.releaseName = stringField(release, "name");
    if (result.releaseName.empty())
        result.releaseName = build.channel + " " + releasedCommit.substr(0, 7);
    // Release names are remote display text, never executable instructions.
    if (result.releaseName.size() > 256)
        result.releaseName.resize(256);
    result.commit = releasedCommit;
    result.downloadUrl = downloadUrl(build.channel);
    if (build.commit == releasedCommit) {
        result.status = UpdateStatus::Current;
        result.message = "You have the latest published " + build.channel + " build.";
        return result;
    }
    if (!validCommit(build.commit)) {
        result.status = UpdateStatus::Available;
        result.message = "Published " + build.channel +
                         " build available; no comparable local commit information.";
        return result;
    }

    // Use ancestry, not the app's version number: several builds can legitimately
    // share one version. Page 2 excludes the first page's potentially large patch
    // list; GitHub still returns the aggregate ahead/behind comparison status.
    const auto comparison = get(std::string(kApiRoot) + "compare/" + build.commit +
                                "..." + releasedCommit + "?per_page=1&page=2");
    if (comparison.error.empty() && comparison.status == 404) {
        result.status = UpdateStatus::Available;
        result.message = "Published " + build.channel +
                         " build available; GitHub cannot compare this local commit.";
        return result;
    }
    if (!comparison.error.empty() || comparison.status != 200)
        return failure(httpError(comparison));
    JsonValue compared;
    if (!parseResponse(comparison, compared))
        return failure("GitHub returned invalid build comparison information. Try again later.");
    const std::string status = stringField(compared, "status");
    if (status == "ahead") {
        result.status = UpdateStatus::Available;
        result.message = "A newer " + build.channel + " build is ready to download.";
    } else if (status == "identical" || status == "behind") {
        result.status = UpdateStatus::Current;
        result.message = status == "identical"
            ? "You have the latest published " + build.channel + " build."
            : "Your build is newer than the latest published " + build.channel + " download.";
    } else if (status == "diverged") {
        result.status = UpdateStatus::Available;
        result.message = "A different " + build.channel +
                         " build is available; your build has separate changes.";
    } else {
        return failure("GitHub returned an unknown build comparison. Try again later.");
    }
    return result;
}

} // namespace

BuildInfo currentBuildInfo() {
    return {AUDIOMON_VERSION, AUDIOMON_BUILD_CHANNEL, AUDIOMON_BUILD_COMMIT};
}

UpdateResult checkForUpdates() {
    return checkForUpdates(currentBuildInfo(), getFromGitHub);
}

UpdateResult checkForUpdates(const BuildInfo& build, const UpdateHttpGet& get) {
    try {
        return checkImpl(build, get);
    } catch (const std::exception&) {
        return failure("The update check could not complete. Try again later.");
    } catch (...) {
        return failure("The update check could not complete. Try again later.");
    }
}

bool openUpdateDownload(const UpdateResult& update, std::string& error) {
    error.clear();
    if (update.status != UpdateStatus::Available || !validDownloadUrl(update.downloadUrl)) {
        error = "No verified update download is available. Check for updates again.";
        return false;
    }
    const auto url = toWide(update.downloadUrl);
    const auto opened = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (opened <= 32) {
        error = "Could not open the download in your browser (Windows error " +
                std::to_string(opened) + ").";
        return false;
    }
    return true;
}

} // namespace audiomon::updates
