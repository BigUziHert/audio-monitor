#pragma once
//
// Persisted settings: fader positions, mute states and device selections,
// stored as JSON under %APPDATA%\audio-monitor\config.json.
//
// Devices are stored as BOTH an endpoint ID and a friendly-name substring. The
// ID is exact but a driver reinstall reissues it; the name substring is what
// lets the config survive that. On first run there is no config at all, so the
// defaults autodetect by name.
//
#include "util/Text.h"

#include <cstdint>
#include <string>
#include <vector>

namespace audiomon {
class JsonValue;

inline constexpr int kMaxSources = 16;
enum class SourceKind { Playback, Microphone, Application };

struct ChannelConfig {
    std::string  label;
    SourceKind   kind = SourceKind::Playback;
    bool         enabled = true;
    std::wstring processPath;      // stable identity; never persist a process ID
    std::wstring deviceId;         // exact endpoint id, if known
    std::wstring deviceNameMatch;  // case-insensitive substring fallback
    std::string  icon;             // optional UI icon key; empty keeps the automatic icon
    float        gain  = 1.0f;     // linear, 0..4 (100% is 1.0)
    bool         muted = false;
};

struct Config {
    std::vector<ChannelConfig> sources;
    ChannelConfig output;

    bool     mono             = false;
    bool     closeToTray      = false;

    bool     exclusiveOutput  = true;
    bool     startWithWindows = false;
    // Only affects a MANUAL launch. The autostart entry passes --tray, so a
    // boot launch goes to the notification area regardless -- defaulting this
    // to true as well would make a first double-click look like it did
    // nothing at all.
    bool     startMinimized   = false;
    uint32_t bufferMillis     = 50;   // per-channel drift setpoint

    static Config defaults();
    static Config fromJson(const JsonValue& root);
    JsonValue toJson() const;

    // %APPDATA%\audio-monitor, created if absent. Empty on failure.
    static std::wstring appDataDir();
    static std::wstring configPath();

    // Missing or corrupt file -> defaults, reported via `usedDefaults`.
    static Config load(bool* usedDefaults = nullptr);
    static Config load(const std::wstring& path, bool* usedDefaults = nullptr);

    // Written via a sibling temp file and an atomic replacement so a crash
    // mid-write cannot leave a truncated config behind.
    bool save() const;
    bool save(const std::wstring& path) const;
};

} // namespace audiomon
