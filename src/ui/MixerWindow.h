#pragma once
//
// The mixer panel, modelled on the OBS audio mixer.
//
// The important property, and the reason this app exists: a fader here only
// scales what is summed and sent to the capture card. Nothing in this UI ever
// touches an endpoint's volume, so the level in the user's own headset is
// unaffected -- exactly like OBS monitoring.
//
#include "audio/AudioEngine.h"
#include "audio/Meter.h"
#include "config/Config.h"

#include <array>
#include <string>
#include <vector>

namespace audiomon::ui {

class MixerWindow {
public:
    void init(AudioEngine* engine, Config* config);

    // Draws one frame. Returns true if a setting changed and the config should
    // be written back.
    bool draw(float dtSeconds, int windowW, int windowH);

    bool exitRequested() const { return exitRequested_; }

private:
    struct Strip {
        MeterBallistics meterL, meterR;
        std::string     title;
        int             index = 0;
    };

    bool drawChannelStrip(Strip& strip, float dt);
    bool drawOutputSection(float dt);
    bool drawSettings();
    void refreshDeviceLists();

    AudioEngine* engine_ = nullptr;
    Config*      config_ = nullptr;

    std::array<Strip, kChannelCount> strips_;
    MeterBallistics outMeterL_, outMeterR_;

    std::vector<DeviceInfo> renderDevices_;
    std::vector<DeviceInfo> captureDevices_;
    bool  deviceListsLoaded_ = false;
    float contentWidth_      = 0.0f;   // capped and centred; set each frame
    bool  showSettings_      = false;
    bool  exitRequested_     = false;
    float deviceRefreshTimer_ = 0.0f;
};

} // namespace audiomon::ui
