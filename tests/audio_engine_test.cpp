#include "audio/AudioEngine.h"
#include "audio/WaveFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    static uint32_t depth(AudioEngine& engine) { return engine.channels_[0]->stream.ring().depth(); }
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
