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

struct ChannelConfig {
    std::wstring deviceId;         // exact endpoint id, if known
    std::wstring deviceNameMatch;  // case-insensitive substring fallback
    float        gain  = 1.0f;     // linear, 0..1
    bool         muted = false;
};

struct Config {
    ChannelConfig game;
    ChannelConfig chat;
    ChannelConfig mic;
    ChannelConfig output;

    bool     exclusiveOutput  = true;
    bool     startWithWindows = false;
    // Only affects a MANUAL launch. The autostart entry passes --tray, so a
    // boot launch goes to the notification area regardless -- defaulting this
    // to true as well would make a first double-click look like it did
    // nothing at all.
    bool     startMinimized   = false;
    uint32_t bufferMillis     = 50;   // per-channel drift setpoint

    static Config defaults();

    // %APPDATA%\audio-monitor, created if absent. Empty on failure.
    static std::wstring appDataDir();
    static std::wstring configPath();

    // Missing or corrupt file -> defaults, reported via `usedDefaults`.
    static Config load(bool* usedDefaults = nullptr);

    // Written via a temp file and ReplaceFile so a crash mid-write cannot
    // leave a truncated config behind.
    bool save() const;
};

} // namespace audiomon
