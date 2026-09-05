#include "config/Config.h"
#include "config/Json.h"
#include "util/Text.h"
#include "util/Log.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <vector>

namespace audiomon {
namespace {

constexpr int kConfigVersion = 2;

JsonValue channelToJson(const ChannelConfig& c) {
    JsonValue v = JsonValue::object();
    v.set("label", JsonValue(c.label));
    v.set("kind", JsonValue(c.kind == SourceKind::Application ? "application" :
                            c.kind == SourceKind::Microphone ? "microphone" : "playback"));
    v.set("enabled", JsonValue(c.enabled));
    v.set("processPath", JsonValue(toUtf8(c.processPath)));
    v.set("deviceId",   JsonValue(toUtf8(c.deviceId)));
    v.set("deviceName", JsonValue(toUtf8(c.deviceNameMatch)));
    if (!c.icon.empty()) v.set("icon", JsonValue(c.icon));
    v.set("gain",       JsonValue(static_cast<double>(c.gain)));
    v.set("muted",      JsonValue(c.muted));
    return v;
}

ChannelConfig channelFromJson(const JsonValue* v, const ChannelConfig& fallback) {
    if (!v || !v->isObject()) return fallback;
    ChannelConfig c = fallback;
    if (auto x = v->find("label")) c.label = x->asString(c.label);
    if (auto x = v->find("icon")) c.icon = x->asString(c.icon);
    if (auto x = v->find("enabled")) c.enabled = x->asBool(c.enabled);
    if (auto x = v->find("processPath")) c.processPath = toWide(x->asString(""));
    if (auto x = v->find("kind")) {
        auto kind = x->asString("playback");
        c.kind = kind == "application" ? SourceKind::Application :
                 kind == "microphone" ? SourceKind::Microphone : SourceKind::Playback;
    }
    const JsonValue* id   = v->find("deviceId");
    const JsonValue* name = v->find("deviceName");
    const JsonValue* gain = v->find("gain");
    const JsonValue* mute = v->find("muted");

    c.deviceId        = id   ? toWide(id->asString("")) : fallback.deviceId;
    c.deviceNameMatch = name ? toWide(name->asString(toUtf8(fallback.deviceNameMatch)))
                             : fallback.deviceNameMatch;
    c.gain  = gain ? static_cast<float>(gain->asNumber(fallback.gain)) : fallback.gain;
    c.muted = mute ? mute->asBool(fallback.muted) : fallback.muted;

    if (!(c.gain >= 0.0f)) c.gain = 0.0f;      // also catches NaN
    if (c.gain > 4.0f)     c.gain = 4.0f;
    return c;
}

void logWin32Failure(const char* operation, DWORD error) {
    LOG_WARN("config: %s failed (Win32 error %lu)", operation,
             static_cast<unsigned long>(error));
}

DWORD closeReadHandle(HANDLE h) {
    if (CloseHandle(h)) return ERROR_SUCCESS;
    const DWORD error = GetLastError();
    logWin32Failure("CloseHandle(existing file)", error);
    return error;
}

bool readWholeFile(const std::wstring& path, std::string& out, DWORD* error = nullptr) {
    if (error) *error = ERROR_SUCCESS;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        const DWORD sizeError = GetLastError();
        closeReadHandle(h);
        if (error) *error = sizeError;
        return false;
    }
    if (size.QuadPart < 0 || size.QuadPart > (4 << 20)) {
        closeReadHandle(h);
        if (error) *error = ERROR_FILE_TOO_LARGE;
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out.empty() ||
                    ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    const DWORD readError = ok ? ERROR_SUCCESS : GetLastError();
    const DWORD closeError = closeReadHandle(h);
    if (!ok) {
        if (error) *error = readError;
        return false;
    }
    if (closeError != ERROR_SUCCESS) {
        if (error) *error = closeError;
        return false;
    }
    out.resize(read);
    return true;
}

void removeTempFile(const std::wstring& path) {
    if (!DeleteFileW(path.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND)
            logWin32Failure("DeleteFileW(temp)", error);
    }
}

} // namespace

Config Config::defaults() {
    Config c;
    // First-run autodetection, chosen to be UNAMBIGUOUS on a real machine
    // rather than merely plausible.
    //
    // "Elgato" alone is actively dangerous: with Wave Link installed it also
    // matches "System (Elgato Virtual Audio)" and the mic mirror, and routing
    // the mix into the virtual driver is exactly what this application exists
    // to avoid. "Elgato 4K" matches only the capture card.
    //
    // The mic is matched on the USB Audio Class name it actually enumerates
    // with; "Yeti" matched nothing, because the device reports itself
    // generically rather than by product name.
    //
    // Anything ambiguous is refused outright at resolve time and reported, so
    // a wrong guess here shows up as an error rather than as audio quietly
    // going somewhere else.
    ChannelConfig game, chat, mic;
    game.label = "Headphones";
    chat.label = "Chat Audio";
    mic.label = "Microphone";
    mic.kind = SourceKind::Microphone;
    game.deviceNameMatch = L"Arctis Pro Wireless Game";
    chat.deviceNameMatch = L"Arctis Pro Wireless Chat";
    mic.deviceNameMatch = L"USB Advanced Audio Device";
    c.sources = {game, chat, mic};
    c.output.deviceNameMatch = L"Elgato 4K";
    return c;
}

std::wstring Config::appDataDir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::wstring dir(raw);
    CoTaskMemFree(raw);
    dir += L"\\audio-monitor";
    CreateDirectoryW(dir.c_str(), nullptr);   // ERROR_ALREADY_EXISTS is fine
    return dir;
}

std::wstring Config::configPath() {
    const std::wstring dir = appDataDir();
    return dir.empty() ? std::wstring() : dir + L"\\config.json";
}

Config Config::load(bool* usedDefaults) {
    return load(configPath(), usedDefaults);
}

Config Config::load(const std::wstring& path, bool* usedDefaults) {
    Config def = defaults();
    if (usedDefaults) *usedDefaults = true;

    if (path.empty()) return def;

    std::string text;
    if (!readWholeFile(path, text)) return def;

    std::string err;
    const JsonValue root = JsonValue::parse(text, &err);
    if (!err.empty() || !root.isObject()) {
        LOG_WARN("config: %s -- using defaults", err.empty() ? "not an object" : err.c_str());
        return def;
    }

    if (usedDefaults) *usedDefaults = false;
    return fromJson(root);
}

Config Config::fromJson(const JsonValue& root) {
    const Config def = defaults();
    Config c = def;
    if (const auto* sources = root.find("sources"); sources && sources->isArray()) {
        c.sources.clear();
        for (const auto& source : sources->items()) {
            if (!source.isObject()) continue;
            c.sources.push_back(channelFromJson(&source, ChannelConfig{}));
            if (c.sources.size() == kMaxSources) break;
        }
    } else {
        // Migrate the original three-source configuration without losing pins or gains.
        c.sources[0] = channelFromJson(root.find("game"), def.sources[0]);
        c.sources[1] = channelFromJson(root.find("chat"), def.sources[1]);
        c.sources[2] = channelFromJson(root.find("mic"), def.sources[2]);
    }
    c.output = channelFromJson(root.find("output"), def.output);
    if (auto v = root.find("mono")) c.mono = v->asBool(false);
    if (auto v = root.find("closeToTray")) c.closeToTray = v->asBool(false);

    if (const JsonValue* v = root.find("exclusiveOutput"))  c.exclusiveOutput  = v->asBool(def.exclusiveOutput);
    if (const JsonValue* v = root.find("startWithWindows")) c.startWithWindows = v->asBool(def.startWithWindows);
    if (const JsonValue* v = root.find("startMinimized"))   c.startMinimized   = v->asBool(def.startMinimized);
    if (const JsonValue* v = root.find("bufferMillis")) {
        const double m = v->asNumber(def.bufferMillis);
        c.bufferMillis = static_cast<uint32_t>(m < 20 ? 20 : (m > 250 ? 250 : m));
    }

    return c;
}

JsonValue Config::toJson() const {
    JsonValue root = JsonValue::object();
    root.set("version",          JsonValue(kConfigVersion));
    JsonValue sourcesJson = JsonValue::array();
    for (const auto& source : sources) sourcesJson.push(channelToJson(source));
    root.set("sources", sourcesJson);
    root.set("mono", JsonValue(mono));
    root.set("closeToTray", JsonValue(closeToTray));
    root.set("output",           channelToJson(output));
    root.set("exclusiveOutput",  JsonValue(exclusiveOutput));
    root.set("startWithWindows", JsonValue(startWithWindows));
    root.set("startMinimized",   JsonValue(startMinimized));
    root.set("bufferMillis",     JsonValue(static_cast<double>(bufferMillis)));

    return root;
}

bool Config::save() const {
    return save(configPath());
}

bool Config::save(const std::wstring& path) const {
    if (path.empty()) return false;
    const std::string text = toJson().dump(2);

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        // Preserve an invalid file for diagnosis before replacing it. Keep the
        // original v1 backup as well so an older build can still be restored.
        std::string previous;
        DWORD readError = ERROR_SUCCESS;
        if (!readWholeFile(path, previous, &readError)) {
            logWin32Failure("reading existing file", readError);
            return false;
        }
        std::string parseError;
        const auto old = JsonValue::parse(previous, &parseError);
        if (!parseError.empty() || !old.isObject()) {
            if (!CopyFileW(path.c_str(), (path + L".corrupt.bak").c_str(), TRUE)) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_EXISTS) {
                    logWin32Failure("CopyFileW(corrupt backup)", error);
                    return false;
                }
            }
        } else if (!old.find("sources")) {
            if (!CopyFileW(path.c_str(), (path + L".v1.bak").c_str(), TRUE)) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_EXISTS) {
                    logWin32Failure("CopyFileW(version 1 backup)", error);
                    return false;
                }
            }
        }
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            logWin32Failure("GetFileAttributesW", error);
            return false;
        }
    }

    // Write to a sibling temp file, then swap it in. A crash part-way through
    // leaves the previous config intact rather than a truncated one.
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        logWin32Failure("CreateFileW(temp)", GetLastError());
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    const DWORD writeError = ok ? ERROR_SUCCESS : GetLastError();
    const BOOL flushed = FlushFileBuffers(h);
    const DWORD flushError = flushed ? ERROR_SUCCESS : GetLastError();
    const BOOL closed = CloseHandle(h);
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
    if (!ok) logWin32Failure("WriteFile", writeError);
    if (ok && written != text.size())
        LOG_WARN("config: WriteFile wrote only %lu of %zu bytes",
                 static_cast<unsigned long>(written), text.size());
    if (!flushed) logWin32Failure("FlushFileBuffers", flushError);
    if (!closed) logWin32Failure("CloseHandle(temp)", closeError);
    if (!ok || written != text.size() || !flushed || !closed) {
        removeTempFile(tmp);
        return false;
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        logWin32Failure("MoveFileExW", GetLastError());
        removeTempFile(tmp);
        return false;
    }
    return true;
}

} // namespace audiomon
