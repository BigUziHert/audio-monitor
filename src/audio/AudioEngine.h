#pragma once
//
// The mixer.
//
// Topology: one capture thread per enabled source, each writing its own SPSC
// ring, and one render thread that resamples them onto the render clock,
// sums, and writes the Elgato. A supervisor thread handles every device
// rebuild so neither the render thread nor a COM notification callback ever
// does that work.
//
// The alternative -- draining the captures on the render thread -- was
// rejected: a single stalled capture device would then stall the output.
// Here a stalled capture just stops filling its ring and the mixer emits
// silence for that channel.
//
#include "audio/CaptureStream.h"
#include "audio/RenderStream.h"
#include "audio/RateController.h"
#include "audio/Resampler.h"
#include "audio/Meter.h"
#include "audio/FadeEnvelope.h"
#include "audio/MixMode.h"
#include "config/Config.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace audiomon {



struct ChannelStatus {
    StreamState  state      = StreamState::Stopped;
    bool         flowing    = false;
    uint32_t     depth      = 0;
    uint32_t     sampleRate = 0;
    double       ratio      = 1.0;
    uint64_t     dropped    = 0;
    std::wstring deviceId;
    uint32_t processId = 0;
    std::wstring deviceName;
    std::string  error;
};

struct OutputStatus {
    StreamState  state       = StreamState::Stopped;
    bool         exclusive   = false;
    uint32_t     sampleRate  = 0;
    uint32_t     blockFrames = 0;
    uint64_t     underruns   = 0;
    std::wstring deviceId;
    std::wstring deviceName;
    std::string  error;
};

class AudioEngine final : public IMixSource {
public:
    AudioEngine();
    ~AudioEngine() override;

    bool start(const Config& config);
    void stop();

    // Applied live from the UI thread; picked up by the audio thread on its
    // next block. Gain is linear, 0..1 mapping is the caller's business.
    void setGain(int channel, float linearGain) noexcept;
    void setMuted(int channel, bool muted) noexcept;
    void setOutputGain(float linearGain) noexcept;
    void setOutputMuted(bool muted) { outputMuted_.store(muted, std::memory_order_relaxed); }
    void setEnabled(int channel, bool enabled);
    void setMono(bool mono) { mono_.store(mono, std::memory_order_relaxed); }
    bool running() const { return running_.load(std::memory_order_acquire); }
    StereoRing& visualSamples() { return visualSamples_; }

    // Applies at the next device open: exclusive vs shared is decided during
    // IAudioClient::Initialize and cannot be changed on a live stream.
    void setExclusiveOutput(bool exclusive) {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_.exclusiveOutput = exclusive;
    }

    // Takes effect immediately. The mixer notices the change on its next block
    // and moves each channel's setpoint without resetting anything, so the
    // buffer migrates to the new depth over some seconds with no dropout.
    void setBufferMillis(uint32_t ms) noexcept {
        bufferMillis_.store(ms < 20 ? 20 : (ms > 250 ? 250 : ms), std::memory_order_relaxed);
    }

    // UI polling. Reads atomics only; never blocks the audio path.
    StereoPeak&   channelPeak(int channel) noexcept { return channels_[channel]->peak; }
    StereoPeak&   outputPeak() noexcept { return outputPeak_; }
    ChannelStatus channelStatus(int channel) const;
    OutputStatus  outputStatus() const;

    // Writes back any device IDs that were re-resolved by name, so a driver
    // reinstall is persisted rather than re-matched every launch.
    void updateConfigFromRuntime(Config& config) const;

    // For the settings UI.
    std::vector<DeviceInfo> listDevices(EDataFlow flow) const { return devices_.list(flow); }

    // Re-points a channel at a different endpoint and restarts just that
    // stream. channel < 0 means the output. Runs on the calling (UI) thread
    // only far enough to hand the work to the stream's own worker.
    void setChannelDevice(int channel, const DeviceRef& ref);

    // --- IMixSource, called on the render thread ---
    void renderMix(float* dst, uint32_t frames) noexcept override;
    void onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept override;

private:
    struct Channel {
        CaptureStream  stream;
        DriftResampler resampler;
        RateController rate;
        StereoPeak     peak;

        std::vector<float> scratch;      // preallocated render-block scratch

        std::atomic<float>    gain{1.0f};
        std::atomic<bool>     muted{false};
        std::atomic<uint32_t> depthOut{0};
        std::atomic<double>   ratioOut{1.0};

        // Audio-thread-only state.
        float    smoothedGain = 1.0f;
        FadeEnvelope presence;           // kills clicks at both edges of a gap
        bool     priming      = true;
        uint32_t lastEpoch    = 0;
        double   baseRatio    = 1.0;     // captureRate / renderRate
        // Denominated in CAPTURE frames, because that is what ring depth is
        // measured in. Recomputed whenever the source rate changes.
        double   targetDepth  = 0.0;
        uint32_t lastSrcRate  = 0;
        uint32_t lastBufferMs = 0;   // so a slider move retargets live
    };

    void supervisorMain();
    void requestRebuild();

    std::array<std::unique_ptr<Channel>, kMaxSources> channels_;
    RenderStream  render_;
    DeviceManager devices_;
    StereoPeak    outputPeak_;

    size_t sourceCount_ = 0; // changed only while every worker is stopped
    StereoRing visualSamples_;
    MixMode mixMode_;
    std::atomic<bool> mono_{false}, outputMuted_{false};
    std::atomic<float> outputGain_{1.0f};
    float              smoothedOutputGain_ = 1.0f;

    Config             config_;
    mutable std::mutex configMutex_;

    std::thread             supervisor_;
    std::mutex              superMutex_;
    std::condition_variable superCv_;
    bool                    superWake_ = false;
    std::atomic<bool>       quit_{false};
    std::atomic<bool>       running_{false};

    std::atomic<uint32_t> renderRate_{0};
    std::atomic<uint32_t> renderBlock_{0};

    // Held as an atomic rather than read from config_ under configMutex_:
    // onRenderFormat runs on the render thread, and taking a lock there is
    // both a real-time hazard and -- because a UI-thread device change joins
    // that same thread -- a deadlock waiting to happen.
    std::atomic<uint32_t> bufferMillis_{50};
};

} // namespace audiomon
