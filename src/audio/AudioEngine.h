#pragma once
//
// The mixer.
//
// Topology: one capture thread per enabled source writes one SPSC ring. A
// fixed-48 kHz pump is their sole consumer, mixes once, and fans the result to
// one SPSC OutputBus per destination. Each output has its own render thread and
// drift resampler, so several playback devices can run at independent clocks
// without reading a capture ring more than once. A supervisor handles rebuilds.
//
// The alternative -- draining the captures on the render thread -- was
// rejected: a single stalled capture device would then stall the output.
// Here a stalled capture just stops filling its ring and the mixer emits
// silence for that channel.
//
#include "audio/CaptureStream.h"
#include "audio/RenderStream.h"
#include "audio/OutputBus.h"
#include "audio/MixPumpScheduler.h"
#include "audio/RateController.h"
#include "audio/Resampler.h"
#include "audio/Meter.h"
#include "audio/FadeEnvelope.h"
#include "audio/MixMode.h"
#include "audio/RetryPolicy.h"
#include "config/Config.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <vector>
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
    uint64_t     dropped     = 0; // canonical frames rejected by this output bus
    uint64_t     pumpMissedPeriods = 0; // global; count once when aggregating outputs
    std::wstring deviceId;
    std::wstring deviceName;
    std::string  error;
};

class AudioEngine final : public IMixSource {
public:
    AudioEngine();
    ~AudioEngine() override;

    bool start(const Config& config, bool monitoring = true);
    void stop();

    // Capture and source meters stay alive while forwarding is paused. This
    // request never joins a worker: the supervisor owns output transitions.
    void setMonitoring(bool enabled);
    bool monitoring() const noexcept {
        return running() && (monitoringState_.load(std::memory_order_acquire) & 1u) != 0;
    }

    // Applied live from the UI thread; picked up by the audio thread on its
    // next block. Gain is linear, 0..4 mapping is the caller's business.
    void setGain(int channel, float linearGain) noexcept;
    void setMuted(int channel, bool muted) noexcept;
    void setOutputGain(size_t output, float linearGain) noexcept;
    void setOutputMuted(size_t output, bool muted) noexcept;
    void setOutputGain(float linearGain) noexcept { setOutputGain(0, linearGain); }
    void setOutputMuted(bool muted) noexcept { setOutputMuted(0, muted); }
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
    StereoPeak&   outputPeak(size_t output = 0) noexcept {
        return outputPeaks_[output < kMaxOutputs ? output : 0];
    }
    ChannelStatus channelStatus(int channel) const;
    OutputStatus  outputStatus(size_t output = 0) const;
    size_t        outputCount() const noexcept { return outputCount_; }
    uint64_t      mixPumpMissedPeriods() const noexcept {
        return mixPumpMissedPeriods_.load(std::memory_order_relaxed);
    }

    // Writes back any device IDs that were re-resolved by name, so a driver
    // reinstall is persisted rather than re-matched every launch.
    bool updateConfigFromRuntime(Config& config) const;

    // For the settings UI.
    std::vector<DeviceInfo> listDevices(EDataFlow flow) const { return devices_.list(flow); }

    // Re-points a channel at a different endpoint and restarts just that
    // stream. channel < 0 means the output. Runs on the calling (UI) thread
    // only far enough to hand the work to the stream's own worker.
    void setChannelDevice(int channel, const DeviceRef& ref);
    void setOutputDevice(size_t output, const DeviceRef& ref);

    // Canonical mixer entry points. The fixed-rate pump calls renderMix;
    // start() configures its fixed format before any audio worker begins.
    void renderMix(float* dst, uint32_t frames) noexcept override;
    void onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept override;

private:
    friend struct AudioEngineTestAccess;
    class OutputGate final : public IMixSource {
    public:
        OutputGate(AudioEngine& engine, size_t output) : engine_(engine), output_(output) {}
        void renderMix(float* dst, uint32_t frames) noexcept override;
        void onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept override;
    private:
        AudioEngine& engine_;
        size_t output_;
    };
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
        uint32_t lastObservedDepth = 0; // distinguishes a stable idle tail from a resume
        bool     timelineBreakPending = false;
    };

    void supervisorMain();
    void mixPumpMain();
    void pumpBlock() noexcept;
    bool synchronizeOutputs(uint64_t state);
    void handleMixPumpDiscontinuity(uint64_t missedPeriods) noexcept;
    void requestRebuild();
    void stopLocked(); // lifecycleMutex_ is held

    std::array<std::unique_ptr<Channel>, kMaxSources> channels_;
    std::array<std::unique_ptr<RenderStream>, kMaxOutputs> renders_;
    std::array<std::unique_ptr<OutputBus>, kMaxOutputs> outputBuses_;
    std::array<std::unique_ptr<OutputGate>, kMaxOutputs> outputGates_;
    DeviceManager devices_;
    std::array<StereoPeak, kMaxOutputs> outputPeaks_;

    size_t sourceCount_ = 0; // changed only while every worker is stopped
    size_t outputCount_ = 1; // changed only while every worker is stopped
    StereoRing visualSamples_;
    MixMode mixMode_;
    std::atomic<bool> mono_{false};
    std::array<std::atomic<bool>, kMaxOutputs> outputMuted_{};
    std::array<std::atomic<float>, kMaxOutputs> outputGains_{};
    std::array<float, kMaxOutputs> smoothedOutputGains_{};
    std::vector<float> mixPumpBuffer_;
    std::array<std::vector<float>, kMaxOutputs> outputScratch_;

    Config             config_;
    mutable std::mutex configMutex_;
    // Serializes complete start/stop transitions. Public UI calls are normally
    // single-threaded, but this also makes teardown safe under lifecycle tests
    // and prevents a concurrent stop from racing partially-created workers.
    std::mutex          lifecycleMutex_;

    std::thread             supervisor_;
    std::thread             mixPump_;
    std::mutex              superMutex_;
    std::condition_variable superCv_;
    MixPumpScheduler        mixPumpScheduler_;
    bool                    superWake_ = false;
    bool                    monitoringWake_ = false;
    std::atomic<bool>       quit_{false};
    std::atomic<bool>       running_{false};
    // The low bit is requested forwarding; the other bits identify a unique
    // transition, including off/on clicks that happen within one pump block.
    std::atomic<uint64_t>   monitoringState_{0};
    std::atomic<uint64_t>   pumpObservedState_{0};
    std::array<std::atomic<uint64_t>, kMaxOutputs> outputReadyState_{};
    std::atomic<uint64_t>   mixPumpMissedPeriods_{0};

    std::atomic<uint32_t> renderRate_{0};
    std::atomic<uint32_t> renderBlock_{0};

    // Held as an atomic rather than read from config_ under configMutex_:
    // renderMix runs on the pump thread, so taking configMutex_ there would be
    // a real-time hazard. Keep the live setpoint atomic instead.
    std::atomic<uint32_t> bufferMillis_{50};
};

} // namespace audiomon
