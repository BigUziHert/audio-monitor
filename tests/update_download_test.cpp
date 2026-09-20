#include "util/UpdateDownload.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace audiomon::updates;

namespace {

UpdateResult fixture() {
    UpdateResult result;
    result.status = UpdateStatus::Available;
    result.channel = "dev";
    result.commit = std::string(40, 'a');
    result.assetId = 123456;
    result.downloadSize = 3;
    result.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    // The download must ignore this moving channel URL and use the asset ID.
    result.downloadUrl = "https://github.com/BigUziHert/audio-monitor/releases/download/dev/audio-monitor-win64.zip";
    return result;
}

bool send(const detail::DownloadSink& sink, const std::string& text) {
    return sink({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--live-download-smoke") {
        auto build = currentBuildInfo();
        build.commit.clear();
        const auto update = checkForUpdates(build);
        if (update.status != UpdateStatus::Available) {
            std::printf("live-download: release check failed: %s\n", update.message.c_str());
            return 1;
        }
        DownloadProgress progress;
        auto result = downloadUpdate(update, progress);
        if (result.directory.empty()) {
            std::printf("live-download: failed: %s\n", result.error.c_str());
            return 1;
        }
        const auto directory = result.directory;
        discardPreparedUpdate(result);
        if (std::filesystem::exists(directory)) {
            std::printf("live-download: verified/extracted, but staging cleanup failed\n");
            return 1;
        }
        std::printf("live-download: verified and extracted asset %llu, %llu bytes, commit %s; staging removed; nothing installed\n",
            static_cast<unsigned long long>(update.assetId),
            static_cast<unsigned long long>(progress.received.load()), update.commit.c_str());
        return 0;
    }
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("FAIL: %s\n", label); ++failed; }
    };
    unsigned prepared = 0;
    const detail::PreparePackage prepare = [&](const std::wstring& directory, std::string&) {
        ++prepared;
        std::ifstream file(std::filesystem::path(directory) / "update.zip", std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        return contents == "abc";
    };
    const std::string api = "https://api.github.com/repos/BigUziHert/audio-monitor/releases/assets/123456";
    const std::string cdn = "https://release-assets.githubusercontent.com/github-production-release-asset/123/abc?token=test";
    {
        DownloadProgress progress;
        std::vector<std::string> requests;
        auto result = detail::downloadUpdate(fixture(), progress,
            [&](const std::string& url, const detail::DownloadSink& sink, DownloadProgress&) {
                requests.push_back(url);
                if (url == api) return detail::DownloadResponse{302, cdn, {}};
                check(send(sink, "a") && send(sink, "bc"), "valid chunks are accepted");
                return detail::DownloadResponse{200, {}, {}};
            }, prepare);
        check(!result.directory.empty() && result.error.empty() && progress.received == 3 &&
                  progress.total == 3 && prepared == 1 && requests == std::vector<std::string>{api, cdn},
              "download uses immutable asset, validates streamed size/hash, then prepares locally");
        const auto staged = result.directory;
        discardPreparedUpdate(result);
        check(staged.empty() || !std::filesystem::exists(staged), "private completed staging can be discarded");
    }
    auto expectFailure = [&](const UpdateResult& update, const detail::DownloadGet& get,
                             const char* label) {
        DownloadProgress progress;
        const auto before = prepared;
        auto result = detail::downloadUpdate(update, progress, get, prepare);
        check(result.directory.empty() && !result.error.empty() && prepared == before, label);
    };
    expectFailure(fixture(), [](const std::string&, const detail::DownloadSink& sink, DownloadProgress&) {
        send(sink, "ab");
        return detail::DownloadResponse{0, {}, "Connection interrupted"};
    }, "interrupted download never reaches extraction or installation");
    expectFailure(fixture(), [](const std::string&, const detail::DownloadSink& sink, DownloadProgress&) {
        send(sink, "ab");
        return detail::DownloadResponse{200, {}, {}};
    }, "early EOF fails the exact expected size check");
    expectFailure(fixture(), [](const std::string&, const detail::DownloadSink& sink, DownloadProgress&) {
        send(sink, "abcd");
        return detail::DownloadResponse{200, {}, {}};
    }, "oversized response is rejected while streaming");
    expectFailure(fixture(), [](const std::string&, const detail::DownloadSink& sink, DownloadProgress&) {
        send(sink, "xyz");
        return detail::DownloadResponse{200, {}, {}};
    }, "same-size replacement fails SHA-256 verification");
    expectFailure(fixture(), [](const std::string&, const detail::DownloadSink&, DownloadProgress& progress) {
        progress.cancel = true;
        return detail::DownloadResponse{200, {}, {}};
    }, "cancellation cannot leave an installable partial update");
    expectFailure(fixture(), [&](const std::string& url, const detail::DownloadSink&, DownloadProgress&) {
        check(url == api, "release replacement still requests the checked asset id");
        return detail::DownloadResponse{404, {}, {}};
    }, "deleted asset during release replacement fails instead of following a moving channel URL");

    for (const auto& location : {
            "http://release-assets.githubusercontent.com/github-production-release-asset/123/x",
            "https://release-assets.githubusercontent.com.evil.test/github-production-release-asset/123/x",
            "https://evil.test/github-production-release-asset/123/x",
            "https://release-assets.githubusercontent.com@evil.test/github-production-release-asset/123/x",
            "https://release-assets.githubusercontent.com:444/github-production-release-asset/123/x",
            "https://api.github.com/repos/other/repo/releases/assets/1",
            "https://github.com/BigUziHert/audio-monitor/releases/download/dev/audio-monitor-win64.zip",
            "https://release-assets.githubusercontent.com/other/path",
            "https://release-assets.githubusercontent.com/github-production-release-asset/123/x#fragment",
            "https://release-assets.githubusercontent.com\\@evil.test/github-production-release-asset/123/x"}) {
        unsigned requests = 0;
        expectFailure(fixture(), [&](const std::string&, const detail::DownloadSink&, DownloadProgress&) {
            ++requests;
            return detail::DownloadResponse{302, location, {}};
        }, "untrusted redirects never supply an update");
        check(requests == 1, "untrusted redirect is rejected before a second request");
    }
    unsigned redirects = 0;
    expectFailure(fixture(), [&](const std::string&, const detail::DownloadSink&, DownloadProgress&) {
        ++redirects;
        return detail::DownloadResponse{302, cdn, {}};
    }, "redirect loops terminate without preparing an update");
    check(redirects == 5, "redirect count is bounded");
    expectFailure(fixture(), [](const std::string&, const detail::DownloadSink&, DownloadProgress&)
            -> detail::DownloadResponse { throw std::runtime_error("Transport failed"); },
        "transport exceptions become a recoverable download error");
    {
        DownloadProgress progress;
        std::wstring directory;
        auto result = detail::downloadUpdate(fixture(), progress,
            [](const std::string&, const detail::DownloadSink& sink, DownloadProgress&) {
                send(sink, "abc");
                return detail::DownloadResponse{200, {}, {}};
            }, [&](const std::wstring& stage, std::string& error) {
                directory = stage;
                error = "Invalid archive";
                return false;
            });
        check(result.directory.empty() && result.error == "Invalid archive" &&
                  !directory.empty() && !std::filesystem::exists(directory),
              "preparation failure removes the complete private staging directory");
    }
    for (const unsigned invalid : {0u, 1u, 2u, 3u, 4u}) {
        auto update = fixture();
        if (invalid == 0) update.sha256.clear();
        if (invalid == 1) update.assetId = 0;
        if (invalid == 2) update.downloadSize = kMaximumUpdateBytes + 1;
        if (invalid == 3) update.channel = "other";
        if (invalid == 4) update.status = UpdateStatus::Current;
        unsigned requests = 0;
        expectFailure(update, [&](const std::string&, const detail::DownloadSink&, DownloadProgress&) {
            ++requests;
            return detail::DownloadResponse{};
        }, "invalid or unavailable metadata cannot start a download");
        check(requests == 0, "metadata is validated before network activity");
    }
    std::printf("update-download: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
    return failed ? 1 : 0;
}
