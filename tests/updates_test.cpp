#include "util/Updates.h"
#include "config/Json.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace audiomon;
using namespace audiomon::updates;

namespace {

const std::string kOldCommit(40, 'a');
const std::string kNewCommit(40, 'b');

JsonValue releaseFixture(const std::string& channel = "dev") {
    auto release = JsonValue::object();
    release.set("tag_name", JsonValue(channel));
    release.set("name", JsonValue("Audio Monitor " + channel));
    release.set("draft", JsonValue(false));
    release.set("prerelease", JsonValue(channel == "dev"));
    release.set("target_commitish", JsonValue(kNewCommit));
    auto asset = JsonValue::object();
    asset.set("name", JsonValue("audio-monitor-win64.zip"));
    asset.set("state", JsonValue("uploaded"));
    asset.set("size", JsonValue(100000));
    asset.set("browser_download_url", JsonValue(
        "https://github.com/BigUziHert/audio-monitor/releases/download/" +
        channel + "/audio-monitor-win64.zip"));
    auto assets = JsonValue::array();
    assets.push(asset);
    release.set("assets", assets);
    return release;
}

void changeAsset(JsonValue& release, const char* field, JsonValue value) {
    auto asset = release.find("assets")->items().front();
    asset.set(field, std::move(value));
    auto assets = JsonValue::array();
    assets.push(asset);
    release.set("assets", assets);
}

struct FakeGitHub {
    UpdateHttpResponse release{200, releaseFixture().dump(), {}};
    UpdateHttpResponse compare{200, R"({"status":"ahead"})", {}};
    std::vector<std::string> requests;

    UpdateResult check(const BuildInfo& build = {"0.1.0", "dev", kOldCommit}) {
        return checkForUpdates(build, [this](const std::string& path) {
            requests.push_back(path);
            if (path.find("/releases/tags/") != std::string::npos)
                return release;
            if (path.find("/compare/") != std::string::npos)
                return compare;
            return UpdateHttpResponse{0, {}, "Unexpected request"};
        });
    }
};

} // namespace

int main() {
    int failed = 0;
    auto check = [&](bool condition, const char* label) {
        if (!condition) {
            std::printf("FAIL: %s\n", label);
            ++failed;
        }
    };

    FakeGitHub github;
    auto result = github.check();
    check(result.status == UpdateStatus::Available && result.commit == kNewCommit,
          "new commit is an update even when app version stays the same");
    check(result.downloadUrl ==
              "https://github.com/BigUziHert/audio-monitor/releases/download/dev/audio-monitor-win64.zip",
          "download comes from the expected repository and channel");
    check(github.requests.size() == 2 && github.requests.front() ==
              "/repos/BigUziHert/audio-monitor/releases/tags/dev" && github.requests.back() ==
              "/repos/BigUziHert/audio-monitor/compare/" + kOldCommit + "..." + kNewCommit +
              "?per_page=1&page=2", "checks release then compares exact full commits");

    github.requests.clear();
    check(github.check({"0.1.0", "dev", kNewCommit}).status == UpdateStatus::Current &&
              github.requests.size() == 1, "same commit is current without another network request");
    github.compare.body = R"({"status":"behind"})";
    result = github.check();
    check(result.status == UpdateStatus::Current && result.message.find("newer") != std::string::npos,
          "local build ahead of published release is never offered a downgrade");
    github.compare.body = R"({"status":"identical"})";
    check(github.check().status == UpdateStatus::Current, "identical comparison is current");
    github.compare.body = R"({"status":"diverged"})";
    result = github.check();
    check(result.status == UpdateStatus::Available && result.message.find("different") != std::string::npos,
          "diverged history is described as different instead of newer");
    github.compare.body = R"({"status":"unexpected"})";
    check(github.check().status == UpdateStatus::Error, "reject unknown comparison status");
    github.compare.body = R"({"status":true})";
    check(github.check().status == UpdateStatus::Error, "reject incorrectly typed comparison status");
    github.compare = {404, {}, {}};
    result = github.check();
    check(result.status == UpdateStatus::Available && result.message.find("cannot compare") != std::string::npos,
          "unknown local commit does not claim a newer release");
    github.requests.clear();
    result = github.check({"0.1.0", "dev", ""});
    check(result.status == UpdateStatus::Available && github.requests.size() == 1 &&
              result.message.find("no comparable") != std::string::npos,
          "missing local metadata offers published build with an honest comparison message");
    github.requests.clear();
    check(github.check({"0.1.0", "other", kOldCommit}).status == UpdateStatus::Error &&
              github.requests.empty(), "unknown channel makes no network request");

    FakeGitHub stable;
    stable.release.body = releaseFixture("main").dump();
    check(stable.check({"0.1.0", "main", kOldCommit}).status == UpdateStatus::Available &&
              stable.requests.front() == "/repos/BigUziHert/audio-monitor/releases/tags/main",
          "main build checks only stable main release");
    check(stable.check().status == UpdateStatus::Error, "dev build rejects main release");
    auto release = releaseFixture();
    release.set("prerelease", JsonValue(false));
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::Error, "dev channel requires prerelease marker");
    release = releaseFixture();
    release.set("draft", JsonValue(true));
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::Error, "draft releases are never offered");
    release = releaseFixture();
    release.set("draft", JsonValue(0));
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::Error, "boolean release metadata is strictly typed");
    release = releaseFixture();
    release.set("target_commitish", JsonValue("devchatgpt"));
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::Error, "branch name cannot identify released binary commit");

    for (const auto& url : {
             "https://github.com.evil.test/BigUziHert/audio-monitor/releases/download/dev/audio-monitor-win64.zip",
             "https://github.com/other/audio-monitor/releases/download/dev/audio-monitor-win64.zip",
             "http://github.com/BigUziHert/audio-monitor/releases/download/dev/audio-monitor-win64.zip",
             "https://github.com/BigUziHert/audio-monitor/releases/download/main/audio-monitor-win64.zip",
             "https://github.com/BigUziHert/audio-monitor/releases/download/dev/audio-monitor-win64.zip?other=1",
             "file:///C:/Windows/System32/cmd.exe"}) {
        release = releaseFixture();
        changeAsset(release, "browser_download_url", JsonValue(url));
        github.release.body = release.dump();
        check(github.check().status == UpdateStatus::Error, "reject unexpected asset URL");
    }
    release = releaseFixture();
    changeAsset(release, "state", JsonValue("new"));
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::Error, "incomplete upload is never offered");
    release = releaseFixture();
    changeAsset(release, "size", JsonValue(0));
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::Error, "empty download is never offered");
    release = releaseFixture();
    release.set("assets", JsonValue::array());
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::NoRelease, "missing download describes build still publishing");
    release.set("assets", JsonValue("invalid"));
    github.release.body = release.dump();
    check(github.check().status == UpdateStatus::Error, "reject malformed asset list");

    for (const auto& body : {"", "not json", "[]", "null", "{\"draft\":false"}) {
        github.release.body = body;
        check(github.check().status == UpdateStatus::Error, "malformed response is an error");
    }
    github.release.body.assign(2 * 1024 * 1024 + 1, ' ');
    check(github.check().status == UpdateStatus::Error, "oversized response is rejected before parsing");
    github.release = {404, {}, {}};
    check(github.check().status == UpdateStatus::NoRelease, "no release is not reported as up to date");
    for (unsigned status : {401u, 403u, 429u, 500u, 302u}) {
        github.release = {status, {}, {}};
        result = github.check();
        check(result.status == UpdateStatus::Error && !result.message.empty() && result.downloadUrl.empty(),
              "HTTP failure has actionable message and no download");
    }
    github.release = {200, releaseFixture().dump(), "Network failed"};
    check(github.check().status == UpdateStatus::Error, "transport failure takes priority over status and body");
    github.release = {200, releaseFixture().dump(), {}};
    github.compare = {429, {}, {}};
    check(github.check().status == UpdateStatus::Error, "failed comparison does not assume newer build");
    result = checkForUpdates({"0.1.0", "dev", kOldCommit}, [](const std::string&) -> UpdateHttpResponse {
        throw std::runtime_error("transport failure");
    });
    check(result.status == UpdateStatus::Error, "worker receives a result rather than a transport exception");

    result.status = UpdateStatus::Available;
    result.downloadUrl = "file:///C:/Windows/System32/cmd.exe";
    std::string error;
    check(!openUpdateDownload(result, error) && !error.empty(), "browser opener independently rejects unsafe URL");
    result.status = UpdateStatus::Current;
    result.downloadUrl = "https://github.com/BigUziHert/audio-monitor/releases/download/dev/audio-monitor-win64.zip";
    check(!openUpdateDownload(result, error), "browser opener requires an available update");

    std::printf("updates: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
    return failed ? 1 : 0;
}
