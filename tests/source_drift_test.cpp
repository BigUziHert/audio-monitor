#include "audio/AudioEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using namespace audiomon;

constexpr uint32_t kMixRate = 48000;
constexpr uint32_t kBlockFrames = 480;
constexpr uint32_t kBufferMillis = 20;
constexpr int kBlocksPerSecond = int(kMixRate / kBlockFrames);
constexpr int kSoakMinutes = 20;
constexpr int kSoakBlocks = kSoakMinutes * 60 * kBlocksPerSecond;
constexpr int kSettlingBlocks = 60 * kBlocksPerSecond;

struct SourceSpec {
    uint32_t rate;
    double ppm;
    float amplitude;
    const char* label;
};

struct Oscillator {
    float amplitude = 0.0f;
    float value = 0.0f;
    uint32_t halfPeriod = 1;
    uint32_t untilFlip = 1;
};

struct VirtualSource {
    SourceSpec spec{};
    double productionCredit = 0.0;
    uint64_t pendingFrames = 0;
    size_t packetCursor = 0;
    Oscillator oscillator{};
};

struct SourceStats {
    uint64_t overflowFrames = 0;
    uint64_t primingBlocks = 0;
    uint64_t fadedBlocks = 0;
    uint64_t silentBlocks = 0;
    int firstBadBlock = -1;
    uint32_t minimumDepth = std::numeric_limits<uint32_t>::max();
    uint32_t maximumDepth = 0;
    std::array<long double, kSoakMinutes> minuteDepthSum{};
    std::array<uint64_t, kSoakMinutes> minuteDepthSamples{};
    long double finalRatioSum = 0.0;
    uint64_t finalRatioSamples = 0;
};

void markBad(SourceStats& stats, int block) {
    if (stats.firstBadBlock < 0)
        stats.firstBadBlock = block;
}

} // namespace

namespace audiomon {

// Deterministic access to the complete source-side AudioEngine path. This is
// deliberately a separate executable from audio_engine_test: the soak drives
// enough samples to expose slow controller drift without opening any device or
// creating any worker thread.
struct AudioEngineTestAccess {
    static void prepare(AudioEngine& engine, const std::vector<SourceSpec>& specs) {
        engine.sourceCount_ = specs.size();
        engine.config_.sources.resize(specs.size());
        engine.setBufferMillis(kBufferMillis);

        for (size_t i = 0; i < specs.size(); ++i) {
            auto& channel = *engine.channels_[i];
            channel.stream.configure(specs[i].label, CaptureMode::Loopback);
            channel.stream.sampleRate_.store(specs[i].rate, std::memory_order_release);
            channel.stream.state_.store(StreamState::Running, std::memory_order_release);
            channel.stream.flowing_.store(true, std::memory_order_release);
            channel.gain.store(1.0f, std::memory_order_relaxed);
            channel.muted.store(false, std::memory_order_relaxed);
        }

        engine.onRenderFormat(kMixRate, kBlockFrames);
    }

    static uint32_t publish(AudioEngine& engine, size_t source, uint32_t frames,
                            Oscillator& oscillator) {
        auto& ring = engine.channels_[source]->stream.ring();
        const uint32_t written = std::min(frames, ring.beginWrite());
        for (uint32_t i = 0; i < written; ++i) {
            ring.writeFrame(i, oscillator.value, oscillator.value * 0.75f);
            if (--oscillator.untilFlip == 0) {
                oscillator.value = -oscillator.value;
                oscillator.untilFlip = oscillator.halfPeriod;
            }
        }
        ring.endWrite(written);
        return written;
    }

    static void setFlowing(AudioEngine& engine, size_t source, bool flowing) {
        engine.channels_[source]->stream.flowing_.store(flowing, std::memory_order_release);
    }

    static uint32_t depth(AudioEngine& engine, size_t source) {
        return engine.channels_[source]->depthOut.load(std::memory_order_relaxed);
    }

    static uint32_t ringDepth(AudioEngine& engine, size_t source) {
        return engine.channels_[source]->stream.ring().depth();
    }

    static uint32_t ringCapacity(AudioEngine& engine, size_t source) {
        return engine.channels_[source]->stream.ring().capacity();
    }

    static bool priming(AudioEngine& engine, size_t source) {
        return engine.channels_[source]->priming;
    }

    static float presence(AudioEngine& engine, size_t source) {
        return engine.channels_[source]->presence.value();
    }

    static double ratio(AudioEngine& engine, size_t source) {
        return engine.channels_[source]->ratioOut.load(std::memory_order_relaxed);
    }
};

} // namespace audiomon

namespace {

Oscillator makeOscillator(const SourceSpec& spec) {
    Oscillator oscillator;
    oscillator.amplitude = spec.amplitude;
    oscillator.value = spec.amplitude;
    // A cheap, deterministic square wave near 440 Hz. Its constant magnitude
    // makes a missing source block observable through the real peak hand-off.
    oscillator.halfPeriod = std::max<uint32_t>(1, spec.rate / 880);
    oscillator.untilFlip = oscillator.halfPeriod;
    return oscillator;
}

uint32_t framesForMicros(uint32_t rate, uint32_t micros) {
    if (micros == 0)
        return 0;
    return std::max<uint32_t>(1, static_cast<uint32_t>(
        (static_cast<uint64_t>(rate) * micros + 500000) / 1000000));
}

uint64_t deliverPackets(AudioEngine& engine, size_t index, VirtualSource& source,
                        uint64_t frames) {
    // Real capture clients publish packets, not one sample at a time. Varying
    // 1.3--4.1 ms packet sizes exercises ring commits and resampler boundaries
    // without tying the simulation to the mix-pump's 10 ms block phase.
    static constexpr std::array<uint32_t, 7> packetMicros{
        2700, 1300, 4100, 1900, 3300, 2200, 1700,
    };

    uint64_t writtenTotal = 0;
    while (frames != 0) {
        const uint32_t packet = framesForMicros(
            source.spec.rate, packetMicros[source.packetCursor++ % packetMicros.size()]);
        const uint32_t requested = static_cast<uint32_t>(
            std::min<uint64_t>(frames, packet));
        const uint32_t written = AudioEngineTestAccess::publish(
            engine, index, requested, source.oscillator);
        writtenTotal += written;
        frames -= requested;
        if (written != requested)
            break;
    }
    return writtenTotal;
}

uint64_t produceOneBlock(AudioEngine& engine, size_t index, VirtualSource& source,
                         int block) {
    source.productionCredit +=
        double(source.spec.rate) * (1.0 + source.spec.ppm * 1.0e-6) /
        double(kBlocksPerSecond);
    const auto generated = static_cast<uint64_t>(source.productionCredit);
    source.productionCredit -= double(generated);
    source.pendingFrames += generated;

    // Model bounded capture scheduling jitter by retaining a changing 0--3 ms
    // tail for the next pump tick. Total production still follows the virtual
    // crystal exactly, but deliveries are deliberately not uniform blocks.
    static constexpr std::array<uint32_t, 11> heldMicros{
        0, 1700, 600, 2800, 1100, 3000, 400, 2200, 900, 2500, 1300,
    };
    const uint64_t desiredHeld = static_cast<uint64_t>(framesForMicros(
        source.spec.rate, heldMicros[static_cast<size_t>(block) % heldMicros.size()]));
    const uint64_t deliverable = source.pendingFrames > desiredHeld
        ? source.pendingFrames - desiredHeld
        : 0;
    const uint64_t written = deliverPackets(engine, index, source, deliverable);
    source.pendingFrames -= deliverable;
    return deliverable - written;
}

bool check(bool condition, const char* message, int& failures) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
    return condition;
}

void observeSoakBlock(AudioEngine& engine, const std::vector<SourceSpec>& specs,
                      std::vector<SourceStats>& stats, int block) {
    for (size_t i = 0; i < specs.size(); ++i) {
        const float peak = std::max(
            engine.channelPeak(static_cast<int>(i)).l.take(),
            engine.channelPeak(static_cast<int>(i)).r.take());
        if (block < kSettlingBlocks)
            continue;

        const uint32_t depth = AudioEngineTestAccess::depth(engine, i);
        stats[i].minimumDepth = std::min(stats[i].minimumDepth, depth);
        stats[i].maximumDepth = std::max(stats[i].maximumDepth, depth);
        if (AudioEngineTestAccess::priming(engine, i)) {
            ++stats[i].primingBlocks;
            markBad(stats[i], block);
        }
        if (AudioEngineTestAccess::presence(engine, i) < 0.999f) {
            ++stats[i].fadedBlocks;
            markBad(stats[i], block);
        }
        if (peak < specs[i].amplitude * 0.80f) {
            ++stats[i].silentBlocks;
            markBad(stats[i], block);
        }

        const int minute = block / (60 * kBlocksPerSecond);
        stats[i].minuteDepthSum[minute] += depth;
        ++stats[i].minuteDepthSamples[minute];
        if (minute == kSoakMinutes - 1) {
            stats[i].finalRatioSum += AudioEngineTestAccess::ratio(engine, i);
            ++stats[i].finalRatioSamples;
        }
    }
}

void validateSoak(AudioEngine& engine, const std::vector<SourceSpec>& specs,
                  const std::vector<SourceStats>& stats, const char* kind,
                  int& failures) {
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto& spec = specs[i];
        const auto& result = stats[i];
        const double target = double(spec.rate) * double(kBufferMillis) / 1000.0;
        const double baseRatio = double(spec.rate) / double(kMixRate);
        const double expectedRatio = baseRatio * (1.0 + spec.ppm * 1.0e-6);
        const double finalRatio = static_cast<double>(
            result.finalRatioSum / result.finalRatioSamples);
        const auto minuteMean = [&](int minute) {
            return static_cast<double>(result.minuteDepthSum[minute] /
                                       result.minuteDepthSamples[minute]);
        };
        const double middleMean = (minuteMean(9) + minuteMean(10) + minuteMean(11)) / 3.0;
        const double finalMean = (minuteMean(17) + minuteMean(18) + minuteMean(19)) / 3.0;
        const double trendMillis = (finalMean - middleMean) * 1000.0 / double(spec.rate);
        const double ratioErrorPpm = (finalRatio / expectedRatio - 1.0) * 1.0e6;

        std::printf(
            "%s %s %+.0f ppm: ratio %.9f (error %+.2f ppm), depth %.1f -> %.1f "
            "(range %u..%u), bad prime/fade/silent %llu/%llu/%llu, overflow %llu\n",
            kind, spec.label, spec.ppm, finalRatio, ratioErrorPpm, middleMean, finalMean,
            result.minimumDepth, result.maximumDepth,
            static_cast<unsigned long long>(result.primingBlocks),
            static_cast<unsigned long long>(result.fadedBlocks),
            static_cast<unsigned long long>(result.silentBlocks),
            static_cast<unsigned long long>(result.overflowFrames));

        check(result.overflowFrames == 0, "packetized producer overflowed the source ring", failures);
        check(result.primingBlocks == 0, "steady source re-entered priming (underrun)", failures);
        check(result.fadedBlocks == 0, "steady source entered a silence fade", failures);
        check(result.silentBlocks == 0, "steady source meter lost an audio block", failures);
        check(result.minimumDepth > static_cast<uint32_t>(target * 0.35),
              "steady source depth approached starvation", failures);
        check(result.maximumDepth < static_cast<uint32_t>(target * 1.75),
              "steady source backlog escaped the 20 ms operating region", failures);
        check(result.maximumDepth < AudioEngineTestAccess::ringCapacity(engine, i) / 2,
              "steady source approached ring capacity", failures);
        check(std::fabs(finalMean - target) < double(spec.rate) * 0.004,
              "final source depth is not centered near the configured target", failures);
        check(std::fabs(trendMillis) < 0.75,
              "source backlog still trends across the second half of the soak", failures);
        check(std::fabs(ratioErrorPpm) < 35.0,
              "source ratio did not converge to the simulated crystal offset", failures);
    }
}

void runClockSoak(int& failures) {
    const std::vector<SourceSpec> specs{
        {44100, +125.0, 0.035f, "44.1k-fast"},
        {48000, -100.0, 0.045f, "48k-slow"},
        {96000,  +75.0, 0.055f, "96k-fast"},
    };

    AudioEngine engine;
    AudioEngineTestAccess::prepare(engine, specs);
    std::vector<VirtualSource> sources;
    sources.reserve(specs.size());
    for (const auto& spec : specs) {
        VirtualSource source;
        source.spec = spec;
        source.oscillator = makeOscillator(spec);
        sources.push_back(source);
    }

    // Begin at the 20 ms operating point, plus the four frames consumed when
    // the Catmull-Rom history is first primed. This isolates long-term clock
    // behavior from startup buffering while still using the real priming path.
    for (size_t i = 0; i < sources.size(); ++i) {
        const uint32_t target = specs[i].rate * kBufferMillis / 1000;
        const uint32_t requested = target + 4;
        const uint64_t written = deliverPackets(engine, i, sources[i], requested);
        check(written == requested, "could not prefill source drift fixture", failures);
    }

    std::array<float, kBlockFrames * 2> mix{};
    engine.renderMix(mix.data(), kBlockFrames);
    for (size_t i = 0; i < sources.size(); ++i) {
        engine.channelPeak(static_cast<int>(i)).l.take();
        engine.channelPeak(static_cast<int>(i)).r.take();
    }

    std::vector<SourceStats> stats(sources.size());
    for (int block = 0; block < kSoakBlocks; ++block) {
        for (size_t i = 0; i < sources.size(); ++i)
            stats[i].overflowFrames += produceOneBlock(engine, i, sources[i], block);

        engine.renderMix(mix.data(), kBlockFrames);
        observeSoakBlock(engine, specs, stats, block);
    }

    validateSoak(engine, specs, stats, "subpacket", failures);
}

void runWholePacketPhaseSoak(int& failures) {
    const std::vector<SourceSpec> specs{
        {44100, +400.0, 0.040f, "44.1k-fast"},
        {96000, -400.0, 0.050f, "96k-slow"},
    };

    AudioEngine engine;
    AudioEngineTestAccess::prepare(engine, specs);
    std::vector<VirtualSource> sources;
    sources.reserve(specs.size());
    for (const auto& spec : specs) {
        VirtualSource source;
        source.spec = spec;
        source.oscillator = makeOscillator(spec);
        sources.push_back(source);
    }

    for (size_t i = 0; i < sources.size(); ++i) {
        const uint32_t target = specs[i].rate * kBufferMillis / 1000;
        const uint64_t written = deliverPackets(engine, i, sources[i], target + 4);
        check(written == target + 4, "could not prefill whole-packet drift fixture", failures);
    }

    // Producers and the mix pump have independent event schedules. Each
    // capture callback commits one complete native 10 ms packet, so the fast
    // clock periodically delivers two packets before a pump and the slow one
    // periodically delivers none. At 400 ppm the phases wrap about 48 times in
    // twenty minutes, exercising the minimum 20 ms buffer across the full
    // packet phase rather than averaging fractional production every tick.
    std::array<double, 2> nextPacketSeconds{0.0, 0.0047};
    std::array<uint64_t, 2> emptyPumpBlocks{};
    std::array<uint64_t, 2> doublePacketBlocks{};
    std::array<float, kBlockFrames * 2> mix{};
    engine.renderMix(mix.data(), kBlockFrames);
    for (size_t i = 0; i < sources.size(); ++i) {
        engine.channelPeak(static_cast<int>(i)).l.take();
        engine.channelPeak(static_cast<int>(i)).r.take();
    }

    std::vector<SourceStats> stats(sources.size());
    double nextPumpSeconds = 0.003;
    for (int block = 0; block < kSoakBlocks; ++block) {
        for (size_t i = 0; i < sources.size(); ++i) {
            const uint32_t packetFrames = specs[i].rate / 100;
            const double packetPeriod = 0.01 / (1.0 + specs[i].ppm * 1.0e-6);
            uint32_t packets = 0;
            while (nextPacketSeconds[i] <= nextPumpSeconds) {
                const uint32_t written = AudioEngineTestAccess::publish(
                    engine, i, packetFrames, sources[i].oscillator);
                stats[i].overflowFrames += packetFrames - written;
                nextPacketSeconds[i] += packetPeriod;
                ++packets;
            }
            if (packets == 0) ++emptyPumpBlocks[i];
            if (packets >= 2) ++doublePacketBlocks[i];
        }

        engine.renderMix(mix.data(), kBlockFrames);
        observeSoakBlock(engine, specs, stats, block);
        nextPumpSeconds += 0.01;
    }

    std::printf("whole-packet phase events: fast double=%llu, slow empty=%llu\n",
                static_cast<unsigned long long>(doublePacketBlocks[0]),
                static_cast<unsigned long long>(emptyPumpBlocks[1]));
    check(doublePacketBlocks[0] > 0,
          "fast capture clock never crossed a whole-packet pump phase", failures);
    check(emptyPumpBlocks[1] > 0,
          "slow capture clock never crossed a whole-packet pump phase", failures);
    validateSoak(engine, specs, stats, "whole-packet", failures);
}

void runStallAndResume(int& failures) {
    const SourceSpec spec{48000, 0.0, 0.05f, "resume"};
    AudioEngine engine;
    AudioEngineTestAccess::prepare(engine, {spec});
    VirtualSource source;
    source.spec = spec;
    source.oscillator = makeOscillator(spec);

    const uint32_t target = spec.rate * kBufferMillis / 1000;
    deliverPackets(engine, 0, source, target + 4);
    std::array<float, kBlockFrames * 2> mix{};
    engine.renderMix(mix.data(), kBlockFrames);
    engine.channelPeak(0).l.take();
    engine.channelPeak(0).r.take();

    for (int block = 0; block < 200; ++block) {
        const uint64_t written = deliverPackets(engine, 0, source, kBlockFrames);
        check(written == kBlockFrames, "steady resume fixture overflowed", failures);
        engine.renderMix(mix.data(), kBlockFrames);
        engine.channelPeak(0).l.take();
        engine.channelPeak(0).r.take();
    }

    AudioEngineTestAccess::setFlowing(engine, 0, false);
    for (int block = 0; block < 50; ++block) {
        engine.renderMix(mix.data(), kBlockFrames);
        engine.channelPeak(0).l.take();
        engine.channelPeak(0).r.take();
    }
    check(AudioEngineTestAccess::ringDepth(engine, 0) == 0,
          "stalled capture did not drain its buffered tail", failures);
    check(AudioEngineTestAccess::priming(engine, 0) &&
              AudioEngineTestAccess::presence(engine, 0) <= 0.0f,
          "stalled capture did not settle into silent priming", failures);

    // Resume with two 10 ms packets delivered together, as happens when a
    // capture poll is delayed by one period. The 20 ms target must absorb the
    // bounded backlog without replaying silence or growing indefinitely.
    const uint64_t resumed = deliverPackets(engine, 0, source, target);
    AudioEngineTestAccess::setFlowing(engine, 0, true);
    engine.renderMix(mix.data(), kBlockFrames);
    const float resumedPeak = std::max(engine.channelPeak(0).l.take(),
                                       engine.channelPeak(0).r.take());
    check(resumed == target && resumedPeak > spec.amplitude * 0.8f &&
              !AudioEngineTestAccess::priming(engine, 0),
          "packet backlog did not resume through the normal fade-in block", failures);

    uint32_t minimumDepth = std::numeric_limits<uint32_t>::max();
    uint32_t maximumDepth = 0;
    uint64_t badBlocks = 0;
    for (int block = 0; block < 60 * kBlocksPerSecond; ++block) {
        deliverPackets(engine, 0, source, kBlockFrames);
        engine.renderMix(mix.data(), kBlockFrames);
        const float peak = std::max(engine.channelPeak(0).l.take(),
                                    engine.channelPeak(0).r.take());
        const uint32_t depth = AudioEngineTestAccess::depth(engine, 0);
        minimumDepth = std::min(minimumDepth, depth);
        maximumDepth = std::max(maximumDepth, depth);
        if (AudioEngineTestAccess::priming(engine, 0) ||
            AudioEngineTestAccess::presence(engine, 0) < 0.999f ||
            peak < spec.amplitude * 0.8f)
            ++badBlocks;
    }
    check(badBlocks == 0, "resumed capture was not continuously audible", failures);
    check(minimumDepth > target / 2 && maximumDepth < target + kBlockFrames,
          "resumed packet backlog did not remain bounded", failures);
}

} // namespace

int main() {
    int failures = 0;
    std::printf("== deterministic AudioEngine source drift soak ==\n");
    runClockSoak(failures);
    runWholePacketPhaseSoak(failures);
    runStallAndResume(failures);
    std::printf("Source drift tests: %d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
