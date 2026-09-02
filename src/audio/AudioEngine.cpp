#include "audio/AudioEngine.h"
#include "util/Log.h"

#include <algorithm>
#include <cmath>

namespace audiomon {
namespace {

// Generous preallocation so a device rebuild with a larger period never has to
// allocate on the render thread.
constexpr uint32_t kMaxBlockFrames = 8192;

// Debounce for device notifications: a driver reinstall fires a burst of them,
// and rebuilding on each one would thrash.
constexpr int kRebuildDebounceMs = 750;
constexpr int kSupervisorPollMs  = 2000;

// Fade length for gain changes and for the edges of a silent gap.
constexpr double kFadeSeconds = 0.005;

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
}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(const Config& config) {
    stop();
    sourceCount_ = std::min(config.sources.size(), size_t(kMaxSources));
    LOG_INFO("engine: start");
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        config_ = config;
    }
    LOG_INFO("engine: config copied");

    if (!devices_.start([this] { requestRebuild(); })) {
        LOG_ERR("engine: device enumerator unavailable");
        return false;
    }

    LOG_INFO("engine: device manager up");

    for (size_t i = 0; i < sourceCount_; ++i) {
        const auto& source = config.sources[i];
        auto& channel = *channels_[i];
        auto mode = source.kind == SourceKind::Application ? CaptureMode::Application :
                    source.kind == SourceKind::Microphone ? CaptureMode::Microphone : CaptureMode::Loopback;
        channel.stream.configure(source.label.c_str(), mode);
        channel.gain.store(source.gain);
        channel.muted.store(source.muted || !source.enabled);
        channel.peak.l.take(); channel.peak.r.take();
        if (source.enabled) channel.stream.start(devices_, source.kind == SourceKind::Application
            ? DeviceRef{L"", source.processPath} : DeviceRef{source.deviceId, source.deviceNameMatch});
    }
    outputGain_.store(config.output.gain);
    outputMuted_.store(config.output.muted);
    mono_.store(config.mono);
    mixMode_.reset(config.mono);
    outputPeak_.l.take(); outputPeak_.r.take();
    visualSamples_.dropAllFromConsumer();
    bufferMillis_.store(config.bufferMillis);

    LOG_INFO("engine: starting render");
    render_.start(devices_, { config.output.deviceId, config.output.deviceNameMatch },
                  this, config.exclusiveOutput);

    LOG_INFO("engine: all streams requested; starting supervisor");
    quit_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    supervisor_ = std::thread(&AudioEngine::supervisorMain, this);
    return true;
}

void AudioEngine::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    quit_.store(true, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lock(superMutex_); superWake_ = true; }
    superCv_.notify_all();
    if (supervisor_.joinable()) supervisor_.join();

    render_.stop();
    for (auto& ch : channels_) ch->stream.stop();
    devices_.stop();
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
        ? DeviceRef{L"", source.processPath} : DeviceRef{source.deviceId, source.deviceNameMatch});
    else channels_[channel]->stream.stop();
}

void AudioEngine::setOutputGain(float g) noexcept {
    outputGain_.store(std::clamp(g, 0.0f, 4.0f), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Render thread. Everything below this line must be allocation-free and
// lock-free.
// ---------------------------------------------------------------------------

void AudioEngine::onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept {
    // Runs on the render thread but BEFORE IAudioClient::Start, so no audio is
    // flowing yet and the allocation below cannot cause a dropout. This is the
    // only place the engine is allowed to allocate after construction.
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
    // is known. Setting it from the render rate here would hold a 96 kHz
    // source at half the intended depth.
    for (size_t i = 0; i < sourceCount_; ++i) {
        Channel& ch = *channels_[i];
        ch.lastSrcRate  = 0;             // force a recompute on the next block
        ch.lastBufferMs = 0;
        ch.targetDepth  = 0.0;
        ch.resampler.reset();
        ch.priming      = true;
        ch.presence.configure(uint32_t(double(sampleRate) * kFadeSeconds));
        ch.presence.reset(0.0f);
        ch.smoothedGain = ch.muted.load(std::memory_order_relaxed) ? 0.0f : ch.gain.load(std::memory_order_relaxed);
        // Ring holds stale audio from before the rebuild; start clean.
        ch.stream.ring().dropAllFromConsumer();
    }
    smoothedOutputGain_ = outputMuted_.load(std::memory_order_relaxed) ? 0.0f : outputGain_.load(std::memory_order_relaxed);
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

        // A timeline break (discontinuity, reconnect, overflow drop) makes the
        // controller's history meaningless.
        const uint32_t ep = ch.stream.epoch();
        if (ep != ch.lastEpoch) {
            ch.lastEpoch = ep;
            ch.rate.reset();
            ch.resampler.reset();
            ch.priming = true;
            // Everything buffered predates the break. Playing it out would be
            // a burst of stale audio, and after a device restart it might not
            // even be at the right sample rate. We are the consumer, so this
            // is ours to drop.
            ch.stream.ring().dropAllFromConsumer();
        }

        const bool live = ch.stream.state() == StreamState::Running && ch.stream.flowing();
        uint32_t   made = 0;

        if (live) {
            const uint32_t depth = ch.stream.ring().depth();
            ch.depthOut.store(depth, std::memory_order_relaxed);

            const uint32_t srcRate = ch.stream.sampleRate();
            ch.baseRatio = (srcRate && rate) ? double(srcRate) / double(rate) : 1.0;

            const uint32_t bufMs = bufferMillis_.load(std::memory_order_relaxed);

            if (srcRate && srcRate != ch.lastSrcRate) {
                // New clock: full reconfigure. Setpoint and controller are
                // denominated in capture frames, matching the ring. dt is real
                // elapsed time, which the RENDER device sets -- the two rates
                // are independent.
                ch.lastSrcRate  = srcRate;
                ch.lastBufferMs = bufMs;
                ch.targetDepth  = double(srcRate) * double(bufMs) / 1000.0;
                const double dt = rate ? double(frames) / double(rate) : 0.01;
                ch.rate.configure(double(srcRate), ch.targetDepth, dt);
                ch.priming = true;
            } else if (srcRate && bufMs != ch.lastBufferMs) {
                // Same clock, new setpoint: the user moved the buffer slider.
                // setTarget rather than configure -- a reconfigure resets the
                // controller and re-primes, which would drop audio for a
                // setting the user is likely to nudge repeatedly. The depth
                // migrates under the existing slew limit instead.
                ch.lastBufferMs = bufMs;
                ch.targetDepth  = double(srcRate) * double(bufMs) / 1000.0;
                ch.rate.setTarget(ch.targetDepth);
            }
            if (ch.targetDepth <= 0.0) { ch.priming = true; }

            // Priming: wait for the ring to reach the setpoint before drawing
            // from it, so we do not immediately starve. This is also the path
            // taken every time a silent loopback endpoint starts producing
            // again.
            if (ch.priming && depth >= ch.targetDepth) {
                ch.priming = false;
                ch.rate.reset();
            }

            if (!ch.priming) {
                // The controller only ever corrects for drift; the base ratio
                // handles a genuine rate difference and is not part of the
                // correction the clamp applies to.
                const double ratio = ch.baseRatio * ch.rate.update(double(depth));
                ch.ratioOut.store(ratio, std::memory_order_relaxed);
                made = ch.resampler.produce(ch.stream.ring(), ch.scratch.data(), frames, ratio);
                if (made < frames) ch.priming = true;   // starved: re-prime
            }
        } else {
            ch.depthOut.store(ch.stream.ring().depth(), std::memory_order_relaxed);
            // Hold the converged ratio while the source is idle. Letting the
            // controller integrate against a permanently empty ring would wind
            // it to an extreme that is wildly wrong when audio resumes.
            ch.priming = true;
        }

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
        ch.presence.beginBlock(made, frames);

        const float gainTarget = ch.muted.load(std::memory_order_relaxed)
                                     ? 0.0f
                                     : ch.gain.load(std::memory_order_relaxed);

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
        ch.smoothedGain = g;

        // Post-fader, post-mute, like the OBS mixer: muting drops the meter.
        ch.peak.l.publish(peakL);
        ch.peak.r.publish(peakR);
    }

    // Output trim and master meter.
    const float outTarget = outputMuted_.load(std::memory_order_relaxed) ? 0.0f : outputGain_.load(std::memory_order_relaxed);
    const bool mono = mono_.load(std::memory_order_relaxed);
    const float modeStep = 1.0f / std::max(1.0f, float(rate) * 0.005f);
    float og = smoothedOutputGain_;
    float opl = 0.0f, opr = 0.0f;
    for (uint32_t f = 0; f < frames; ++f) {
        og += (outTarget - og) * gainCoef;
        float l = dst[f * 2]     * og;
        float r = dst[f * 2 + 1] * og;
        mixMode_.process(l, r, mono, modeStep);
        dst[f * 2]     = l;
        dst[f * 2 + 1] = r;
        const float al = std::fabs(l), ar = std::fabs(r);
        if (al > opl) opl = al;
        if (ar > opr) opr = ar;
    }
    smoothedOutputGain_ = og;
    outputPeak_.l.publish(opl);
    outputPeak_.r.publish(opr);
    const uint32_t visualCount = std::min(frames, visualSamples_.beginWrite());
    for (uint32_t f = 0; f < visualCount; ++f) visualSamples_.writeFrame(f, dst[f * 2], dst[f * 2 + 1]);
    visualSamples_.endWrite(visualCount);

#if defined(AUDIOMON_TRACE_FIRST_MIX)
    if (firstMix) LOG_INFO("mix: first block completed");
#endif
}

// ---------------------------------------------------------------------------
// Supervisor thread: every device rebuild happens here, never on the render
// thread and never inside a COM notification callback.
// ---------------------------------------------------------------------------

void AudioEngine::requestRebuild() {
    { std::lock_guard<std::mutex> lock(superMutex_); superWake_ = true; }
    superCv_.notify_one();
}

void AudioEngine::supervisorMain() {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsUninit = SUCCEEDED(coHr);

    while (!quit_.load(std::memory_order_relaxed)) {
        bool woken = false;
        {
            std::unique_lock<std::mutex> lock(superMutex_);
            woken = superCv_.wait_for(lock, std::chrono::milliseconds(kSupervisorPollMs),
                                      [this] { return superWake_ || quit_.load(std::memory_order_relaxed); });
            superWake_ = false;
        }
        if (quit_.load(std::memory_order_relaxed)) break;

        if (woken) {
            // Coalesce the burst a driver reinstall produces.
            std::this_thread::sleep_for(std::chrono::milliseconds(kRebuildDebounceMs));
            std::lock_guard<std::mutex> lock(superMutex_);
            superWake_ = false;
        }
        if (quit_.load(std::memory_order_relaxed)) break;

        // Each ref is read from config_ IMMEDIATELY before the restart that
        // uses it, not from a snapshot taken at the top of the pass. A snapshot
        // goes stale the moment the UI changes a device -- and Restore
        // defaults changes four at once -- so a channel that failed fast on
        // its new name would have been reopened on its OLD pinned device.
        auto outputRef = [this](bool& exclusive) {
            std::lock_guard<std::mutex> lock(configMutex_);
            exclusive = config_.exclusiveOutput;
            return DeviceRef{ config_.output.deviceId, config_.output.deviceNameMatch };
        };

        for (size_t i = 0; i < sourceCount_; ++i) {
            std::lock_guard<std::mutex> lock(configMutex_);
            const auto& source = config_.sources[i];
            auto& ch = *channels_[i];
            if (!source.enabled || ch.stream.state() != StreamState::Failed) continue;
            ch.stream.start(devices_, source.kind == SourceKind::Application
                ? DeviceRef{L"", source.processPath} : DeviceRef{source.deviceId, source.deviceNameMatch});
        }

        // Passivity check. If the endpoint we hold exclusively has since become
        // a system default -- the user changed it in the Sound control panel,
        // or a driver reinstall reassigned it -- we must hand exclusive control
        // back, or every other application on the machine loses its output.
        // Restarting is enough: openDevice re-runs the same gate and will come
        // back up in shared mode.
        if (render_.state() == StreamState::Running && render_.exclusive() &&
            devices_.isDefaultForAnyRole(render_.resolvedId())) {
            LOG_WARN("supervisor: output endpoint became a system default; "
                     "releasing exclusive mode and reopening shared");
            bool exclusive;
            const DeviceRef ref = outputRef(exclusive);
            render_.start(devices_, ref, this, exclusive);
        }

        // The Elgato commonly enumerates late on a cold boot, so a failed
        // output is a normal startup state that resolves itself.
        if (render_.state() == StreamState::Failed || render_.wantsRetry()) {
            LOG_INFO("supervisor: restarting output (%s)", render_.lastError().c_str());
            bool exclusive;
            const DeviceRef ref = outputRef(exclusive);
            render_.start(devices_, ref, this, exclusive);
        }
    }

    if (needsUninit) CoUninitialize();
}

// ---------------------------------------------------------------------------

ChannelStatus AudioEngine::channelStatus(int channel) const {
    ChannelStatus s;
    if (channel < 0 || size_t(channel) >= sourceCount_) return s;
    const Channel& ch = *channels_[channel];
    s.state      = ch.stream.state();
    s.flowing    = ch.stream.flowing();
    s.depth      = ch.depthOut.load(std::memory_order_relaxed);
    s.sampleRate = ch.stream.sampleRate();
    s.ratio      = ch.ratioOut.load(std::memory_order_relaxed);
    s.dropped    = ch.stream.droppedFrames();
    s.deviceId   = ch.stream.resolvedId();
    s.processId  = ch.stream.processId();
    s.deviceName = ch.stream.resolvedName();
    s.error      = ch.stream.lastError();
    return s;
}

OutputStatus AudioEngine::outputStatus() const {
    OutputStatus s;
    s.state       = render_.state();
    s.exclusive   = render_.exclusive();
    s.sampleRate  = render_.sampleRate();
    s.blockFrames = render_.blockFrames();
    s.underruns   = render_.underruns();
    s.deviceId    = render_.resolvedId();
    s.deviceName  = render_.resolvedName();
    s.error       = render_.lastError();
    return s;
}

void AudioEngine::setChannelDevice(int channel, const DeviceRef& ref) {
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        ChannelConfig* target = channel >= 0 && size_t(channel) < config_.sources.size()
            ? &config_.sources[channel] : &config_.output;
        target->deviceId        = ref.id;
        target->deviceNameMatch = ref.nameMatch;
    }

    if (!running()) return;
    if (channel >= 0 && size_t(channel) < sourceCount_) {
        channels_[channel]->stream.start(devices_, ref);
    } else {
        // Read the flag out from under the lock first. start() joins the
        // render thread, and holding configMutex_ across that join would
        // deadlock against onRenderFormat if it were to take the same lock.
        bool exclusive;
        { std::lock_guard<std::mutex> lock(configMutex_); exclusive = config_.exclusiveOutput; }
        render_.start(devices_, ref, this, exclusive);
    }
}

void AudioEngine::updateConfigFromRuntime(Config& config) const {
    // Persist any endpoint ID that was re-resolved by name, so the next launch
    // matches by ID again instead of re-scanning.
    auto refresh = [](ChannelConfig& c, const std::wstring& id) {
        if (!id.empty()) c.deviceId = id;
    };
    for (size_t i = 0; i < std::min(sourceCount_, config.sources.size()); ++i) {
        if (config.sources[i].kind != SourceKind::Application)
            refresh(config.sources[i], channels_[i]->stream.resolvedId());
    }
    refresh(config.output, render_.resolvedId());
    // UI owns faders and toggles, including edits made while monitoring is stopped.

}

} // namespace audiomon
