#include "config/Config.h"
#include "config/Json.h"
#include <windows.h>
#include <cstdio>
#include <string>
using namespace audiomon;

namespace {

std::wstring makeTempDirectory() {
    wchar_t root[MAX_PATH]{};
    wchar_t name[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, root) || !GetTempFileNameW(root, L"amt", 0, name)) return {};
    if (!DeleteFileW(name) || !CreateDirectoryW(name, nullptr)) return {};
    return name;
}

bool writeFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == text.size();
}

std::string readFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (4 << 20)) {
        CloseHandle(h);
        return {};
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = text.empty() || ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return {};
    text.resize(read);
    return text;
}

void removeTempDirectory(const std::wstring& dir) {
    for (const wchar_t* suffix : {L"\\config.json", L"\\config.json.tmp",
                                  L"\\config.json.v1.bak", L"\\config.json.corrupt.bak"}) {
        const std::wstring path = dir + suffix;
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
    }
    RemoveDirectoryW(dir.c_str());
}

} // namespace

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char *label) {
        if (!ok) {
            std::printf("FAIL: %s\n", label);
            ++failed;
        }
    };
    for (const char* text : {
             R"({"gain":1e})", R"({"gain":.})", R"({"gain":+1})", R"({"gain":01})",
             R"({"gain":1.})", R"({"gain":1e+})", R"({"gain":1-2})", R"({"gain":1.2.3})",
             R"({"gain":1e9999})", R"({"label":"\uZZZZ"})", R"({"label":"\u12x4"})",
             R"({"label":"\uD800"})", R"({"label":"\uDC00"})", R"({"label":"\uD800\u0041"})",
             "{\"label\":\"raw\nnewline\"}"}) {
        std::string error;
        const auto invalid = JsonValue::parse(text, &error);
        check(invalid.isNull() && !error.empty(), text);
    }
    std::string parseError;
    const auto valid = JsonValue::parse(
        R"({"negative":-0.125,"exponent":2.5E+1,"label":"\u0041\uD83D\uDD0A"})", &parseError);
    check(parseError.empty() && valid.find("negative") &&
              valid.find("negative")->asNumber(0) == -0.125 && valid.find("exponent") &&
              valid.find("exponent")->asNumber(0) == 25,
          "valid fractions and exponents parse");
    check(valid.find("label") && valid.find("label")->asString("") == "A\xF0\x9F\x94\x8A",
          "Unicode escapes and surrogate pairs decode to UTF-8");
    const auto bom = JsonValue::parse(std::string("\xEF\xBB\xBF") +
                                          R"({"bufferMillis":90})",
                                      &parseError);
    check(parseError.empty() && bom.find("bufferMillis") &&
              bom.find("bufferMillis")->asNumber(0) == 90,
          "UTF-8 BOM is accepted");
    auto legacy = JsonValue::parse(
        R"({"game":{"deviceId":"game-pin","gain":0.8,"muted":true},"chat":{"gain":0.7},"mic":{"gain":2.1},"output":{"muted":true},"bufferMillis":90})");
    auto c = Config::fromJson(legacy);
    check(c.sources.size() == 3, "legacy keeps three sources");
    check(c.sources[0].deviceId == L"game-pin" && c.sources[0].muted && c.sources[0].gain == 0.8f,
          "legacy preserves pin, gain, mute");
    check(c.sources[2].kind == SourceKind::Microphone && c.sources[2].gain == 2.1f,
          "legacy microphone retains boost");
    check(c.output.muted && c.bufferMillis == 90, "legacy master mute and buffer");
    ChannelConfig app;
    app.kind = SourceKind::Application;
    app.label = "Discord";
    app.icon = "chat";
    app.processPath = L"C:\\Apps\\Discord.exe";
    app.enabled = false;
    c.sources.push_back(app);
    c.mono = true;
    c.closeToTray = true;
    c.output.label = "Stream Output";
    c.output.icon = "screen";
    c.output.gain = 1.75f;
    auto copy = Config::fromJson(JsonValue::parse(c.toJson().dump()));
    check(copy.sources.size() == 4 && copy.sources[3].processPath == app.processPath &&
              copy.sources[3].icon == "chat" &&
              !copy.sources[3].enabled,
          "app identity, icon, and enabled state persist");
    check(copy.mono && copy.closeToTray && copy.output.muted, "mix and window preferences persist");
    check(copy.output.label == "Stream Output" && copy.output.icon == "screen" &&
              copy.output.gain == 1.75f,
          "output name, icon, and gain persist");
    c.sources.clear();
    copy = Config::fromJson(c.toJson());
    check(copy.sources.empty(), "empty source list remains empty");
    auto malformed =
        Config::fromJson(JsonValue::parse(R"({"sources":[{},false,{"gain":999}],"bufferMillis":999})"));
    check(malformed.sources.size() == 2 && malformed.sources[1].gain == 4 && malformed.bufferMillis == 250,
          "invalid entries skipped and gains clamped");
    auto many = JsonValue::object(), list = JsonValue::array();
    for (int i = 0; i < 50; ++i)
        list.push(JsonValue::object());
    many.set("sources", list);
    check(Config::fromJson(many).sources.size() == kMaxSources, "source count bounded");

    const std::wstring tempDir = makeTempDirectory();
    check(!tempDir.empty(), "temporary config directory created");
    if (!tempDir.empty()) {
        const std::wstring path = tempDir + L"\\config.json";
        const std::wstring tmp = path + L".tmp";
        Config saved = Config::defaults();
        saved.bufferMillis = 90;
        saved.mono = true;
        saved.output.label = "Temporary output";
        check(saved.save(path), "save to explicit path");
        bool usedDefaults = true;
        const Config loaded = Config::load(path, &usedDefaults);
        check(!usedDefaults && loaded.bufferMillis == 90 && loaded.mono &&
                  loaded.output.label == "Temporary output",
              "load from explicit path");
        check(GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES,
              "successful save leaves no temp file");

        const std::string legacyText =
            R"({"game":{"gain":0.8},"chat":{},"mic":{},"bufferMillis":90})";
        check(writeFile(path, legacyText), "write version 1 fixture");
        check(saved.save(path), "save migrates version 1 config");
        check(readFile(path + L".v1.bak") == legacyText, "version 1 backup preserved");

        const std::string corruptText = R"({"bufferMillis":)";
        check(writeFile(path, corruptText), "write corrupt fixture");
        check(saved.save(path), "save replaces corrupt config");
        check(readFile(path + L".corrupt.bak") == corruptText,
              "corrupt config backed up before replacement");

        const std::string laterCorruptText = R"({"later":)";
        check(writeFile(path, laterCorruptText), "write later corrupt fixture");
        check(saved.save(path), "save replaces a later corrupt config");
        check(readFile(path + L".corrupt.bak") == corruptText,
              "first corrupt backup is preserved");

        check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
              "make config read-only");
        check(!saved.save(path), "save to read-only config fails");
        check(GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES,
              "failed save cleans up temp file");
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

        const std::wstring corruptBackup = path + L".corrupt.bak";
        SetFileAttributesW(corruptBackup.c_str(), FILE_ATTRIBUTE_NORMAL);
        check(DeleteFileW(corruptBackup.c_str()) != FALSE,
              "remove corrupt backup before read-only retry fixture");
        const std::string readOnlyCorruptText = R"({"readOnly":)";
        check(writeFile(path, readOnlyCorruptText), "write read-only corrupt fixture");
        check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
              "make corrupt config read-only");
        check(!saved.save(path), "first save of read-only corrupt config fails");
        check(readFile(corruptBackup) == readOnlyCorruptText,
              "read-only corrupt config is backed up");
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        check(saved.save(path), "save retries after corrupt config becomes writable");
        check(readFile(corruptBackup) == readOnlyCorruptText,
              "retry preserves the first corrupt backup");
        removeTempDirectory(tempDir);
    }
    std::printf("Configuration tests: %d failures\n", failed);
    return failed ? 1 : 0;
}
