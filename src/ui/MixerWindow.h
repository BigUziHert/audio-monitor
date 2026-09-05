#pragma once
#include "audio/AudioEngine.h"
#include "audio/AppAudio.h"
#include "audio/Spectrum.h"
#include "audio/Overlap.h"
#include "util/DiagnosticExport.h"
#include <array>
#include <future>

namespace audiomon::ui {
class SpatialNavigation;
class MixerWindow {
  public:
    void init(AudioEngine *engine, Config *config, void *window);
    void setVisible(bool visible);
    void shutdown();
    bool draw(float dt, int width, int height);
    bool hitTitleBar(int x, int y, int width, int height) const;
    bool exitRequested() const {
        return exitRequested_;
    }

  private:
    friend struct MixerWindowTestAccess;
    static constexpr float kApplicationRefreshSeconds = 5.f;
    static constexpr float kStatusRefreshSeconds = .2f;
    static constexpr float kSpectrumRefreshSeconds = 1.f / 30.f;
    void refreshDevices();
    void syncMeteringVisibility();
    void refreshStatus(float dt);
    bool updateDropoutTimer(StreamState outputState, uint64_t underruns, uint64_t dropped,
                            float dt);
    bool updateStartupSettling(bool running, bool allStreamsReady, float dt);
    void addStatusWarning(const std::string &label, const std::string &detail, int severity = 1);
    void refreshDropoutStatus(StreamState outputState, uint64_t underruns, uint64_t dropped,
                              float dt);
    void restart();
    void startDiagnosticExport(const std::wstring &directory);
    void pollDiagnosticExport();
    bool drawSource(size_t index, float width, SpatialNavigation &navigation);
    bool drawDialogs();
    AudioEngine *engine_ = nullptr;
    Config *config_ = nullptr;
    void *window_ = nullptr;
    std::array<MeterBallistics, kMaxSources> meters_;
    // Source capture and its meter stay on one continuous timeline while the
    // dashboard is visible. Start/Stop changes output forwarding only.
    bool visible_ = false;
    bool meteringStartAttempted_ = false;
    MeterBallistics outputMeter_;
    Spectrum spectrum_;
    std::vector<DeviceInfo> playback_, microphones_;
    std::vector<AppAudioInfo> apps_;
    std::string status_ = "Stopped", statusDetail_;
    int severity_ = 0;
    float scale_ = 1, refreshTimer_ = 0, clippingTimer_ = 0, dropoutTimer_ = 0;
    float statusRefreshTimer_ = 0;
    float spectrumRefreshTimer_ = 0;
    float startupSettleTimer_ = 0;
    uint64_t lastUnderruns_ = 0, lastDropped_ = 0;
    bool monitoringWasRunning_ = false;
    bool lastStatusMonitoring_ = false;
    bool statusRefreshForced_ = true;
    bool spectrumWasActive_ = false;
    uint64_t statusEvaluationCount_ = 0;
    uint64_t spectrumEvaluationCount_ = 0;
    bool exitRequested_ = false, openSettings_ = false, openSource_ = false;
    bool openChannels_ = false, openStatus_ = false, openOutput_ = false;
    int settingsPage_ = 1;
    bool resetDevicesOnSave_ = false;
    Config settingsDraft_;
    int editSource_ = -1;
    size_t selectedOutput_ = 0;
    int editOutput_ = 0; // -1 while adding a destination
    ChannelConfig draft_;
    ChannelConfig outputDraft_;
    char name_[128]{};
    char outputName_[128]{};
    std::future<DiagnosticExportResult> diagnosticExport_;
    std::wstring diagnosticPath_;
    std::string diagnosticMessage_;
    bool diagnosticExportFailed_ = false;
};
} // namespace audiomon::ui
