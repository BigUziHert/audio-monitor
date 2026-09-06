#include "audio/AudioEngine.h"
#include "audio/RealtimeThread.h"
#include "util/Log.h"
#include "util/Text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <sstream>

namespace audiomon {
namespace {

// Generous preallocation for deterministic direct-mixer tests and defensive
// format changes. Production pump blocks are fixed at 480 frames.
constexpr uint32_t kMaxBlockFrames = 8192;

// Debounce for device notifications: a driver reinstall fires a burst of them,
// and rebuilding on each one would thrash.
constexpr int kRebuildDebounceMs = 750;
constexpr int kSupervisorPollMs  = 2000;

// Fade length for gain changes and for the edges of a silent gap.
constexpr double kFadeSeconds = 0.005;
constexpr uint32_t kMixSampleRate = OutputBus::kSourceSampleRate;
constexpr uint32_t kMixBlockFrames = 480; // 10 ms at the fixed internal rate.

float smoothingCoef(double seconds, uint32_t rate) {
    if (rate == 0) return 1.0f;
    return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * double(rate))));
}

} // namespace

AudioEngine::AudioEngine() {
    visualSamples_.init(8192);
    for (auto& ch : channels_) {
        ch = std::make_unique<Channel>();
        ch->scratch.assign(static_cast<size_t>(kMaxBlockFrames) * 2, 0.0f);
    }
    mixPumpBuffer_.assign(static_cast<size_t>(kMixBlockFrames) * 2, 0.0f);
    for (size_t i = 0; i < kMaxOutputs; ++i) {
        renders_[i] = std::make_unique<RenderStream>();
        outputBuses_[i] = std::make_unique<OutputBus>();
        outputGates_[i] = std::make_unique<OutputGate>(*this, i);
        outputGains_[i].store(1.0f, std::memory_order_relaxed);
        outputMuted_[i].store(false, std::memory_order_relaxed);
        smoothedOutputGains_[i] = 1.0f;
        outputScratch_[i].assign(static_cast<size_t>(kMixBlockFrames) * 2, 0.0f);
    }
}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(const Config& config, bool monitoring) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
    try {
        sourceCount_ = std::min(config.sources.size(), size_t(kMaxSources));
        outputCount_ = std::min(config.outputCount(), size_t(kMaxOutputs));
        monitoring = monitoring && outputCount_ > 0;
        LOG_INFO("engine: start");
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            config_ = config;
            diagnosticSources_ = {};
            diagnosticOutputs_ = {};
        }
        LOG_INFO("engine: config copied");

        for (size_t i = 0; i < sourceCount_; ++i) {
            const auto& source = config.sources[i];
            auto& channel = *channels_[i];
            auto mode = source.kind == SourceKind::Application ? CaptureMode::Application :
                        source.kind == SourceKind::Microphone ? CaptureMode::Microphone : CaptureMode::Loopback;
            channel.stream.configure(source.label.c_str(), mode);
            channel.gain.store(effectiveGain(source));
            channel.muted.store(source.muted || !source.enabled);
            channel.peak.l.take(); channel.peak.r.take();
            channel.latencyTrimmed.store(0, std::memory_order_relaxed);
            channel.shortfallFrames.store(0, std::memory_order_relaxed);
            channel.shortfallEvents.store(0, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kMaxOutputs; ++i) {
            const bool configured = i < outputCount_;
            const auto& output = configured ? config.outputAt(i) : config.output;
            outputGains_[i].store(configured ? effectiveGain(output) : 0.0f,
                                  std::memory_order_relaxed);
            outputMuted_[i].store(!configured || output.muted, std::memory_order_relaxed);
            smoothedOutputGains_[i] = (!configured || output.muted) ? 0.0f : effectiveGain(output);
            outputPeaks_[i].l.take(); outputPeaks_[i].r.take();
            outputBuses_[i]->requestReset();
            outputBuses_[i]->clearStatistics();
            renders_[i]->clearStatistics();
            outputReadyState_[i].store(0, std::memory_order_relaxed);
        }
        mixPumpMissedPeriods_.store(0, std::memory_order_relaxed);
        mono_.store(config.mono);
        mixMode_.reset(config.mono);
        visualSamples_.dropAllFromConsumer();
        setBufferMillis(config.bufferMillis);

        // The mixer has one fixed clock, independent of every output endpoint.
        // This is what allows several RenderStreams to consume the same mix
        // without ever becoming multiple readers of a capture channel's SPSC ring.
        onRenderFormat(kMixSampleRate, kMixBlockFrames);

        if (!mixPumpScheduler_.open()) {
            LOG_ERR("engine: mix-pump scheduler unavailable (%lu)",
                    mixPumpScheduler_.lastError());
            return false;
        }
        if (!mixPumpScheduler_.highResolution())
            LOG_WARN("engine: high-resolution mix-pump timer unavailable; using normal timer");

        quit_.store(false, std::memory_order_relaxed);
        monitoringState_.store((monitoringState_.load(std::memory_order_relaxed) & ~uint64_t{1}) +
                                   2 + (monitoring ? 1 : 0), std::memory_order_release);
        pumpObservedState_.store(0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        ++diagnosticSession_;
        try {
            std::lock_guard<std::mutex> lock(configMutex_);
            // Runtime objects still describe the preceding session here;
            // retain configured selections only until new streams resolve.
            diagnostics_.beginSession(diagnosticSession_, diagnosticDevicesLocked(false));
        }
        catch (...) {} // Diagnostic retention must not prevent audio startup.
        recordDiagnostics(false);
        if (!devices_.start([this] { requestRebuild(); })) {
            LOG_ERR("engine: device enumerator unavailable");
            stopLocked();
            return false;
        }
        LOG_INFO("engine: device manager up");

        for (size_t i = 0; i < sourceCount_; ++i) {
            const auto& source = config.sources[i];
            if (source.enabled) channels_[i]->stream.start(
                devices_, source.kind == SourceKind::Application
                              ? DeviceRef{L"", source.processPath}
                              : DeviceRef{source.deviceId, source.deviceNameMatch}, "engine start");
        }

        mixPump_ = std::thread(&AudioEngine::mixPumpMain, this);
        LOG_INFO("engine: all streams requested; starting supervisor");
        { std::lock_guard<std::mutex> lock(superMutex_); monitoringWake_ = true; }
        supervisor_ = std::thread(&AudioEngine::supervisorMain, this);
    } catch (const std::exception& e) {
        LOG_ERR("engine: worker start failed: %s", e.what());
        stopLocked();
        return false;
    } catch (...) {
        LOG_ERR("engine: worker start failed with an unknown exception");
        stopLocked();
        return false;
    }
    return true;
}

void AudioEngine::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
}

void AudioEngine::stopLocked() {
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    monitoringState_.fetch_and(~uint64_t{1}, std::memory_order_acq_rel);
    if (!wasRunning && !supervisor_.joinable() && !mixPump_.joinable() &&
        !mixPumpScheduler_.ready())
        return;

    LOG_INFO("engine: stop requested");
    quit_.store(true, std::memory_order_relaxed);
    if (mixPumpScheduler_.ready())
        mixPumpScheduler_.signalStop();
    { std::lock_guard<std::mutex> lock(superMutex_); superWake_ = true; }
    superCv_.notify_all();
    if (supervisor_.joinable()) supervisor_.join();

    if (mixPump_.joinable()) mixPump_.join();
    mixPumpScheduler_.close();
    // The supervisor is joined, so this final retained observation cannot be
    // spliced with its periodic sample. Capture identity still exists here.
    recordDiagnostics();
    for (auto& render : renders_) render->stop();
    for (auto& ch : channels_) ch->stream.stop();
    devices_.stop();
    {
        std::lock_guard<std::mutex> lock(superMutex_);
        superWake_ = false;
        monitoringWake_ = false;
    }
}

void AudioEngine::setMonitoring(bool enabled) {
    if (!running()) return;
    enabled = enabled && outputCount_ > 0;
    uint64_t state = monitoringState_.load(std::memory_order_acquire);
    for (;;) {
        if (((state & 1u) != 0) == enabled) return;
        const uint64_t next = (state & ~uint64_t{1}) + 2 + (enabled ? 1 : 0);
        if (monitoringState_.compare_exchange_weak(state, next,
                std::memory_order_acq_rel, std::memory_order_acquire)) break;
    }
    LOG_INFO("engine: monitoring %s requested; generation=%llu", enabled ? "on" : "off",
             static_cast<unsigned long long>((state & ~uint64_t{1}) + 2 + (enabled ? 1 : 0)));
    // Peak hand-offs are atomics, so clearing them here cannot block either
    // worker. The pump also clears paused outputs at its next block boundary.
    for (auto& peak : outputPeaks_) { peak.l.take(); peak.r.take(); }
    { std::lock_guard<std::mutex> lock(superMutex_); monitoringWake_ = true; }
    superCv_.notify_one();
}

void AudioEngine::OutputGate::renderMix(float* dst, uint32_t frames) noexcept {
    const uint64_t state = engine_.monitoringState_.load(std::memory_order_acquire);
    if (engine_.running() && (state & 1u) != 0 &&
        engine_.outputReadyState_[output_].load(std::memory_order_acquire) == state) {
        engine_.outputBuses_[output_]->renderMix(dst, frames);
        // A pause during this callback must not return the buffered old mix.
        if (engine_.monitoringState_.load(std::memory_order_acquire) == state &&
            engine_.running()) return;
    }
    std::fill_n(dst, static_cast<size_t>(frames) * 2, 0.0f);
}

void AudioEngine::OutputGate::onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept {
    engine_.outputBuses_[output_]->onRenderFormat(sampleRate, blockFrames);
}

void AudioEngine::OutputGate::onRenderPeriod(uint32_t periodFrames) noexcept {
    engine_.outputBuses_[output_]->onRenderPeriod(periodFrames);
}

void AudioEngine::setGain(int channel, float g) noexcept {
    if (channel < 0 || size_t(channel) >= sourceCount_) return;
    channels_[channel]->gain.store(std::clamp(g, 0.0f, 4.0f), std::memory_order_relaxed);
}

void AudioEngine::setMuted(int channel, bool m) noexcept {
    if (channel < 0 || size_t(channel) >= sourceCount_) return;
    std::lock_guard<std::mutex> lock(configMutex_);
    config_.sources[channel].muted = m;
    channels_[channel]->muted.store(m || !config_.sources[channel].enabled, std::memory_order_relaxed);
}

void AudioEngine::setEnabled(int channel, bool enabled) {
    if (channel < 0 || size_t(channel) >= sourceCount_) return;
    std::lock_guard<std::mutex> lock(configMutex_);
    auto& source = config_.sources[channel];
    source.enabled = enabled;
    channels_[channel]->muted.store(source.muted || !enabled);
    if (!running()) return;
    if (enabled) channels_[channel]->stream.start(devices_, source.kind == SourceKind::Application
        ? DeviceRef{L"", source.processPath} : DeviceRef{source.deviceId, source.deviceNameMatch}, "source enabled");
    else {
        LOG_INFO("engine: source %d disabled; stopping capture", channel);
        channels_[channel]->stream.stop();
    }
}

void AudioEngine::setOutputGain(size_t output, float g) noexcept {
    if (output >= outputCount_) return;
    outputGains_[output].store(std::clamp(g, 0.0f, 4.0f), std::memory_order_relaxed);
}

void AudioEngine::setOutputMuted(size_t output, bool muted) noexcept {
    if (output >= outputCount_) return;
    outputMuted_[output].store(muted, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Fixed-rate mix pump. Everything below this line must be allocation-free and
// lock-free.
// ---------------------------------------------------------------------------

void AudioEngine::onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept {
    // Called by start() before any audio worker begins, so the defensive
    // allocation below cannot cause a dropout. This is the only place the
    // engine is allowed to allocate after construction.
    renderRate_.store(sampleRate, std::memory_order_release);
    renderBlock_.store(blockFrames, std::memory_order_release);

    // kMaxBlockFrames covers every period a sane device reports, but a device
    // that asks for a bigger block would otherwise silently get a short mix.
    // Grow to fit rather than truncate.
    const size_t needed = static_cast<size_t>(blockFrames) * 2;
    for (auto& up : channels_) {
        if (up->scratch.size() < needed) up->scratch.assign(needed, 0.0f);
    }

    // The setpoint is NOT computed here. Ring depth is measured in capture
    // frames, and the capture device's rate is not known until it opens (and
    // can differ per channel), so renderMix derives it once each source rate
    // is known. Setting it from the mix rate here would hold a 96 kHz
    // source at half the intended depth.
    for (size_t i = 0; i < sourceCount_; ++i) {
        Channel& ch = *channels_[i];
        ch.lastSrcRate  = 0;             // force a recompute on the next block
        ch.lastBufferMs = 0;
        ch.targetDepth  = 0.0;
        ch.targetOut.store(0, std::memory_order_relaxed);
        ch.rateOut.store(0, std::memory_order_relaxed);
        ch.epochOut.store(ch.stream.epoch(), std::memory_order_relaxed);
        ch.primingOut.store(true, std::memory_order_relaxed);
        ch.correctionPpmOut.store(0, std::memory_order_relaxed);
        ch.depthOut.store(0, std::memory_order_relaxed);
        ch.resampler.reset();
        ch.priming      = true;
        ch.presence.configure(uint32_t(double(sampleRate) * kFadeSeconds));
        ch.presence.reset(0.0f);
        ch.timelineBreakPending = false;
        ch.lastObservedDepth = 0;
        ch.smoothedGain = ch.muted.load(std::memory_order_relaxed) ? 0.0f : ch.gain.load(std::memory_order_relaxed);
        // Ring holds stale audio from before the rebuild; start clean.
        ch.stream.ring().dropAllFromConsumer();
    }
}

void AudioEngine::renderMix(float* dst, uint32_t frames) noexcept {
#if defined(AUDIOMON_TRACE_FIRST_MIX)
    // One-shot breadcrumb: proves the first mix block ran to completion.
    static std::atomic<bool> traced{false};
    const bool firstMix = !traced.exchange(true, std::memory_order_relaxed);
    if (firstMix) LOG_INFO("mix: first block, %u frames", frames);
#endif
    const uint32_t rate = renderRate_.load(std::memory_order_relaxed);

    // Defensive backstop only. onRenderFormat sizes every scratch buffer to
    // the render block, so this cannot trigger -- but truncating is the right
    // failure if the invariant is ever broken, since overrunning is not.
    const uint32_t capacity = static_cast<uint32_t>(channels_[0]->scratch.size() / 2);
    if (frames > capacity) frames = capacity;

    std::fill_n(dst, static_cast<size_t>(frames) * 2, 0.0f);

    // ~5ms. Slow enough to be inaudible, fast enough that unmuting feels
    // instant.
    const float gainCoef = smoothingCoef(kFadeSeconds, rate);

    for (size_t i = 0; i < sourceCount_; ++i) {
        Channel& ch = *channels_[i];
        StereoRing& ring = ch.stream.ring();
        uint32_t made = 0;
        bool forceFadeOut = false;

        const auto resetTimeline = [&] {
            ring.dropAllFromConsumer();
            ch.rate.reset();
            ch.resampler.reset();
            ch.priming = true;
            ch.lastObservedDepth = 0;
            ch.timelineBreakPending = false;
            ch.depthOut.store(0, std::memory_order_relaxed);
            ch.primingOut.store(true, std::memory_order_relaxed);
        };

        // A timeline break (discontinuity, reconnect, overflow drop) makes the
        // controller's history meaningless.
        const uint32_t ep = ch.stream.epoch();
        if (ep != ch.lastEpoch) {
            ch.lastEpoch = ep;
            // If audio was already present, retain the old timeline until the
            // configured fade has completed. Short render periods can require
            // several calls to consume one fade length.
            if (ch.presence.value() > 0.0f && ring.depth() >= 4) {
                ch.timelineBreakPending = true;
            } else {
                resetTimeline();
            }
        }

        const bool flowing = ch.stream.flowing();
        const StreamState state = ch.stream.state();
        const uint32_t observedDepth = ring.depth();
        const bool stoppingFade = (ch.timelineBreakPending || state != StreamState::Running) &&
                                  ch.presence.value() > 0.0f && observedDepth >= 4;
        // Loopback stops delivering packets when playback stops, but the ring
        // may still hold the buffer's entire tail. A stopped/failed source also
        // gets one fade of real samples rather than being cut off immediately.
        const bool live = stoppingFade ||
                          (!ch.timelineBreakPending && state == StreamState::Running &&
                           (flowing || observedDepth > 0));

        if (live) {
            uint32_t depth = observedDepth;
            ch.depthOut.store(depth, std::memory_order_relaxed);

            const uint32_t srcRate = ch.stream.sampleRate();
            if (!stoppingFade)
                ch.baseRatio = (srcRate && rate) ? double(srcRate) / double(rate) : 1.0;

            const uint32_t bufMs = bufferMillis_.load(std::memory_order_relaxed);

            if (!stoppingFade && srcRate && srcRate != ch.lastSrcRate) {
                // New clock: full reconfigure. Setpoint and controller are
                // denominated in capture frames, matching the ring. dt is real
                // elapsed time set by the fixed-rate pump; the capture clock
                // remains independent.
                ch.lastSrcRate  = srcRate;
                ch.lastBufferMs = bufMs;
                ch.targetDepth  = double(srcRate) * double(bufMs) / 1000.0;
                const double dt = rate ? double(frames) / double(rate) : 0.01;
                ch.rate.configure(double(srcRate), ch.targetDepth, dt);
                ch.priming = true;
            } else if (!stoppingFade && srcRate && bufMs != ch.lastBufferMs) {
                // Same clock, new setpoint: the user moved the buffer slider.
                // setTarget rather than configure -- a reconfigure resets the
                // controller and re-primes, which would drop audio for a
                // setting the user is likely to nudge repeatedly. The depth
                // migrates under the existing slew limit instead.
                ch.lastBufferMs = bufMs;
                ch.targetDepth  = double(srcRate) * double(bufMs) / 1000.0;
                ch.rate.setTarget(ch.targetDepth);
            }
            if (!stoppingFade && ch.targetDepth <= 0.0) { ch.priming = true; }

            // A descheduled capture worker can return a batch of old WASAPI
            // packets after this channel has already starved. Re-priming from
            // the oldest packet would retain that whole stall as extra delay
            // until the very slow drift controller catches up. Keep only the
            // newest target-sized tail; silence has already ended the old
            // envelope, so the normal fade-in makes this transition safe.
            // Steady playback and live buffer-slider migrations are untouched.
            if (!stoppingFade && ch.priming && ch.presence.value() <= 0.0f &&
                ch.targetDepth > 0.0) {
                const auto keep = static_cast<uint32_t>(std::ceil(ch.targetDepth));
                const auto reserve = static_cast<uint32_t>(std::ceil(
                    double(frames) * ch.baseRatio * RateController::kMaxRatio)) + 4;
                if (depth > keep && depth - keep > reserve) {
                    const uint32_t available = ring.beginRead();
                    const uint32_t trim = available > keep ? available - keep : 0;
                    ring.endRead(trim);
                    ch.latencyTrimmed.fetch_add(trim, std::memory_order_relaxed);
                    ch.rate.reset();
                    ch.resampler.reset();
                    depth = available - trim;
                    ch.depthOut.store(depth, std::memory_order_relaxed);
                }
            }

            // Priming: wait for the ring to reach the setpoint before drawing
            // from it, so we do not immediately starve. This is also the path
            // taken every time a silent loopback endpoint starts producing
            // again.
            // A short sound may end before it fills the target buffer. Once
            // capture is idle, waiting for more packets would strand it forever.
            // Do not mistake frames published by a producer that has just
            // resumed for an old idle tail. The idle bypass is valid only once
            // the same depth has survived a complete mix block.
            const bool stableIdleTail = !stoppingFade && !flowing && depth >= 4 &&
                                        depth == ch.lastObservedDepth;
            ch.lastObservedDepth = depth;
            if (!stoppingFade && ch.priming &&
                (depth >= ch.targetDepth || stableIdleTail)) {
                ch.priming = false;
                ch.rate.reset();
            }

            if (stoppingFade) {
                const uint32_t requested = std::min(frames, ch.presence.framesUntilSilent());
                const double ratio = ch.baseRatio * ch.rate.ratio();
                made = ch.resampler.produce(ring, ch.scratch.data(), requested, ratio);
                forceFadeOut = made > 0;
            } else if (!ch.priming) {
                // The controller only ever corrects for drift; the base ratio
                // handles a genuine rate difference and is not part of the
                // correction the clamp applies to.
                const double ratio = ch.baseRatio *
                    (flowing ? ch.rate.update(double(depth)) : ch.rate.ratio());
                ch.ratioOut.store(ratio, std::memory_order_relaxed);
                made = ch.resampler.produce(ring, ch.scratch.data(), frames, ratio);
                if (made < frames) {
                    ch.priming = true;
                    if (flowing) {
                        ch.shortfallFrames.fetch_add(frames - made, std::memory_order_relaxed);
                        ch.shortfallEvents.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        } else {
            ch.depthOut.store(observedDepth, std::memory_order_relaxed);
            ch.lastObservedDepth = observedDepth;
            // Hold the converged ratio while the source is idle. Letting the
            // controller integrate against a permanently empty ring would wind
            // it to an extreme that is wildly wrong when audio resumes.
            ch.priming = true;
        }

        // Scalar mirrors only: no diagnostic locks, allocation, formatting or
        // clocks in the audio path. Epoch equality lets observers reject the
        // short interval before this pump has acknowledged a capture restart.
        ch.targetOut.store(static_cast<uint32_t>(ch.targetDepth), std::memory_order_relaxed);
        ch.rateOut.store(ch.lastSrcRate, std::memory_order_relaxed);
        ch.primingOut.store(ch.priming, std::memory_order_relaxed);
        ch.correctionPpmOut.store((ch.rate.ratio() - 1.0) * 1000000.0, std::memory_order_relaxed);
        ch.epochOut.store(ch.lastEpoch, std::memory_order_release);

        if (made < frames) {
            std::fill_n(ch.scratch.data() + static_cast<size_t>(made) * 2,
                        static_cast<size_t>(frames - made) * 2, 0.0f);
        }

        // Fade the channel in and out rather than switching, so a silent gap
        // does not click at either edge.
        //
        // The fade-OUT has to land exactly where the audio stops. produce()
        // reports that it filled `made` frames, so the ramp is scheduled to
        // reach zero at that index. Simply setting a target of 0 for the whole
        // block and letting a smoother decay from frame 0 does not work: the
        // envelope is still near 1.0 when the samples run out, so the cut is
        // just as abrupt as no fade at all -- which defeats the entire point
        // of having one.
        ch.presence.beginBlock(made, frames, forceFadeOut);

        const float gainTarget = ch.muted.load(std::memory_order_relaxed)
                                     ? 0.0f
                                     : ch.gain.load(std::memory_order_relaxed);

        // Disabled/disconnected and idle loopback sources need no per-sample
        // gain, envelope, sum or peak work. beginBlock(0, ...) has already
        // landed the presence envelope at zero, so snapping the gain while
        // silent cannot click; returning audio still fades in normally. The
        // stream/ring/controller bookkeeping above must run even when silent.
        if (made == 0 && ch.presence.value() <= 0.0f) {
            if (ch.timelineBreakPending || state != StreamState::Running)
                resetTimeline();
            ch.smoothedGain = gainTarget;
            ch.peak.l.publish(0.0f);
            ch.peak.r.publish(0.0f);
            continue;
        }

        float g = ch.smoothedGain;
        float peakL = 0.0f, peakR = 0.0f;

        for (uint32_t f = 0; f < frames; ++f) {
            g += (gainTarget - g) * gainCoef;
            const float amp = g * ch.presence.next(f);
            const float l = ch.scratch[f * 2]     * amp;
            const float r = ch.scratch[f * 2 + 1] * amp;
            dst[f * 2]     += l;
            dst[f * 2 + 1] += r;
            const float al = std::fabs(l), ar = std::fabs(r);
            if (al > peakL) peakL = al;
            if (ar > peakR) peakR = ar;
        }

        if ((ch.timelineBreakPending || state != StreamState::Running) &&
            ch.presence.value() <= 0.0f) {
            resetTimeline();
        }
        ch.smoothedGain = g;

        // Post-fader, post-mute, like the OBS mixer: muting drops the meter.
        ch.peak.l.publish(peakL);
        ch.peak.r.publish(peakR);
    }

    // Channel-mode conversion is part of the canonical mix. Per-destination
    // trim and mute are applied while the pump fans this block into each
    // OutputBus, so one output can never silence or scale another.
    const bool mono = mono_.load(std::memory_order_relaxed);
    const float modeStep = 1.0f / std::max(1.0f, float(rate) * 0.005f);
    for (uint32_t f = 0; f < frames; ++f) {
        float l = dst[f * 2];
        float r = dst[f * 2 + 1];
        mixMode_.process(l, r, mono, modeStep);
        dst[f * 2]     = l;
        dst[f * 2 + 1] = r;
    }

#if defined(AUDIOMON_TRACE_FIRST_MIX)
    if (firstMix) LOG_INFO("mix: first block completed");
#endif
}

void AudioEngine::mixPumpMain() {
    enableDenormalFlush();
    MmcssRegistration mmcss;
    if (!mmcss.acquire(L"Pro Audio", AVRT_PRIORITY_CRITICAL)) {
        LOG_WARN("mix pump: MMCSS 'Pro Audio' unavailable (%lu)", mmcss.lastError());
    }

    using clock = std::chrono::steady_clock;
    constexpr auto period = std::chrono::milliseconds(10);
    auto next = clock::now();
    bool fallbackFailureLogged = false;

    while (!quit_.load(std::memory_order_relaxed)) {
        const auto started = clock::now();
        if (started - next >= period) {
            const uint64_t missed = static_cast<uint64_t>((started - next) / period);
            handleMixPumpDiscontinuity(missed);
            // Skip the stale clock slots. Rendering them in a burst would move
            // the capture backlog into every output ring and preserve the
            // latency spike we are trying to remove.
            next = started;
        }

        pumpBlock();

        next += period;
        const bool wasDegraded = mixPumpScheduler_.degraded();
        const auto wait = mixPumpScheduler_.waitFor(next - clock::now());
        if (!wasDegraded && mixPumpScheduler_.degraded()) {
            LOG_WARN("mix pump: waitable timer failed (%lu); continuing with "
                     "cancellable timeout scheduling",
                     mixPumpScheduler_.lastError());
        }
        if (wait == MixPumpScheduler::WaitResult::Stop) break;
        if (wait == MixPumpScheduler::WaitResult::Failed) {
            if (!fallbackFailureLogged) {
                LOG_ERR("mix pump: timeout scheduler wait failed (%lu); using "
                        "bounded sleep",
                        mixPumpScheduler_.lastError());
                fallbackFailureLogged = true;
            }
            // A valid event wait should not fail. Keep monitoring alive even
            // if the kernel handle is unexpectedly rejected; the outer quit
            // check still bounds shutdown by one coarse period.
            std::this_thread::sleep_for(period);
        }
    }
}

void AudioEngine::pumpBlock() noexcept {
    // Source metering always uses this same capture/resample path, including
    // while forwarding is paused. No capture clock, ring or envelope restarts.
    renderMix(mixPumpBuffer_.data(), kMixBlockFrames);

    const uint64_t state = monitoringState_.load(std::memory_order_acquire);
    const bool forwarding = (state & 1u) != 0;
    const float gainCoef = smoothingCoef(kFadeSeconds, kMixSampleRate);
    size_t visualOutput = outputCount_;
    float visualPeak = -1.0f;
    for (size_t output = 0; output < outputCount_; ++output) {
        const float target = outputMuted_[output].load(std::memory_order_relaxed)
                                 ? 0.0f
                                 : outputGains_[output].load(std::memory_order_relaxed);
        if (!forwarding || outputReadyState_[output].load(std::memory_order_acquire) != state ||
            renders_[output]->state() != StreamState::Running) {
            // Do not fill a paused branch or one without an active consumer.
            // Each resumed renderer re-primes from the current mix only.
            smoothedOutputGains_[output] = target;
            outputPeaks_[output].l.take();
            outputPeaks_[output].r.take();
            continue;
        }
        float gain = smoothedOutputGains_[output];
        float peakL = 0.0f, peakR = 0.0f;
        auto& busScratch = outputScratch_[output];
        for (uint32_t frame = 0; frame < kMixBlockFrames; ++frame) {
            gain += (target - gain) * gainCoef;
            const float l = mixPumpBuffer_[frame * 2] * gain;
            const float r = mixPumpBuffer_[frame * 2 + 1] * gain;
            busScratch[frame * 2] = l;
            busScratch[frame * 2 + 1] = r;
            peakL = std::max(peakL, std::fabs(l));
            peakR = std::max(peakR, std::fabs(r));
        }
        smoothedOutputGains_[output] = gain;
        outputPeaks_[output].l.publish(peakL);
        outputPeaks_[output].r.publish(peakR);
        outputBuses_[output]->publish(busScratch.data(), kMixBlockFrames, 1.0f, false);
        const float branchPeak = std::max(peakL, peakR);
        if (branchPeak > visualPeak) {
            visualPeak = branchPeak;
            visualOutput = output;
        }
    }

    // Preserve the Live Mix's loudest-output visualization during forwarding.
    // The UI drains this ring while paused too, so silence stays current.
    const bool stillForwarding = forwarding &&
        monitoringState_.load(std::memory_order_acquire) == state;
    const uint32_t visualCount = std::min(kMixBlockFrames, visualSamples_.beginWrite());
    for (uint32_t frame = 0; frame < visualCount; ++frame) {
        const bool show = stillForwarding && visualOutput < outputCount_;
        visualSamples_.writeFrame(frame,
            show ? outputScratch_[visualOutput][frame * 2] : 0.0f,
            show ? outputScratch_[visualOutput][frame * 2 + 1] : 0.0f);
    }
    visualSamples_.endWrite(visualCount);
    // Release only after every old-generation publish has finished. The
    // supervisor uses this hand-off before reopening a render consumer.
    pumpObservedState_.store(state, std::memory_order_release);
}

void AudioEngine::handleMixPumpDiscontinuity(uint64_t missedPeriods) noexcept {
    if (missedPeriods == 0) return;
    mixPumpMissedPeriods_.fetch_add(missedPeriods, std::memory_order_relaxed);

    // A whole canonical block was missed. Keeping every frame captured during
    // that pause would turn a scheduler hiccup into persistent monitoring
    // latency: the deliberately slow drift controllers can take tens of
    // seconds to remove a step that large. The pump is the sole consumer of
    // these rings, so trim the oldest excess back to the normal latency target
    // in O(1). Preserve the newest target-sized tail to avoid a full 20-250 ms
    // rebuffer after an otherwise brief scheduler hiccup.
    for (size_t i = 0; i < sourceCount_; ++i) {
        Channel& ch = *channels_[i];
        StereoRing& ring = ch.stream.ring();
        const uint32_t epoch = ch.stream.epoch();
        if (epoch != ch.lastEpoch) {
            // The capture producer itself reported a gap/overflow, so no
            // contiguous newest tail can be identified safely.
            ring.dropAllFromConsumer();
        } else {
            const uint32_t available = ring.beginRead();
            const uint32_t keep = ch.targetDepth > 0.0
                ? std::min(available, static_cast<uint32_t>(std::ceil(ch.targetDepth)))
                : 0;
            ring.endRead(available - keep);
        }
        ch.rate.reset();
        ch.resampler.reset();
        ch.priming = true;
        ch.presence.reset(0.0f);
        ch.timelineBreakPending = false;
        ch.lastObservedDepth = 0;
        ch.lastEpoch = epoch;
        ch.depthOut.store(ring.depth(), std::memory_order_relaxed);
        ch.ratioOut.store(ch.baseRatio, std::memory_order_relaxed);
    }

    // Old canonical frames already queued at the outputs belong to the broken
    // timeline. Each consumer fades/drops them and independently re-primes.
    for (size_t i = 0; i < outputCount_; ++i)
        outputBuses_[i]->requestReset();
}

// ---------------------------------------------------------------------------
// Supervisor thread: every device rebuild happens here, never on the render
// thread and never inside a COM notification callback.
// ---------------------------------------------------------------------------

void AudioEngine::requestRebuild() {
    { std::lock_guard<std::mutex> lock(superMutex_); superWake_ = true; }
    superCv_.notify_one();
}

bool AudioEngine::synchronizeOutputs(uint64_t state) {
    for (auto& ready : outputReadyState_) ready.store(0, std::memory_order_release);
    if (outputCount_ == 0) return true;
    // A previous pump block may have read the old forwarding generation just
    // before the click. Let that block finish before a restarted consumer
    // clears its ring; otherwise its late publish could replay old audio.
    while (pumpObservedState_.load(std::memory_order_acquire) != state) {
        if (quit_.load(std::memory_order_relaxed) ||
            monitoringState_.load(std::memory_order_acquire) != state) return false;
        std::unique_lock<std::mutex> lock(superMutex_);
        superCv_.wait_for(lock, std::chrono::milliseconds(2), [this, state] {
            return quit_.load(std::memory_order_relaxed) ||
                monitoringState_.load(std::memory_order_acquire) != state;
        });
    }
    for (size_t i = 0; i < outputCount_; ++i) {
        if (quit_.load(std::memory_order_relaxed) ||
            monitoringState_.load(std::memory_order_acquire) != state) return false;
        std::lock_guard<std::mutex> lock(configMutex_);
        LOG_INFO("supervisor: output %zu synchronizing monitoring generation=%llu (%s)", i,
                 static_cast<unsigned long long>(state), (state & 1u) ? "on" : "off");
        renders_[i]->stop();
        outputBuses_[i]->requestReset();
        if ((state & 1u) == 0) continue;
        const auto& output = config_.outputAt(i);
        outputReadyState_[i].store(state, std::memory_order_release);
        // RenderStream calls onRenderFormat before exposing Running; this
        // drops the old ring after the old producer has acknowledged pause.
        try {
            renders_[i]->start(devices_, {output.deviceId, output.deviceNameMatch},
                               outputGates_[i].get(), config_.exclusiveOutput,
                               "monitoring generation changed");
        } catch (const std::exception& e) {
            LOG_ERR("supervisor: output worker start failed: %s", e.what());
            setMonitoring(false);
            return false;
        } catch (...) {
            LOG_ERR("supervisor: output worker start failed");
            setMonitoring(false);
            return false;
        }
    }
    return true;
}

void AudioEngine::supervisorMain() {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsUninit = SUCCEEDED(coHr);

    std::array<uint32_t, kMaxSources> captureFailures{};
    std::array<std::chrono::steady_clock::time_point, kMaxSources> captureRetryAt{};
    std::array<uint32_t, kMaxOutputs> outputFailures{};
    std::array<std::chrono::steady_clock::time_point, kMaxOutputs> outputRetryAt{};
    uint64_t handledMonitoringState = 0;
    auto diagnosticAt = std::chrono::steady_clock::now();

    const auto failSupervisor = [this](const char* error) {
        LOG_ERR("supervisor: worker restart failed: %s", error);
        // Publish failure before cleanup so output callbacks are immediately
        // silent. Any UI restart enters stopLocked() and joins this supervisor
        // before mutating streams, counts, rings or device ownership.
        running_.store(false, std::memory_order_release);
        monitoringState_.fetch_and(~uint64_t{1}, std::memory_order_acq_rel);
        quit_.store(true, std::memory_order_relaxed);
        mixPumpScheduler_.signalStop();
        std::lock_guard<std::mutex> lock(configMutex_);
        for (auto& render : renders_) render->stop();
        for (auto& channel : channels_) channel->stream.stop();
        devices_.stop();
    };

    try {
        while (!quit_.load(std::memory_order_relaxed)) {
            bool woken = false;
            bool monitoringWoken = false;
            {
                std::unique_lock<std::mutex> lock(superMutex_);
                superCv_.wait_until(lock, std::min(diagnosticAt, std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kSupervisorPollMs)),
                    [this] { return superWake_ || monitoringWake_ || quit_.load(std::memory_order_relaxed); });
                woken = superWake_;
                monitoringWoken = monitoringWake_;
                superWake_ = false;
                monitoringWake_ = false;
            }
            if (quit_.load(std::memory_order_relaxed)) break;

            if (woken && !monitoringWoken) {
                // Coalesce the burst a driver reinstall produces.
                std::unique_lock<std::mutex> lock(superMutex_);
                superCv_.wait_for(lock, std::chrono::milliseconds(kRebuildDebounceMs),
                    [this] { return monitoringWake_ || quit_.load(std::memory_order_relaxed); });
                superWake_ = false;
                monitoringWake_ = false;
            }
            if (woken) {
                captureFailures.fill(0);
                captureRetryAt.fill({});
                outputFailures.fill(0);
                outputRetryAt.fill({});
            }
            if (quit_.load(std::memory_order_relaxed)) break;

            const uint64_t monitoringState = monitoringState_.load(std::memory_order_acquire);
            if (monitoringState != handledMonitoringState) {
                if (!synchronizeOutputs(monitoringState)) continue;
                handledMonitoringState = monitoringState;
                outputFailures.fill(0);
                outputRetryAt.fill({});
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= diagnosticAt) {
                recordDiagnostics(); // includes paused metering and tray operation
                diagnosticAt = now + std::chrono::milliseconds(DiagnosticHistory::kSampleMillis);
            }

            // Serialize reading each selection AND restarting it with UI changes.
            // Releasing the lock in between lets a stale restart undo a new choice.
            for (size_t i = 0; i < sourceCount_; ++i) {
                std::lock_guard<std::mutex> lock(configMutex_);
                const auto& source = config_.sources[i];
                auto& ch = *channels_[i];
                ch.stream.flushDiagnostics(); // non-real-time, including while forwarding is paused
                if (!source.enabled || ch.stream.state() == StreamState::Running) {
                    captureFailures[i] = 0;
                    captureRetryAt[i] = {};
                    continue;
                }
                if (!shouldRestart(ch.stream.state(), ch.stream.retryable(), woken) ||
                    (!woken && now < captureRetryAt[i])) continue;
                ch.stream.start(devices_, source.kind == SourceKind::Application
                    ? DeviceRef{L"", source.processPath} : DeviceRef{source.deviceId, source.deviceNameMatch},
                    woken ? "supervisor retry after device notification" : "supervisor retry after stream failure/stop");
                ++captureFailures[i];
                captureRetryAt[i] = now + std::chrono::milliseconds(
                    retryBackoffMillis(captureFailures[i]));
            }

            if (quit_.load(std::memory_order_relaxed)) break;
            if ((monitoringState & 1u) == 0) continue;
            for (size_t i = 0; i < outputCount_; ++i) {
                if (monitoringState_.load(std::memory_order_acquire) != monitoringState) break;
                std::lock_guard<std::mutex> outputLock(configMutex_);
                auto& render = *renders_[i];
                const auto& output = config_.outputAt(i);

                // Passivity check. An endpoint that becomes a system default must
                // be reopened so RenderStream's exclusive-mode gate can release it
                // without disturbing the other destinations.
                if (render.state() == StreamState::Running && render.exclusive() &&
                    devices_.isDefaultForAnyRole(render.resolvedId())) {
                    LOG_WARN("supervisor: output %zu became a system default; reopening shared", i);
                    outputBuses_[i]->requestReset();
                    render.start(devices_, {output.deviceId, output.deviceNameMatch},
                                 outputGates_[i].get(), config_.exclusiveOutput,
                                 "output became a system default; reopen shared");
                    outputFailures[i] = 0;
                    outputRetryAt[i] = {};
                }

                if (render.state() == StreamState::Running) {
                    outputFailures[i] = 0;
                    outputRetryAt[i] = {};
                } else if (shouldRestart(render.state(), render.wantsRetry(), woken) &&
                           (woken || now >= outputRetryAt[i])) {
                    LOG_INFO("supervisor: restarting output %zu (%s)", i,
                             render.lastError().c_str());
                    outputBuses_[i]->requestReset();
                    render.start(devices_, {output.deviceId, output.deviceNameMatch},
                                 outputGates_[i].get(), config_.exclusiveOutput,
                                 woken ? "supervisor retry after device notification" : "supervisor retry after stream failure/stop");
                    ++outputFailures[i];
                    outputRetryAt[i] = now + std::chrono::milliseconds(
                        retryBackoffMillis(outputFailures[i]));
                }
            }
        }

    } catch (const std::exception& e) {
        failSupervisor(e.what());
    } catch (...) {
        failSupervisor("unknown exception");
    }

    if (needsUninit) CoUninitialize();
}

// ---------------------------------------------------------------------------

DiagnosticSample AudioEngine::collectDiagnostics() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return collectDiagnosticsLocked();
}

DiagnosticSample AudioEngine::collectDiagnosticsLocked() const {
    DiagnosticSample sample;
    sample.session = diagnosticSession_;
    sample.elapsedMillis = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - diagnosticOrigin_).count());
    sample.utcMillis = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    sample.running = running();
    sample.monitoringGeneration = monitoringState_.load(std::memory_order_acquire);
    sample.monitoring = sample.running && (sample.monitoringGeneration & 1u) != 0;
    sample.bufferMillis = bufferMillis_.load(std::memory_order_relaxed);
    sample.pumpMissedPeriods = mixPumpMissedPeriods();
    // Before the first start there is no configured audio session, regardless
    // of the legacy default value of outputCount_. After stop retain topology.
    if (!sample.session) return sample;
    sample.sourceCount = sourceCount_;
    sample.outputCount = outputCount_;
    for (size_t i = 0; i < sourceCount_; ++i) {
        const auto& ch = *channels_[i];
        auto& s = sample.sources[i];
        s.state = ch.stream.state();
        s.epoch = ch.stream.epoch();
        const auto timeline = ch.stream.timelineStats();
        s.captureStartRequests = timeline.startRequests;
        s.captureInitialDiscontinuities = timeline.initialDiscontinuities;
        s.captureDiscontinuities = timeline.discontinuities;
        s.captureOverflowEvents = timeline.overflowEvents;
        s.valid = sample.running && config_.sources[i].enabled && s.state == StreamState::Running &&
                  s.epoch == ch.epochOut.load(std::memory_order_acquire);
        s.flowing = ch.stream.flowing();
        s.priming = ch.primingOut.load(std::memory_order_relaxed);
        s.muted = ch.muted.load(std::memory_order_relaxed);
        s.nativeRate = ch.stream.sampleRate();
        s.queueRate = ch.rateOut.load(std::memory_order_relaxed);
        s.queueFrames = ch.depthOut.load(std::memory_order_relaxed);
        s.targetFrames = ch.targetOut.load(std::memory_order_relaxed);
        // Ring capacity is immutable after configure, while this lifecycle is running.
        s.capacityFrames = ch.stream.ringCapacityFrames();
        s.correctionPpm = ch.correctionPpmOut.load(std::memory_order_relaxed);
        s.overflowFrames = config_.sources[i].enabled ? ch.stream.droppedFrames() : 0;
        s.trimmedFrames = ch.latencyTrimmed.load(std::memory_order_relaxed);
        s.starvedFrames = ch.shortfallFrames.load(std::memory_order_relaxed);
        s.starvationEvents = ch.shortfallEvents.load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < outputCount_; ++i) {
        const auto& render = *renders_[i];
        const auto& bus = *outputBuses_[i];
        auto& s = sample.outputs[i];
        s.state = render.state();
        s.valid = sample.monitoring && s.state == StreamState::Running &&
            outputReadyState_[i].load(std::memory_order_acquire) == sample.monitoringGeneration;
        s.flowing = s.valid;
        s.priming = bus.priming();
        s.muted = outputMuted_[i].load(std::memory_order_relaxed);
        s.exclusive = render.exclusive();
        s.epoch = bus.diagnosticEpoch();
        s.nativeRate = bus.renderSampleRate();
        s.queueRate = kMixSampleRate;
        s.queueFrames = bus.diagnosticDepthFrames();
        s.targetFrames = bus.effectiveTargetFrames();
        s.capacityFrames = bus.capacityFrames();
        s.blockFrames = render.blockFrames();
        s.correctionPpm = s.nativeRate
            ? (bus.resamplingRatio() * double(s.nativeRate) / double(kMixSampleRate) - 1.0) * 1000000.0 : 0;
        s.overflowFrames = bus.overflowFrames();
        s.trimmedFrames = bus.trimmedFrames();
        s.starvedFrames = bus.starvedFrames();
        s.starvationEvents = bus.starvationEvents();
        s.underrunEvents = render.underruns();
        s.latencyCorrections = bus.latencyCorrections();
    }
    return sample;
}

void AudioEngine::recordDiagnostics(bool runtime) noexcept {
    try {
        DiagnosticSample sample;
        std::string devices;
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            sample = collectDiagnosticsLocked();
            // The initial pre-open observation must not retain the preceding
            // session's format/identity. Periodic and final samples replace
            // one bounded per-session description, not every history row.
            if (runtime)
                devices = diagnosticDevicesLocked();
        }
        diagnostics_.record(sample, std::move(devices));
    }
    catch (...) {} // Optional diagnostics must never stop the mixer.
}

std::string AudioEngine::diagnosticDevicesLocked(bool runtime) const {
    std::ostringstream out;
    const auto retain = [](DiagnosticDeviceCache& cache, const ChannelConfig& configured,
                           const StreamDiagnosticInfo& info, StreamState state, uint32_t processId) {
        if (cache.selection.id != configured.deviceId ||
            cache.selection.nameMatch != configured.deviceNameMatch ||
            cache.processPath != configured.processPath) {
            cache = {};
            cache.selection = {configured.deviceId, configured.deviceNameMatch};
            cache.processPath = configured.processPath;
        }
        if (state != StreamState::Stopped) {
            if (!info.name.empty()) cache.info.name = info.name;
            if (!info.id.empty()) cache.info.id = info.id;
            if (!info.format.empty()) cache.info.format = info.format;
            if (!info.error.empty() || state == StreamState::Running) cache.info.error = info.error;
            if (processId) cache.processId = processId;
        }
    };
    for (size_t i = 0; i < sourceCount_; ++i) {
        const auto& configured = config_.sources[i];
        out << "Source " << i << ": " << configured.label << "; enabled=" << configured.enabled
            << "; kind=" << static_cast<int>(configured.kind) << "; selection="
            << toUtf8(configured.deviceNameMatch) << "; pinned ID=" << toUtf8(configured.deviceId)
            << "; application=" << toUtf8(configured.processPath);
        if (!runtime) { out << '\n'; continue; }
        const auto state = channels_[i]->stream.state();
        const auto live = state == StreamState::Stopped ? StreamDiagnosticInfo{} : channels_[i]->stream.diagnosticInfo();
        auto& cache = diagnosticSources_[i];
        retain(cache, configured, live, state, channels_[i]->stream.processId());
        out << "; state=" << streamStateName(state)
            << "; last-known resolved=" << toUtf8(cache.info.name) << "; last-known ID=" << toUtf8(cache.info.id)
            << "; last-known process ID=" << cache.processId
            << "; last-known format=" << cache.info.format << "; last-known error=" << cache.info.error << '\n';
    }
    for (size_t i = 0; i < outputCount_; ++i) {
        const auto& configured = config_.outputAt(i);
        out << "Output " << i << ": " << configured.label << "; selection="
            << toUtf8(configured.deviceNameMatch) << "; pinned ID=" << toUtf8(configured.deviceId);
        if (!runtime) { out << '\n'; continue; }
        const auto state = renders_[i]->state();
        const auto live = state == StreamState::Stopped ? StreamDiagnosticInfo{} : renders_[i]->diagnosticInfo();
        auto& cache = diagnosticOutputs_[i];
        retain(cache, configured, live, state, 0);
        out << "; state=" << streamStateName(state)
            << "; last-known resolved=" << toUtf8(cache.info.name) << "; last-known ID=" << toUtf8(cache.info.id)
            << "; last-known format=" << cache.info.format << "; last-known error=" << cache.info.error << '\n';
    }
    return out.str();
}

std::string AudioEngine::diagnosticReport() const {
    DiagnosticSample current;
    DiagnosticHistoryCopy history;
    std::string devices;
    {
        // Do not let an export splice counts or labels from separate sessions.
        // Supervisor snapshots intentionally do NOT take this lock: stop/start
        // owns it while joining the supervisor, so that would deadlock.
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
        {
            std::lock_guard<std::mutex> lock(configMutex_);
            current = collectDiagnosticsLocked();
            if (current.session) devices = diagnosticDevicesLocked();
        }
        history = diagnostics_.copy();
    }
    return formatDiagnosticReport(current, history, devices, log::recentText());
}

ChannelStatus AudioEngine::channelStatus(int channel) const {
    ChannelStatus s;
    if (channel < 0 || size_t(channel) >= sourceCount_) return s;
    const Channel& ch = *channels_[channel];
    s.state      = ch.stream.state();
    s.flowing    = ch.stream.flowing();
    s.depth      = ch.depthOut.load(std::memory_order_relaxed);
    s.sampleRate = ch.stream.sampleRate();
    s.ratio      = ch.ratioOut.load(std::memory_order_relaxed);
    s.dropped    = ch.stream.droppedFrames() + ch.latencyTrimmed.load(std::memory_order_relaxed);
    s.deviceId   = ch.stream.resolvedId();
    s.processId  = ch.stream.processId();
    s.deviceName = ch.stream.resolvedName();
    s.error      = ch.stream.lastError();
    return s;
}

OutputStatus AudioEngine::outputStatus(size_t output) const {
    OutputStatus s;
    if (output >= outputCount_) return s;
    const auto& render = *renders_[output];
    s.state       = render.state();
    s.exclusive   = render.exclusive();
    s.sampleRate  = render.sampleRate();
    s.blockFrames = render.blockFrames();
    s.underruns   = render.underruns() + outputBuses_[output]->starvationEvents();
    s.dropped     = outputBuses_[output]->droppedFrames();
    s.pumpMissedPeriods = mixPumpMissedPeriods_.load(std::memory_order_relaxed);
    s.deviceId    = render.resolvedId();
    s.deviceName  = render.resolvedName();
    s.error       = render.lastError();
    const uint64_t state = monitoringState_.load(std::memory_order_acquire);
    if (running() && ((state & 1u) == 0 ||
        outputReadyState_[output].load(std::memory_order_acquire) != state)) {
        s.state = (state & 1u) != 0 ? StreamState::Opening : StreamState::Stopped;
        s.exclusive = false;
        s.sampleRate = 0;
        s.blockFrames = 0;
        s.error.clear();
    }
    return s;
}

void AudioEngine::setChannelDevice(int channel, const DeviceRef& ref) {
    if (channel < 0) {
        setOutputDevice(0, ref);
        return;
    }
    // Render callbacks never take configMutex_, so it is safe to hold it while
    // start() joins the worker. This also serializes with supervisor restarts.
    std::lock_guard<std::mutex> lock(configMutex_);
    if (size_t(channel) >= config_.sources.size()) return;
    ChannelConfig& target = config_.sources[channel];
    target.deviceId        = ref.id;
    target.deviceNameMatch = ref.nameMatch;

    if (!running()) return;
    if (size_t(channel) < sourceCount_ && target.enabled)
        channels_[channel]->stream.start(devices_, ref, "source device selection changed");
}

void AudioEngine::setOutputDevice(size_t output, const DeviceRef& ref) {
    std::lock_guard<std::mutex> lock(configMutex_);
    if (output >= outputCount_ || output >= config_.outputCount()) return;
    ChannelConfig& target = config_.outputAt(output);
    target.deviceId = ref.id;
    target.deviceNameMatch = ref.nameMatch;
    if (!monitoring()) return;
    outputBuses_[output]->requestReset();
    renders_[output]->start(devices_, ref, outputGates_[output].get(),
                            config_.exclusiveOutput, "output device selection changed");
}

bool AudioEngine::updateConfigFromRuntime(Config& config) const {
    // Persist any endpoint ID that was re-resolved by name, so the next launch
    // matches by ID again instead of re-scanning.
    bool changed = false;
    auto refresh = [&changed](ChannelConfig& c, const std::wstring& id) {
        if (!id.empty() && c.deviceId != id) {
            c.deviceId = id;
            changed = true;
        }
    };
    for (size_t i = 0; i < std::min(sourceCount_, config.sources.size()); ++i) {
        if (config.sources[i].kind != SourceKind::Application &&
            channels_[i]->stream.state() == StreamState::Running)
            refresh(config.sources[i], channels_[i]->stream.resolvedId());
    }
    for (size_t i = 0; i < std::min(outputCount_, config.outputCount()); ++i)
        if (renders_[i]->state() == StreamState::Running)
            refresh(config.outputAt(i), renders_[i]->resolvedId());
    // UI owns faders and toggles, including edits made while monitoring is stopped.
    return changed;
}

} // namespace audiomon
