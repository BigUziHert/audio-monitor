#pragma once
#include "audio/AudioEngine.h"
#include "audio/AppAudio.h"
#include "audio/Spectrum.h"
#include "audio/Overlap.h"
#include <array>

namespace audiomon::ui {
class MixerWindow {
  public:
    void init(AudioEngine *engine, Config *config, void *window);
    bool draw(float dt, int width, int height);
    bool exitRequested() const {
        return exitRequested_;
    }

  private:
    void refreshDevices();
    void refreshStatus(float dt);
    void restart();
    bool drawSource(size_t index, float width, float dt);
    bool drawDialogs();
    AudioEngine *engine_ = nullptr;
    Config *config_ = nullptr;
    void *window_ = nullptr;
    std::array<MeterBallistics, kMaxSources> meters_;
    MeterBallistics outputMeter_;
    Spectrum spectrum_;
    std::vector<DeviceInfo> playback_, microphones_;
    std::vector<AppAudioInfo> apps_;
    std::string status_ = "Stopped", statusDetail_;
    int severity_ = 0;
    float scale_ = 1, refreshTimer_ = 0, clippingTimer_ = 0, dropoutTimer_ = 0;
    uint64_t lastUnderruns_ = 0, lastDropped_ = 0;
    bool exitRequested_ = false, openSettings_ = false, openSource_ = false;
    int editSource_ = -1;
    ChannelConfig draft_;
    char name_[128]{};
};
} // namespace audiomon::ui
