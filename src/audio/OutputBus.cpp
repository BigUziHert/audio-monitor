#include "audio/OutputBus.h"

#include <algorithm>
#include <cmath>

namespace audiomon {
namespace {

constexpr uint32_t kResamplerHistoryFrames = 4;
constexpr uint32_t kMinimumRingFrames = 8;
constexpr uint32_t kLargestRingFrames = uint32_t{1} << 31;
constexpr double kTransitionSeconds = 0.005;
constexpr uint32_t kProducerBlockFrames = OutputBus::kSourceSampleRate / 100; // 10 ms pump

uint32_t normalizedCapacity(uint32_t requested) noexcept {
    // RingBuffer's generic rounding loop assumes the requested power of two
    // is representable.  Bound this public adapter's input so a bad diagnostic
    // value cannot wrap that loop, and leave enough room for the four-frame
    // interpolation history plus useful audio.
    requested = std::clamp(requested, kMinimumRingFrames, kLargestRingFrames);
    uint32_t rounded = kMinimumRingFrames;
    while (rounded < requested) rounded <<= 1;
    return rounded;
}

uint32_t usableTarget(uint32_t requested, uint32_t capacity) noexcept {
    // Four frames are required to prime the Catmull-Rom kernel.  Keep tiny
    // injected test rings useful, but never advertise an unreachable target.
    if (capacity < kResamplerHistoryFrames) return capacity;
    return std::clamp(requested, kResamplerHistoryFrames, capacity);
}

double requiredInputFrames(uint32_t outputFrames, double inputPerOutput) noexcept {
    return std::ceil(double(outputFrames) * inputPerOutput) +
           double(kResamplerHistoryFrames);
}

} // namespace

OutputBus::OutputBus(uint32_t capacityFrames, uint32_t targetFrames)
    : targetFrames_(usableTarget(targetFrames, normalizedCapacity(capacityFrames))) {
    ring_.init(normalizedCapacity(capacityFrames));
    effectiveTargetFrames_ = targetFrames_;
    effectiveTargetOut_.store(targetFrames_, std::memory_order_relaxed);
}

void OutputBus::publish(const float* src, uint32_t frames,
                        float gain, bool muted) noexcept {
    if (frames == 0) return;

    // Invalid UI/config values must not poison an audio ring with NaNs.  Do
    // not otherwise clamp here: the public gain contract is linear and the
    // caller owns its policy range.
    const float amp = (!muted && std::isfinite(gain)) ? gain : 0.0f;
    const uint32_t space = ring_.beginWrite();
    const uint32_t written = std::min(space, frames);

    if (src && amp != 0.0f) {
        for (uint32_t i = 0; i < written; ++i)
            ring_.writeFrame(i, src[i * 2] * amp, src[i * 2 + 1] * amp);
    } else {
        for (uint32_t i = 0; i < written; ++i)
            ring_.writeFrame(i, 0.0f, 0.0f);
    }
    ring_.endWrite(written);
    published_.fetch_add(written, std::memory_order_relaxed);

    if (written < frames) {
        // Never apply back-pressure to the central mix pump.  The retained
        // prefix and anything already queued belong to a broken timeline; the
        // consumer will discard them and re-prime on its next callback.
        dropped_.fetch_add(frames - written, std::memory_order_relaxed);
        timelineEpoch_.fetch_add(1, std::memory_order_release);
    }
}

void OutputBus::requestReset() noexcept {
    timelineEpoch_.fetch_add(1, std::memory_order_release);
}

void OutputBus::resetConsumer(uint32_t keepNewestFrames) noexcept {
    if (keepNewestFrames > 0) {
        const uint32_t available = ring_.beginRead();
        const uint32_t trimmed = available - std::min(available, keepNewestFrames);
        ring_.endRead(trimmed);
        latencyTrimmed_.fetch_add(trimmed, std::memory_order_relaxed);
        latencyCorrections_.fetch_add(1, std::memory_order_relaxed);
    } else {
        ring_.dropAllFromConsumer();
    }
    resampler_.reset();
    rate_.reset();
    priming_ = true;
    timelineBreakPending_ = false;
    latencyTrimPending_ = false;
    presence_.reset(0.0f);
    primingOut_.store(true, std::memory_order_relaxed);
    ratioOut_.store(baseRatio_, std::memory_order_relaxed);
}

void OutputBus::finishTimelineBreak() noexcept {
    const uint32_t epoch = timelineEpoch_.load(std::memory_order_acquire);
    // A real discontinuity that arrives during a latency fade invalidates the
    // whole timeline. Only an intact stream may retain its newest short tail.
    const uint32_t keep = latencyTrimPending_ && epoch == seenEpoch_
        ? effectiveTargetFrames_ : 0;
    seenEpoch_ = epoch;
    resetConsumer(keep);
}

void OutputBus::setCallbackTarget(uint32_t frames) noexcept {
    const double maximumNeed = requiredInputFrames(frames, baseRatio_ * RateController::kMaxRatio);
    const uint32_t reserve = std::max<uint32_t>(1, targetFrames_ / 4);
    // Half a producer packet of phase headroom is needed even when nominal
    // consumption fits in the base target. Independent clocks slowly shift
    // a render callback across a 10 ms pump boundary; a bare 20 ms setpoint
    // can otherwise lose one frame during that packet-phase transition.
    const double desiredTarget = std::max(maximumNeed, double(targetFrames_)) + double(reserve);
    effectiveTargetFrames_ = static_cast<uint32_t>(std::min(desiredTarget, double(ring_.capacity())));
    effectiveTargetOut_.store(effectiveTargetFrames_, std::memory_order_relaxed);
    rate_.setTarget(double(effectiveTargetFrames_));
}

void OutputBus::applyPresence(float* samples, uint32_t frames) noexcept {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const float gain = presence_.next(frame);
        samples[frame * 2] *= gain;
        samples[frame * 2 + 1] *= gain;
    }
}

void OutputBus::onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept {
    renderRate_ = sampleRate ? sampleRate : kSourceSampleRate;
    baseRatio_ = double(kSourceSampleRate) / double(renderRate_);

    // blockFrames is the largest buffer RenderStream can ask us to fill.  For
    // ordinary 5-10 ms periods, a 20 ms base plus 5 ms phase reserve is enough
    // for independently clocked packets. A longer endpoint period needs a larger
    // effective setpoint or every callback would drain the ring to zero.
    setCallbackTarget(blockFrames);

    const double period = blockFrames
        ? double(blockFrames) / double(renderRate_)
        : 0.01;
    rate_.configure(double(kSourceSampleRate), double(effectiveTargetFrames_), period);
    configuredCallbackFrames_ = 0;
    presence_.configure(static_cast<uint32_t>(
        std::max(1.0, double(renderRate_) * kTransitionSeconds)));

    renderRateOut_.store(renderRate_, std::memory_order_release);
    seenEpoch_ = timelineEpoch_.load(std::memory_order_acquire);
    resetConsumer();
}

void OutputBus::onRenderPeriod(uint32_t nominalFrames) noexcept {
    if (nominalFrames > 0) setCallbackTarget(nominalFrames);
}

void OutputBus::renderMix(float* dst, uint32_t frames) noexcept {
    if (!dst || frames == 0) return;
    std::fill_n(dst, static_cast<size_t>(frames) * 2, 0.0f);

    const uint32_t epoch = timelineEpoch_.load(std::memory_order_acquire);
    if (epoch != seenEpoch_ && !timelineBreakPending_) {
        // If this endpoint was audible, spend the already queued old timeline
        // on a short fade before dropping it. A stopped/reopened device cannot
        // play that fade, but presence_ still guarantees its new timeline
        // fades in from silence after onRenderFormat().
        if (!priming_ && presence_.value() > 0.0f &&
            ring_.depth() >= kResamplerHistoryFrames) {
            timelineBreakPending_ = true;
        } else {
            seenEpoch_ = epoch;
            resetConsumer();
            return;
        }
    }

    // A render thread can miss several periods while the capture/pump stays
    // healthy. The resulting extra depth is a scheduling step, not crystal
    // drift; allowing the minute-scale PI loop to drain it would leave audio
    // noticeably late long after the game hitch. Keep normal packet jitter
    // (two 10 ms producer blocks) untouched, and fade/trim larger backlogs.
    const uint32_t allowance = std::max(effectiveTargetFrames_, 2 * kProducerBlockFrames);
    const uint32_t highWater = static_cast<uint32_t>(std::min(
        uint64_t(ring_.capacity()), uint64_t(effectiveTargetFrames_) + allowance));
    if (!timelineBreakPending_ && ring_.depth() > highWater) {
        latencyTrimPending_ = true;
        timelineBreakPending_ = true;
    }

    if (timelineBreakPending_) {
        const uint32_t requested =
            std::min(frames, presence_.framesUntilSilent());
        if (requested == 0) {
            finishTimelineBreak();
            return;
        }

        const double ratio = baseRatio_ * rate_.ratio();
        ratioOut_.store(ratio, std::memory_order_relaxed);
        const uint32_t made = resampler_.produce(ring_, dst, requested, ratio);
        // A planned fade may span short endpoint periods. If the old timeline
        // unexpectedly runs out, schedule the taper to land on its actual end.
        presence_.beginBlock(made, frames, made == requested);
        applyPresence(dst, frames);

        if (made < requested || presence_.value() <= 0.0f) {
            finishTimelineBreak();
        }
        return;
    }


    // Padding can vary individual shared-mode callback sizes; change only
    // controller elapsed time here, never infer a smaller safety target from
    // one short callback. onRenderPeriod supplies the endpoint's normal clock.
    if (configuredCallbackFrames_ != frames) {
        rate_.setUpdatePeriod(double(frames) / double(renderRate_));
        configuredCallbackFrames_ = frames;
    }

    const uint32_t depth = ring_.depth();
    if (priming_) {
        // Start with ~20 ms queued so a small scheduling lead by this endpoint
        // does not immediately drain the branch.  The current callback also
        // needs four interpolation-history frames plus its worst-case input
        // consumption.  Without this second threshold, an unusually large
        // endpoint period would leave a permanent prime/starve/reset loop.
        const double worstRatio = baseRatio_ * RateController::kMaxRatio;
        const double neededExact = requiredInputFrames(frames, worstRatio);
        if (neededExact > double(ring_.capacity())) {
            // This injected/custom capacity cannot service even one callback.
            // Stay primed and expose the configuration failure as starvation
            // rather than emitting the same partial, clicking block forever.
            starved_.fetch_add(frames, std::memory_order_relaxed);
            starvationEvents_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const uint32_t required = std::max(
            effectiveTargetFrames_, static_cast<uint32_t>(neededExact));
        if (depth < required) return;
        priming_ = false;
        primingOut_.store(false, std::memory_order_relaxed);
        rate_.reset();
    }

    const double ratio = baseRatio_ * rate_.update(double(depth));
    ratioOut_.store(ratio, std::memory_order_relaxed);
    const uint32_t made = resampler_.produce(ring_, dst, frames, ratio);
    presence_.beginBlock(made, frames);
    applyPresence(dst, frames);

    if (made < frames) {
        // The buffer discontinuity invalidates both the interpolation history
        // and the slow PI controller.  Silence is already present in the tail
        // because the destination was cleared before produce().
        starved_.fetch_add(frames - made, std::memory_order_relaxed);
        starvationEvents_.fetch_add(1, std::memory_order_relaxed);
        resetConsumer();
    }
}

void OutputBus::clearStatistics() noexcept {
    published_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    latencyTrimmed_.store(0, std::memory_order_relaxed);
    latencyCorrections_.store(0, std::memory_order_relaxed);
    starved_.store(0, std::memory_order_relaxed);
    starvationEvents_.store(0, std::memory_order_relaxed);
}

} // namespace audiomon
