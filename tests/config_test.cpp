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
    check(c.sources[0].deviceId == L"game-pin" && c.sources[0].muted && c.sources[0].gain == 0.8f &&
              c.sources[0].volume == 1.0f,
          "legacy preserves pin, gain, mute and defaults volume");
    check(c.sources[2].kind == SourceKind::Microphone && c.sources[2].gain == 2.1f,
          "legacy microphone retains boost");
    check(c.output.muted && c.bufferMillis == 90, "legacy master mute and buffer");
    const auto previous = Config::fromJson(JsonValue::parse(
        R"({"version":2,"sources":[{"gain":4}],"output":{"gain":2}})"));
    check(previous.sources.size() == 1 && previous.sources[0].gain == 4.f &&
              previous.sources[0].volume == 1.f && previous.output.gain == 2.f &&
              previous.output.volume == 1.f && previous.colorTheme == ColorTheme::Dark,
          "version 2 mix gains load with full dashboard volume and dark theme");
    check(Config::fromJson(JsonValue::parse(R"({})")).colorTheme == ColorTheme::Dark,
          "missing theme remains backward-compatible dark");
    check(Config::fromJson(JsonValue::parse(R"({"colorTheme":"sepia"})")).colorTheme ==
              ColorTheme::Dark,
          "unknown theme remains backward-compatible dark");
    check(Config::fromJson(JsonValue::parse(R"({"colorTheme":"light"})")).colorTheme ==
              ColorTheme::Light,
          "light theme string loads");
    const auto defaultJson = Config::defaults().toJson();
    check(defaultJson.find("version") && defaultJson.find("version")->asNumber(0) == 6 &&
              defaultJson.find("colorTheme") &&
              defaultJson.find("colorTheme")->asString("") == "dark",
          "version 6 writes the default dark theme string");
    check(Config{}.outputCount() == 1, "bare Config preserves the legacy directly assigned output slot");
    check(Config::defaults().outputCount() == 0 &&
              Config::defaults().output.deviceId.empty() &&
              Config::defaults().output.deviceNameMatch.empty() &&
              defaultJson.find("outputs") && defaultJson.find("outputs")->items().empty() &&
              !defaultJson.find("output"),
          "first-run defaults contain no guessed output or legacy hardware mirror");
    const Config emptyOutputs = Config::fromJson(JsonValue::parse(
        R"({"outputs":[],"output":{"deviceId":"obsolete-output","deviceName":"Old capture card"}})"));
    check(emptyOutputs.outputCount() == 0 && !emptyOutputs.hasOutput &&
              emptyOutputs.output.deviceId.empty() && emptyOutputs.additionalOutputs.empty() &&
              Config::fromJson(emptyOutputs.toJson()).outputCount() == 0 &&
              !emptyOutputs.toJson().find("output"),
          "an explicit empty outputs array overrides the legacy mirror and stays empty on round-trip");
    bool rejectedEmptyLookup = false;
    try { (void)emptyOutputs.outputAt(0); }
    catch (const std::out_of_range&) { rejectedEmptyLookup = true; }
    check(rejectedEmptyLookup, "empty output lookup cannot expose a hidden stale primary");

    const auto legacySingleOutput = Config::fromJson(JsonValue::parse(
        R"({"version":4,"output":{"label":"Legacy HDMI","deviceId":"legacy-id","deviceName":"Legacy Device","icon":"screen","gain":1.75,"volume":0.42,"muted":true}})"));
    check(legacySingleOutput.outputCount() == 1 &&
              legacySingleOutput.output.label == "Legacy HDMI" &&
              legacySingleOutput.output.deviceId == L"legacy-id" &&
              legacySingleOutput.output.deviceNameMatch == L"Legacy Device" &&
              legacySingleOutput.output.icon == "screen" &&
              legacySingleOutput.output.gain == 1.75f &&
              legacySingleOutput.output.volume == 0.42f && legacySingleOutput.output.muted,
          "version 4 single output migrates losslessly");

    Config multipleOutputs = Config::defaults();
    ChannelConfig primaryOutput;
    primaryOutput.label = "Primary";
    primaryOutput.deviceId = L"primary-id";
    primaryOutput.deviceNameMatch = L"Primary Device";
    primaryOutput.gain = 1.25f;
    primaryOutput.volume = 0.8f;
    check(multipleOutputs.addOutput(primaryOutput), "adding the first output installs the primary");
    ChannelConfig secondaryOutput;
    secondaryOutput.label = "Headphones";
    secondaryOutput.deviceId = L"secondary-id";
    secondaryOutput.deviceNameMatch = L"Secondary Device";
    secondaryOutput.icon = "headphones";
    secondaryOutput.gain = 0.75f;
    secondaryOutput.volume = 0.55f;
    secondaryOutput.muted = true;
    ChannelConfig tertiaryOutput;
    tertiaryOutput.label = "Recorder";
    tertiaryOutput.deviceId = L"tertiary-id";
    tertiaryOutput.deviceNameMatch = L"Tertiary Device";
    tertiaryOutput.icon = "wave";
    check(multipleOutputs.addOutput(secondaryOutput) && multipleOutputs.addOutput(tertiaryOutput),
          "adding extra outputs preserves ordering");

    const JsonValue multipleJson = multipleOutputs.toJson();
    const Config multipleCopy = Config::fromJson(JsonValue::parse(multipleJson.dump()));
    check(multipleCopy.outputCount() == 3 && multipleCopy.outputAt(0).label == "Primary" &&
              multipleCopy.outputAt(0).deviceId == L"primary-id" &&
              multipleCopy.outputAt(1).label == "Headphones" &&
              multipleCopy.outputAt(1).deviceId == L"secondary-id" &&
              multipleCopy.outputAt(1).icon == "headphones" &&
              multipleCopy.outputAt(1).gain == 0.75f &&
              multipleCopy.outputAt(1).volume == 0.55f && multipleCopy.outputAt(1).muted &&
              multipleCopy.outputAt(2).label == "Recorder" &&
              multipleCopy.outputAt(2).deviceId == L"tertiary-id",
          "multiple outputs round-trip in order with independent settings");

    const JsonValue* legacyOutputMirror = multipleJson.find("output");
    const JsonValue* outputArray = multipleJson.find("outputs");
    check(legacyOutputMirror && outputArray && outputArray->isArray() &&
              outputArray->items().size() == 3 &&
              legacyOutputMirror->dump() == outputArray->items()[0].dump(),
          "legacy output mirrors the first item in outputs");

    const auto sanitizedOutputs = Config::fromJson(JsonValue::parse(
        R"({"outputs":[false,{"label":"One","deviceId":"one","deviceName":"First"},{"label":"Duplicate ID","deviceId":"one","deviceName":"Other"},{"label":"Fallback","deviceName":"Fallback Name"},{"label":"Duplicate fallback","deviceName":"Fallback Name"},{},{"label":"Three","deviceId":"three"},{"label":"Four","deviceId":"four"},{"label":"Beyond cap","deviceId":"five"}]})"));
    check(sanitizedOutputs.outputCount() == kMaxOutputs &&
              sanitizedOutputs.outputAt(0).label == "One" &&
              sanitizedOutputs.outputAt(1).label == "Fallback" &&
              sanitizedOutputs.outputAt(2).label == "Three" &&
              sanitizedOutputs.outputAt(3).label == "Four",
          "malformed, empty, and duplicate outputs are skipped and output count is capped");

    const auto malformedOutputsFallback = Config::fromJson(JsonValue::parse(
        R"({"output":{"label":"Legacy fallback","deviceId":"legacy-fallback"},"outputs":[false,null,"bad"]})"));
    check(malformedOutputsFallback.outputCount() == 0,
          "an authoritative array containing only invalid entries cannot restore a legacy endpoint");
    const auto invalidArrayLegacy = Config::fromJson(JsonValue::parse(
        R"({"output":{"deviceId":"legacy-fallback"},"outputs":false})"));
    check(invalidArrayLegacy.outputCount() == 1 && invalidArrayLegacy.output.deviceId == L"legacy-fallback",
          "a malformed non-array outputs field still permits singular legacy migration");
    {
        Config editable = multipleOutputs;
        check(!editable.addOutput(primaryOutput) && !editable.addOutput(ChannelConfig{}),
              "output helpers reject duplicates and empty selections");
        check(editable.removeOutput(0) && editable.outputCount() == 2 &&
                  editable.outputAt(0).deviceId == secondaryOutput.deviceId &&
                  editable.outputAt(0).muted && editable.outputAt(0).volume == 0.55f,
              "deleting the primary promotes the next output without losing its controls");
        check(editable.removeOutput(1) && editable.outputCount() == 1 &&
                  editable.removeOutput(0) && editable.outputCount() == 0 &&
                  !editable.hasOutput && editable.output.deviceId.empty() &&
                  editable.output.deviceNameMatch.empty() && editable.additionalOutputs.empty(),
              "deleting the final output clears all stored destination state");
        check(!editable.removeOutput(0) && !editable.removeOutput(900),
              "deleting from an empty output list is a safe no-op");
        check(editable.addOutput(primaryOutput) && editable.outputCount() == 1 &&
                  editable.outputAt(0).deviceId == primaryOutput.deviceId,
              "adding after deleting all outputs creates one fresh primary");
        editable.clearOutputs();
        editable.output.deviceId = L"stale-cached-field";
        check(editable.outputCount() == 0 && !editable.toJson().find("output") &&
                  Config::fromJson(editable.toJson()).outputCount() == 0,
              "cached legacy fields cannot resurrect an explicitly cleared list");
        for (int i = 0; i < kMaxOutputs; ++i) {
            ChannelConfig output;
            output.deviceId = L"target-" + std::to_wstring(i);
            check(editable.addOutput(output), "add helper accepts each distinct available output slot");
        }
        check(!editable.addOutput(primaryOutput) && editable.outputCount() == kMaxOutputs,
              "add helper enforces the output limit");
    }
    ChannelConfig app;
    app.kind = SourceKind::Application;
    app.label = "Discord";
    app.icon = "chat";
    app.processPath = L"C:\\Apps\\Discord.exe";
    app.enabled = false;
    app.gain = 4.0f;
    app.volume = 0.65f;
    c.sources.push_back(app);
    c.mono = true;
    c.closeToTray = true;
    c.colorTheme = ColorTheme::System;
    c.output.label = "Stream Output";
    c.output.icon = "screen";
    c.output.gain = 1.75f;
    c.output.volume = 0.35f;
    auto copy = Config::fromJson(JsonValue::parse(c.toJson().dump()));
    check(copy.sources.size() == 4 && copy.sources[3].processPath == app.processPath &&
              copy.sources[3].icon == "chat" &&
              !copy.sources[3].enabled && copy.sources[3].gain == 4.0f &&
              copy.sources[3].volume == 0.65f,
          "app identity, icon, enabled state, mix gain, and volume persist");
    check(copy.mono && copy.closeToTray && copy.output.muted &&
              copy.colorTheme == ColorTheme::System,
          "mix, window, and theme preferences persist");
    check(copy.output.label == "Stream Output" && copy.output.icon == "screen" &&
              copy.output.gain == 1.75f && copy.output.volume == 0.35f,
          "output name, icon, mix gain, and volume persist");
    c.sources.clear();
    copy = Config::fromJson(c.toJson());
    check(copy.sources.empty(), "empty source list remains empty");
    auto malformed =
        Config::fromJson(JsonValue::parse(
            R"({"sources":[{},false,{"gain":999,"volume":999}],"bufferMillis":999})"));
    check(malformed.sources.size() == 2 && malformed.sources[1].gain == 4 &&
              malformed.sources[1].volume == 1 && malformed.bufferMillis == 250,
          "invalid entries skipped and gains and volumes clamped");
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
        saved.colorTheme = ColorTheme::Light;
        ChannelConfig savedOutput;
        savedOutput.label = "Temporary output";
        savedOutput.deviceId = L"temporary-output";
        check(saved.addOutput(savedOutput), "create explicit output for save fixture");
        check(saved.save(path), "save to explicit path");
        bool usedDefaults = true;
        const Config loaded = Config::load(path, &usedDefaults);
        check(!usedDefaults && loaded.bufferMillis == 90 && loaded.mono &&
                  loaded.colorTheme == ColorTheme::Light &&
                  loaded.output.label == "Temporary output",
              "load from explicit path");
        check(GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES,
              "successful save leaves no temp file");
        Config cleared = saved;
        cleared.clearOutputs();
        check(cleared.save(path), "save an explicitly empty output list");
        const Config reopenedEmpty = Config::load(path, &usedDefaults);
        check(!usedDefaults && reopenedEmpty.outputCount() == 0 &&
                  reopenedEmpty.output.deviceId.empty() && reopenedEmpty.bufferMillis == 90,
              "reopening a saved empty output list preserves zero outputs and other preferences");
        cleared.sources.clear();
        check(cleared.save(path), "save a reset setup with no devices or outputs");
        const Config reopenedReset = Config::load(path, &usedDefaults);
        check(!usedDefaults && reopenedReset.sources.empty() && reopenedReset.outputCount() == 0,
              "reopening a reset setup does not recreate preset devices or outputs");

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
