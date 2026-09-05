#include "audio/AudioEngine.h"
#include "audio/WaveFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>

namespace audiomon {
// Feed the mixer deterministically without opening real capture/render devices.
struct AudioEngineTestAccess {
    static void prepare(AudioEngine& engine, uint32_t bufferMs) {
        engine.sourceCount_ = 1;
        engine.config_.sources.resize(1);
        auto& stream = engine.channels_[0]->stream;
        stream.configure("test", CaptureMode::Loopback);
        stream.sampleRate_.store(48000);
        stream.state_.store(StreamState::Running);
        stream.flowing_.store(true);
        engine.setBufferMillis(bufferMs);
        engine.onRenderFormat(48000, 480);
    }
    static void feed(AudioEngine& engine, uint32_t frames) {
        auto& ring = engine.channels_[0]->stream.ring();
        const auto available = ring.beginWrite();
        frames = std::min(frames, available);
        for (uint32_t i = 0; i < frames; ++i) ring.writeFrame(i, 0.5f, 0.25f);
        ring.endWrite(frames);
    }
    static void idle(AudioEngine& engine) { engine.channels_[0]->stream.flowing_.store(false); }
    static void setFlowing(AudioEngine& engine, bool flowing) {
        engine.channels_[0]->stream.flowing_.store(flowing);
    }
    static void setState(AudioEngine& engine, StreamState state) {
        engine.channels_[0]->stream.state_.store(state);
    }
    static void bumpEpoch(AudioEngine& engine) {
        engine.channels_[0]->stream.epoch_.fetch_add(1);
    }
    static void setSourceRate(AudioEngine& engine, uint32_t rate) {
        engine.channels_[0]->stream.sampleRate_.store(rate);
    }
    static double ratioOut(AudioEngine& engine) {
        return engine.channels_[0]->ratioOut.load();
    }
    static uint32_t depth(AudioEngine& engine) { return engine.channels_[0]->stream.ring().depth(); }
    static void setResolvedIdentity(AudioEngine& engine, const wchar_t* id, const wchar_t* name) {
        auto& stream = engine.channels_[0]->stream;
        std::lock_guard<std::mutex> lock(stream.infoMutex_);
        stream.resolvedId_ = id;
        stream.resolvedName_ = name;
    }
    static void stopSource(AudioEngine& engine) { engine.channels_[0]->stream.stop(); }
    static bool updateConfig(AudioEngine& engine, Config& config) {
        return engine.updateConfigFromRuntime(config);
    }
    static void startSupervisorOnly(AudioEngine& engine) {
        engine.quit_.store(false);
        engine.running_.store(true);
        engine.supervisor_ = std::thread(&AudioEngine::supervisorMain, &engine);
    }
    static void requestRebuild(AudioEngine& engine) { engine.requestRebuild(); }
    static bool invalidSelectionPreservesOutput(AudioEngine& engine) {
        engine.config_.output.deviceId = L"original";
        engine.setChannelDevice(500, {L"invalid", L"invalid"});
        return engine.config_.output.deviceId == L"original";
    }
};
} // namespace audiomon

int main() {
    using namespace audiomon;
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("FAIL: %s\n", label); ++failed; }
    };
    auto audible = [](const std::vector<float>& samples) {
        return std::any_of(samples.begin(), samples.end(), [](float v) { return std::fabs(v) > 0.1f; });
    };
    std::vector<float> block(480 * 2);
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 250);
        AudioEngineTestAccess::feed(engine, 12000);
        engine.renderMix(block.data(), 480);
        check(audible(block), "full buffer starts playback");
        AudioEngineTestAccess::idle(engine);
        engine.renderMix(block.data(), 480);
        check(audible(block), "idle loopback continues playing its buffered tail");
        for (int i = 0; i < 30; ++i) engine.renderMix(block.data(), 480);
        check(AudioEngineTestAccess::depth(engine) == 0, "idle loopback drains the full buffered tail");
        check(!audible(block), "drained source returns to silence");
        check(AudioEngineTestAccess::invalidSelectionPreservesOutput(engine),
              "invalid source index does not change the output device");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 250);
        AudioEngineTestAccess::setResolvedIdentity(engine, L"old", L"Old endpoint");
        AudioEngineTestAccess::stopSource(engine);
        Config config;
        config.sources.resize(1);
        config.sources[0].deviceId = L"new";
        check(!AudioEngineTestAccess::updateConfig(engine, config) &&
                  config.sources[0].deviceId == L"new",
              "stopping a source cannot persist its stale endpoint id");
        check(engine.channelStatus(0).deviceName.empty(),
              "stopping a source clears its stale friendly name");

        AudioEngineTestAccess::setResolvedIdentity(engine, L"resolved", L"Resolved endpoint");
        AudioEngineTestAccess::setState(engine, StreamState::Running);
        check(AudioEngineTestAccess::updateConfig(engine, config) &&
                  config.sources[0].deviceId == L"resolved",
              "re-resolved endpoint id reports a persistent config change");
        check(!AudioEngineTestAccess::updateConfig(engine, config),
              "unchanged endpoint id does not dirty the config repeatedly");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 250);
        AudioEngineTestAccess::feed(engine, 12000);
        engine.renderMix(block.data(), 480);
        check(audible(block), "source is audible before it stops");
        AudioEngineTestAccess::setState(engine, StreamState::Stopped);
        engine.renderMix(block.data(), 480);
        check(std::fabs(block[0]) > 0.1f, "stopped source begins with fade audio instead of zero");
        bool monotonic = true;
        float previous = std::fabs(block[0]);
        for (uint32_t frame = 1; frame < 480; ++frame) {
            const float current = std::fabs(block[frame * 2]);
            if (current > previous + 0.0001f) monotonic = false;
            previous = current;
        }
        check(monotonic && std::fabs(block.back()) < 0.0001f,
              "stopped source decays monotonically to silence");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 50);
        AudioEngineTestAccess::setSourceRate(engine, 96000);
        AudioEngineTestAccess::feed(engine, 4799);
        engine.renderMix(block.data(), 480);
        check(!audible(block), "96 kHz source waits below its 4800-frame setpoint");
        AudioEngineTestAccess::feed(engine, 1);
        engine.renderMix(block.data(), 480);
        check(audible(block), "96 kHz source starts at its 4800-frame setpoint");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 50);
        AudioEngineTestAccess::feed(engine, 12000);
        AudioEngineTestAccess::bumpEpoch(engine);
        engine.renderMix(block.data(), 480);
        check(!audible(block) && AudioEngineTestAccess::depth(engine) == 0,
              "epoch break before playback drops the stale ring and emits silence");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 50);
        AudioEngineTestAccess::feed(engine, 12000);
        engine.renderMix(block.data(), 480);
        check(audible(block), "source is audible before an epoch break");
        AudioEngineTestAccess::bumpEpoch(engine);
        engine.renderMix(block.data(), 480);
        bool monotonic = std::fabs(block[0]) > 0.1f;
        float previous = std::fabs(block[0]);
        for (uint32_t frame = 1; frame < 480; ++frame) {
            const float current = std::fabs(block[frame * 2]);
            monotonic = monotonic && current <= previous + 0.0001f;
            previous = current;
        }
        check(monotonic && std::fabs(block.back()) < 0.0001f &&
                  AudioEngineTestAccess::depth(engine) == 0,
              "audible epoch break fades stale audio before dropping the ring");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 50);
        engine.onRenderFormat(48000, 48);
        std::vector<float> shortBlock(48 * 2);
        AudioEngineTestAccess::feed(engine, 12000);
        for (int i = 0; i < 5; ++i) engine.renderMix(shortBlock.data(), 48);
        check(audible(shortBlock), "short render block becomes audible before stopping");
        float previous = std::fabs(shortBlock[(48 - 1) * 2]);
        AudioEngineTestAccess::setState(engine, StreamState::Stopped);
        bool monotonic = true;
        bool beganAudible = false;
        float largestStep = 0.0f;
        for (int render = 0; render < 6; ++render) {
            engine.renderMix(shortBlock.data(), 48);
            if (render == 0) beganAudible = std::fabs(shortBlock[0]) > 0.1f;
            for (uint32_t frame = 0; frame < 48; ++frame) {
                const float current = std::fabs(shortBlock[frame * 2]);
                largestStep = std::max(largestStep, std::fabs(current - previous));
                monotonic = monotonic && current <= previous + 0.0001f;
                previous = current;
            }
        }
        check(beganAudible && monotonic && largestStep < 0.01f && previous < 0.0001f &&
                  AudioEngineTestAccess::depth(engine) == 0,
              "stopped source preserves its fade across short render blocks");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 50);
        AudioEngineTestAccess::feed(engine, 4800);
        engine.renderMix(block.data(), 480);
        check(audible(block), "buffer-change source starts audible");
        engine.setBufferMillis(100);
        bool stayedAudible = true;
        for (int i = 0; i < 10; ++i) {
            AudioEngineTestAccess::feed(engine, 480);
            engine.renderMix(block.data(), 480);
            stayedAudible = stayedAudible && audible(block);
        }
        check(stayedAudible, "increasing the live buffer never re-primes to silence");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 20);
        AudioEngineTestAccess::feed(engine, 12000);
        engine.renderMix(block.data(), 480);
        const double ratio = AudioEngineTestAccess::ratioOut(engine);
        AudioEngineTestAccess::idle(engine);
        bool unchanged = true;
        for (int i = 0; i < 10; ++i) {
            engine.renderMix(block.data(), 480);
            unchanged = unchanged && AudioEngineTestAccess::ratioOut(engine) == ratio;
        }
        check(unchanged, "idle source freezes its converged resampling ratio");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::startSupervisorOnly(engine);
        AudioEngineTestAccess::requestRebuild(engine);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto start = std::chrono::steady_clock::now();
        engine.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        check(elapsed.count() < 200, "stop interrupts the supervisor debounce wait");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 20);
        AudioEngineTestAccess::feed(engine, 960);
        engine.renderMix(block.data(), 480);
        AudioEngineTestAccess::idle(engine);
        for (int i = 0; i < 4; ++i) engine.renderMix(block.data(), 480);
        check(AudioEngineTestAccess::depth(engine) == 0, "idle source drained before resume test");
        AudioEngineTestAccess::feed(engine, 480);
        engine.renderMix(block.data(), 480);
        check(!audible(block) && AudioEngineTestAccess::depth(engine) == 480,
              "new frames with stale flowing state wait one observation block");
        engine.renderMix(block.data(), 480);
        check(audible(block), "stable idle depth plays on the next block");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 50);
        AudioEngineTestAccess::feed(engine, 960);
        engine.renderMix(block.data(), 480); // records a depth below the setpoint
        engine.onRenderFormat(48000, 480);   // rebuild must discard that observation
        AudioEngineTestAccess::setFlowing(engine, false);
        AudioEngineTestAccess::feed(engine, 960);
        engine.renderMix(block.data(), 480);
        check(!audible(block) && AudioEngineTestAccess::depth(engine) == 960,
              "render-format rebuild clears stale idle-depth history");
        engine.renderMix(block.data(), 480);
        check(audible(block), "idle tail after a render rebuild plays on its second observation");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::prepare(engine, 250);
        AudioEngineTestAccess::feed(engine, 2400); // 50 ms notification, below the 250 ms target.
        engine.renderMix(block.data(), 480);
        check(!audible(block), "flowing source waits for its initial buffer");
        AudioEngineTestAccess::idle(engine);
        engine.renderMix(block.data(), 480);
        check(audible(block), "short sound is played even if it never fills the target buffer");
        for (int i = 0; i < 8; ++i) engine.renderMix(block.data(), 480);
        check(AudioEngineTestAccess::depth(engine) == 0, "short sound finishes draining");
    }
    {
        StreamFormat format{};
        format.sampleRate = 48000;
        format.channels = 1;
        format.type = SampleType::Float64;
        format.bitsPerSample = format.validBits = 64;
        format.blockAlign = 8;
        FormatConverter converter;
        converter.configure(format);
        const float stereo[] = {0.0f, 0.8f, 0.6f, -0.6f, -0.8f, 0.0f};
        double mono[3]{};
        converter.fromStereoFloat(stereo, mono, 3);
        check(std::fabs(mono[0] - 0.4) < 0.00001 && std::fabs(mono[1]) < 0.00001 &&
                  std::fabs(mono[2] + 0.4) < 0.00001,
              "64-bit mono output averages both stereo channels");
    }
    std::printf("Audio engine tests: %d failures\n", failed);
    return failed ? 1 : 0;
}
