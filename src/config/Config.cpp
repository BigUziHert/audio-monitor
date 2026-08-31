#include "config/Config.h"
#include "config/Json.h"
#include "util/Log.h"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <vector>

namespace audiomon {
namespace {

constexpr int kConfigVersion = 1;

JsonValue channelToJson(const ChannelConfig& c) {
    JsonValue v = JsonValue::object();
    v.set("deviceId",   JsonValue(toUtf8(c.deviceId)));
    v.set("deviceName", JsonValue(toUtf8(c.deviceNameMatch)));
    v.set("gain",       JsonValue(static_cast<double>(c.gain)));
    v.set("muted",      JsonValue(c.muted));
    return v;
}

ChannelConfig channelFromJson(const JsonValue* v, const ChannelConfig& fallback) {
    if (!v || !v->isObject()) return fallback;
    ChannelConfig c;
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

bool readWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (4 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);
    return true;
}

} // namespace

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

Config Config::defaults() {
    Config c;
    // First-run autodetection. The Arctis presents two render endpoints whose
    // friendly names contain "Game" and "Chat"; the capture card's contains
    // "Elgato". The mic is matched on the product name.
    c.game.deviceNameMatch   = L"Game";
    c.chat.deviceNameMatch   = L"Chat";
    c.mic.deviceNameMatch    = L"Yeti";
    c.output.deviceNameMatch = L"Elgato";

    c.game.gain = c.chat.gain = c.mic.gain = c.output.gain = 1.0f;
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
    Config def = defaults();
    if (usedDefaults) *usedDefaults = true;

    const std::wstring path = configPath();
    if (path.empty()) return def;

    std::string text;
    if (!readWholeFile(path, text)) return def;

    std::string err;
    const JsonValue root = JsonValue::parse(text, &err);
    if (!err.empty() || !root.isObject()) {
        LOG_WARN("config: %s -- using defaults", err.empty() ? "not an object" : err.c_str());
        return def;
    }

    Config c;
    c.game   = channelFromJson(root.find("game"),   def.game);
    c.chat   = channelFromJson(root.find("chat"),   def.chat);
    c.mic    = channelFromJson(root.find("mic"),    def.mic);
    c.output = channelFromJson(root.find("output"), def.output);

    if (const JsonValue* v = root.find("exclusiveOutput"))  c.exclusiveOutput  = v->asBool(def.exclusiveOutput);
    if (const JsonValue* v = root.find("startWithWindows")) c.startWithWindows = v->asBool(def.startWithWindows);
    if (const JsonValue* v = root.find("startMinimized"))   c.startMinimized   = v->asBool(def.startMinimized);
    if (const JsonValue* v = root.find("bufferMillis")) {
        const double m = v->asNumber(def.bufferMillis);
        c.bufferMillis = static_cast<uint32_t>(m < 20 ? 20 : (m > 250 ? 250 : m));
    }

    if (usedDefaults) *usedDefaults = false;
    return c;
}

bool Config::save() const {
    const std::wstring path = configPath();
    if (path.empty()) return false;

    JsonValue root = JsonValue::object();
    root.set("version",          JsonValue(kConfigVersion));
    root.set("game",             channelToJson(game));
    root.set("chat",             channelToJson(chat));
    root.set("mic",              channelToJson(mic));
    root.set("output",           channelToJson(output));
    root.set("exclusiveOutput",  JsonValue(exclusiveOutput));
    root.set("startWithWindows", JsonValue(startWithWindows));
    root.set("startMinimized",   JsonValue(startMinimized));
    root.set("bufferMillis",     JsonValue(static_cast<double>(bufferMillis)));

    const std::string text = root.dump(2);

    // Write to a sibling temp file, then swap it in. A crash part-way through
    // leaves the previous config intact rather than a truncated one.
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != text.size()) { DeleteFileW(tmp.c_str()); return false; }

    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace audiomon
