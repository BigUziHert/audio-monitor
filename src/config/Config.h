#pragma once
//
// Persisted settings: fader positions, mute states and device selections,
// stored as JSON under %APPDATA%\audio-monitor\config.json.
//
// Devices are stored as BOTH an endpoint ID and a friendly-name substring. The
// ID is exact but a driver reinstall reissues it; the name substring is what
// lets the config survive that. On first run there is no config at all, so the
// source defaults autodetect by name; playback destinations are selected by
// the user rather than guessed from machine-specific hardware names.
//
#include "util/Text.h"

#include <cstdint>
#include <string>
#include <stdexcept>
#include <vector>

namespace audiomon {
class JsonValue;

inline constexpr int kMaxSources = 16;
inline constexpr int kMaxOutputs = 4;
enum class SourceKind { Playback, Microphone, Application };
enum class ColorTheme { Dark, Light, System };

struct ChannelConfig {
    std::string  label;
    SourceKind   kind = SourceKind::Playback;
    bool         enabled = true;
    std::wstring processPath;      // stable identity; never persist a process ID
    std::wstring deviceId;         // exact endpoint id, if known
    std::wstring deviceNameMatch;  // case-insensitive substring fallback
    std::string  icon;             // optional UI icon key; empty keeps the automatic icon
    float        gain   = 1.0f;    // mix gain, 0..4 (100% is 1.0)
    float        volume = 1.0f;    // dashboard volume, 0..1
    bool         muted = false;
};

inline float effectiveGain(const ChannelConfig& channel) noexcept {
    return channel.gain * channel.volume;
}

// True when two persisted endpoint selections identify the same physical
// destination. IDs are authoritative when both exist; otherwise the friendly
// name fallback uses the same case-insensitive substring semantics as device
// resolution so a migrated name-only output cannot be added again by ID.
bool sameEndpointSelection(const ChannelConfig& a, const ChannelConfig& b) noexcept;

struct Config {
    std::vector<ChannelConfig> sources;
    // `output` remains the first destination so older configuration files and
    // code keep their meaning. Extra destinations mirror the same mix and
    // retain their own device, trim, mute, label, and icon settings.
    ChannelConfig output;
    std::vector<ChannelConfig> additionalOutputs;
    // Bare Config{} retains the legacy primary slot for existing code that
    // assigns output directly. defaults()/clearOutputs() explicitly remove it.
    bool hasOutput = true;

    size_t outputCount() const noexcept {
        if (!hasOutput) return 0;
        const size_t count = 1 + additionalOutputs.size();
        return count < size_t(kMaxOutputs) ? count : size_t(kMaxOutputs);
    }
    ChannelConfig& outputAt(size_t index) {
        if (index >= outputCount()) throw std::out_of_range("output index");
        return index == 0 ? output : additionalOutputs.at(index - 1);
    }
    const ChannelConfig& outputAt(size_t index) const {
        if (index >= outputCount()) throw std::out_of_range("output index");
        return index == 0 ? output : additionalOutputs.at(index - 1);
    }
    // Returns false for an empty selection, duplicate, or the four-output cap.
    bool addOutput(ChannelConfig channel);
    // Deleting the primary promotes the next output; deleting the last keeps
    // an explicitly empty list, including after saving and restarting.
    bool removeOutput(size_t index);
    void clearOutputs();

    bool     mono             = false;
    bool     closeToTray      = false;

    bool     exclusiveOutput  = true;
    bool     startWithWindows = false;
    // Only affects a MANUAL launch. The autostart entry passes --tray, so a
    // boot launch goes to the notification area regardless -- defaulting this
    // to true as well would make a first double-click look like it did
    // nothing at all.
    bool       startMinimized = false;
    ColorTheme colorTheme     = ColorTheme::Dark;
    uint32_t   bufferMillis   = 50;   // per-channel drift setpoint

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
