#pragma once
//
// One branch of the fixed-48 kHz mix fan-out.
//
// The central mix pump is the sole producer and one RenderStream is the sole
// consumer.  Giving every output its own OutputBus keeps both the ring and the
// resampler strictly SPSC even when several WASAPI render callbacks run at the
// same time.
//
// publish() applies that destination's trim while copying the canonical mix
// into the ring.  The render side then follows the independent endpoint clock
// with the same slow drift correction used by capture sources.  Neither hot
// path allocates, locks, or blocks.
//
#include "audio/RateController.h"
#include "audio/RenderStream.h"
#include "audio/Resampler.h"
#include "audio/RingBuffer.h"
#include "audio/FadeEnvelope.h"

#include <atomic>
#include <cstdint>

namespace audiomon {

class OutputBus final : public IMixSource {
public:
    static constexpr uint32_t kSourceSampleRate       = 48000;
    // Base queue; the consumer adds 25% phase reserve (normally 5 ms). This
    // headroom prevents drift from starving a callback at a pump packet edge.
    static constexpr uint32_t kTargetBufferMillis     = 20;
    static constexpr uint32_t kDefaultTargetFrames    =
        kSourceSampleRate * kTargetBufferMillis / 1000;
    // About 170 ms at 48 kHz.  This leaves ample scheduling headroom while the
    // controller holds normal latency at the much smaller target above.
    static constexpr uint32_t kDefaultRingCapacityFrames = 8192;

    // targetFrames is injectable so the SPSC and resampling behaviour can be
    // tested with tiny deterministic blocks.  Production callers use the
    // defaults.  StereoRing rounds capacityFrames up to a power of two.
    explicit OutputBus(uint32_t capacityFrames = kDefaultRingCapacityFrames,
                       uint32_t targetFrames = kDefaultTargetFrames);
    ~OutputBus() override = default;

    OutputBus(const OutputBus&) = delete;
    OutputBus& operator=(const OutputBus&) = delete;

    // Producer side.  `src` is interleaved stereo at exactly 48 kHz.  A null
    // source publishes timed silence.  gain is linear and muted overrides it.
    // When the ring is full, the unwritten tail is dropped and the consumer is
    // told that the timeline broke; the producer is never held up.
    void publish(const float* src, uint32_t frames, float gain, bool muted) noexcept;

    // Producer/control-side discontinuity notification (pump restart, seek,
    // or any other break not represented by an overflow).  The consumer owns
    // the read cursor, so it performs the actual drop on its next callback.
    void requestReset() noexcept;

    // IMixSource -- called only by this bus's RenderStream consumer.
    void renderMix(float* dst, uint32_t frames) noexcept override;
    void onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept override;
    void onRenderPeriod(uint32_t nominalFrames) noexcept override;

    // Lock-free diagnostics. droppedFrames includes overflow rejection and
    // stale frames trimmed after a scheduling hitch; starvedFrames counts
    // missing output frames after playback left its initial priming state.
    uint32_t depthFrames() const noexcept { return ring_.depth(); }
    // An observer must not combine two independently changing SPSC cursors.
    // The consumer publishes its own bounded pre-consume observation instead.
    uint32_t diagnosticDepthFrames() const noexcept { return depthOut_.load(std::memory_order_relaxed); }
    uint32_t diagnosticEpoch() const noexcept { return timelineEpoch_.load(std::memory_order_acquire); }
    uint64_t overflowFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    uint64_t trimmedFrames() const noexcept { return latencyTrimmed_.load(std::memory_order_relaxed); }
    uint32_t capacityFrames() const noexcept { return ring_.capacity(); }
    uint32_t targetFrames() const noexcept { return targetFrames_; }
    uint32_t effectiveTargetFrames() const noexcept {
        return effectiveTargetOut_.load(std::memory_order_relaxed);
    }
    uint32_t renderSampleRate() const noexcept {
        return renderRateOut_.load(std::memory_order_acquire);
    }
    double resamplingRatio() const noexcept {
        return ratioOut_.load(std::memory_order_relaxed);
    }
    bool priming() const noexcept { return primingOut_.load(std::memory_order_relaxed); }
    uint64_t publishedFrames() const noexcept {
        return published_.load(std::memory_order_relaxed);
    }
    uint64_t droppedFrames() const noexcept {
        return dropped_.load(std::memory_order_relaxed) +
            latencyTrimmed_.load(std::memory_order_relaxed);
    }
    uint64_t latencyCorrections() const noexcept {
        return latencyCorrections_.load(std::memory_order_relaxed);
    }
    uint64_t starvedFrames() const noexcept {
        return starved_.load(std::memory_order_relaxed);
    }
    uint64_t starvationEvents() const noexcept {
        return starvationEvents_.load(std::memory_order_relaxed);
    }
    void clearStatistics() noexcept;

private:
    void resetConsumer(uint32_t keepNewestFrames = 0) noexcept;
    void applyPresence(float* samples, uint32_t frames) noexcept;
    void finishTimelineBreak() noexcept;
    void setCallbackTarget(uint32_t frames) noexcept;

    StereoRing     ring_;
    DriftResampler resampler_;
    RateController rate_;
    FadeEnvelope  presence_;
    const uint32_t targetFrames_;

    // Written by requestReset()/publish(), observed by the consumer.  Only the
    // consumer ever moves the read cursor or mutates DSP state.
    std::atomic<uint32_t> timelineEpoch_{0};

    // Consumer-thread-only state.
    uint32_t seenEpoch_ = 0;
    uint32_t renderRate_ = kSourceSampleRate;
    uint32_t effectiveTargetFrames_ = kDefaultTargetFrames;
    uint32_t configuredCallbackFrames_ = 0;
    double   baseRatio_ = 1.0;
    bool     priming_ = true;
    bool     timelineBreakPending_ = false;
    bool     latencyTrimPending_ = false;

    // Cross-thread diagnostics/status.
    std::atomic<uint32_t> renderRateOut_{0};
    std::atomic<uint32_t> depthOut_{0};
    std::atomic<uint32_t> effectiveTargetOut_{kDefaultTargetFrames};
    std::atomic<double>   ratioOut_{1.0};
    std::atomic<bool>     primingOut_{true};
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> latencyTrimmed_{0};
    std::atomic<uint64_t> latencyCorrections_{0};
    std::atomic<uint64_t> starved_{0};
    std::atomic<uint64_t> starvationEvents_{0};
};

} // namespace audiomon
