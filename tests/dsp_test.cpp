#include <numbers>
#include "audio/MixMode.h"
#include "audio/Overlap.h"
#include "audio/Spectrum.h"
// Host-native tests for the parts of the mixer that are not Windows-specific:
// the SPSC ring, resampler, rate controller, sample conversion and config JSON.
//
// These run on any platform through scripts/run_tests.sh without audio
// hardware.

#include "audio/RingBuffer.h"
#include "audio/Resampler.h"
#include "audio/RateController.h"
#include "audio/RetryPolicy.h"
#include "audio/SampleFormat.h"
#include "audio/Meter.h"
#include "audio/FadeEnvelope.h"
#include "config/Json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace audiomon;

static int g_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static void testRing() {
    std::printf("ring buffer\n");
    StereoRing r;
    r.init(1000);                       // rounds up to 1024
    CHECK(r.capacity() == 1024, "capacity %u", r.capacity());
    CHECK(r.depth() == 0, "fresh ring not empty");

    const uint32_t space = r.beginWrite();
    CHECK(space == 1024, "space %u", space);
    for (uint32_t i = 0; i < 100; ++i) r.writeFrame(i, float(i), float(-int(i)));
    r.endWrite(100);
    CHECK(r.depth() == 100, "depth %u", r.depth());

    const uint32_t avail = r.beginRead();
    CHECK(avail == 100, "avail %u", avail);
    float l = 0, rr = 0;
    r.readFrame(50, l, rr);
    CHECK(l == 50.0f && rr == -50.0f, "frame 50 = %f/%f", l, rr);
    r.endRead(100);
    CHECK(r.depth() == 0, "depth after drain %u", r.depth());

    // Wrap the index space many times to prove the mask arithmetic and the
    // unsigned-subtraction depth stay correct across a uint32 wrap.
    for (int cycle = 0; cycle < 5000; ++cycle) {
        r.beginWrite();
        for (uint32_t i = 0; i < 700; ++i) r.writeFrame(i, float(cycle), 0.0f);
        r.endWrite(700);
        CHECK(r.depth() == 700, "cycle %d depth %u", cycle, r.depth());
        r.beginRead();
        r.readFrame(0, l, rr);
        CHECK(l == float(cycle), "cycle %d read %f", cycle, l);
        r.endRead(700);
    }
}

// At ratio 1.0 Catmull-Rom evaluates at t=0, which returns the sample itself,
// so the output must be a bit-for-bit copy of the input delayed by the kernel's
// one-frame priming latency.
//
// The previous version of this test compared the left channel against the right
// and asserted they matched -- but both are fed identical samples, so it passed
// for ANY resampler, including one returning zeros. It tested nothing. This one
// compares against the analytic signal.
static void testResamplerUnityIsExact() {
    std::printf("resampler is exact at ratio 1.0\n");
    StereoRing r;
    r.init(8192);
    DriftResampler rs;
    rs.reset();

    const double freq = 440.0, fs = 48000.0;
    auto sample = [&](long n) { return float(std::sin(2.0 * std::numbers::pi * freq * double(n) / fs)); };

    long inWritten = 0;      // next input frame index to generate
    long outCount  = 0;      // total output frames produced
    double maxErr = 0.0;
    std::vector<float> out(512 * 2);

    for (int block = 0; block < 40; ++block) {
        const uint32_t space = r.beginWrite();
        uint32_t w = 0;
        for (; w < space && w < 600; ++w) r.writeFrame(w, sample(inWritten + w), sample(inWritten + w));
        r.endWrite(w);
        inWritten += w;

        const uint32_t made = rs.produce(r, out.data(), 512, 1.0);
        for (uint32_t i = 0; i < made; ++i) {
            // Priming consumes 4 frames and leaves the read position on
            // frame 1, so output k corresponds to input k+1.
            const double expected = sample(outCount + 1);
            maxErr = std::max(maxErr, std::fabs(double(out[i * 2]) - expected));
            maxErr = std::max(maxErr, std::fabs(double(out[i * 2 + 1]) - expected));
            ++outCount;
        }
    }
    std::printf("   %ld frames, max deviation from the analytic signal = %.3g\n", outCount, maxErr);
    CHECK(outCount > 10000, "only produced %ld frames", outCount);
    CHECK(maxErr < 1e-6, "not an identity at ratio 1.0: max error %g", maxErr);
}

// Exercises the interpolation kernel itself. A broken Catmull-Rom coefficient
// shows up here but not in the unity test, where t is always 0.
static void testResamplerInterpolates() {
    std::printf("resampler interpolation accuracy (ratio 0.5)\n");
    StereoRing r;
    r.init(16384);
    DriftResampler rs;
    rs.reset();

    const double freq = 440.0, fs = 48000.0, ratio = 0.5;
    auto sample = [&](double n) { return std::sin(2.0 * std::numbers::pi * freq * n / fs); };

    long inWritten = 0, outCount = 0;
    double maxErr = 0.0;
    std::vector<float> out(512 * 2);

    for (int block = 0; block < 40; ++block) {
        const uint32_t space = r.beginWrite();
        uint32_t w = 0;
        for (; w < space && w < 400; ++w) {
            const float v = float(sample(double(inWritten + w)));
            r.writeFrame(w, v, v);
        }
        r.endWrite(w);
        inWritten += w;

        const uint32_t made = rs.produce(r, out.data(), 512, ratio);
        for (uint32_t i = 0; i < made; ++i) {
            // Output k sits at input position 1 + k*ratio.
            const double expected = sample(1.0 + double(outCount) * ratio);
            maxErr = std::max(maxErr, std::fabs(double(out[i * 2]) - expected));
            ++outCount;
        }
    }
    std::printf("   %ld frames, max interpolation error = %.3g\n", outCount, maxErr);
    CHECK(outCount > 5000, "only produced %ld frames", outCount);
    // 440 Hz at 48 kHz is heavily oversampled, so a correct 4-point kernel is
    // far better than this; a wrong coefficient blows straight past it.
    CHECK(maxErr < 1e-4, "interpolation kernel is inaccurate: max error %g", maxErr);
}

// The important one: a producer clocked fast relative to the consumer must NOT
// make the ring grow without bound once the controller is in the loop.
static void testDriftConvergence(double ppm, const char* label) {
    std::printf("drift convergence %s (%+.0f ppm)\n", label, ppm);

    const double   fs         = 48000.0;
    const uint32_t blockOut   = 480;          // 10ms consumer block
    const uint32_t ringFrames = 8192;
    const double   target     = 2400.0;       // 50ms setpoint

    StereoRing r;
    r.init(ringFrames);
    DriftResampler rs;
    rs.reset();
    RateController ctl;
    ctl.configure(fs, target, double(blockOut) / fs);

    // Prime the ring to the setpoint so we start at the operating point.
    r.beginWrite();
    for (uint32_t i = 0; i < uint32_t(target); ++i) r.writeFrame(i, 0.0f, 0.0f);
    r.endWrite(uint32_t(target));

    std::vector<float> out(blockOut * 2);
    double producerCredit = 0.0;
    const double producerPerBlock = blockOut * (1.0 + ppm * 1e-6);

    double phase = 0.0;
    uint32_t underruns = 0, overflows = 0;
    double minDepth = 1e18, maxDepth = 0.0;

    // 20 minutes of audio. Uncorrected, a 100ppm error moves 288000 frames in
    // that time -- 35x the whole ring -- so this test genuinely fails without
    // the controller.
    const int blocks = int(20 * 60 * fs / blockOut);
    for (int b = 0; b < blocks; ++b) {
        producerCredit += producerPerBlock;
        uint32_t toWrite = uint32_t(producerCredit);
        producerCredit -= toWrite;

        const uint32_t space = r.beginWrite();
        if (toWrite > space) { overflows++; toWrite = space; }
        for (uint32_t i = 0; i < toWrite; ++i) {
            const float s = float(std::sin(2.0 * std::numbers::pi * 220.0 * phase / fs));
            r.writeFrame(i, s, s);
            phase += 1.0;
        }
        r.endWrite(toWrite);

        const double ratio = ctl.update(double(r.depth()));
        const uint32_t made = rs.produce(r, out.data(), blockOut, ratio);
        if (made < blockOut) underruns++;

        if (b > 600) {                            // ignore the settling window
            const double d = double(r.depth());
            if (d < minDepth) minDepth = d;
            if (d > maxDepth) maxDepth = d;
        }
    }

    std::printf("   ratio=%.6f depth=%.0f (settled range %.0f..%.0f) underruns=%u overflows=%u\n",
                ctl.ratio(), ctl.smoothed(), minDepth, maxDepth, underruns, overflows);

    CHECK(underruns == 0, "%u underruns", underruns);
    CHECK(overflows == 0, "%u overflows", overflows);
    CHECK(maxDepth < ringFrames * 0.95, "ring nearly overflowed: max depth %.0f", maxDepth);
    CHECK(minDepth > 200.0, "ring nearly starved: min depth %.0f", minDepth);
    // The whole point: depth is held near the setpoint instead of running away.
    CHECK(std::fabs(ctl.smoothed() - target) < 400.0,
          "settled depth %.0f is far from target %.0f", ctl.smoothed(), target);
    CHECK(ctl.ratio() > RateController::kMinRatio && ctl.ratio() < RateController::kMaxRatio,
          "ratio hit the clamp: %.6f", ctl.ratio());
    // Ratio must end up near the true clock error, not somewhere arbitrary.
    const double expected = 1.0 + ppm * 1e-6;
    CHECK(std::fabs(ctl.ratio() - expected) < 30e-6,
          "ratio %.6f does not track the %.0f ppm clock error (expected ~%.6f)",
          ctl.ratio(), ppm, expected);
}

static void testControllerIsGentle() {
    std::printf("controller ramps rather than jumps\n");
    RateController ctl;
    ctl.configure(48000.0, 2400.0, 480.0 / 48000.0);

    // Slam the measured depth to a full ring and confirm the ratio creeps.
    double prev = ctl.ratio();
    double maxStep = 0.0;
    for (int i = 0; i < 500; ++i) {
        const double r = ctl.update(8000.0);
        maxStep = std::max(maxStep, std::fabs(r - prev));
        prev = r;
    }
    std::printf("   after a 5600-frame step: ratio=%.6f, largest single step=%.2e\n",
                ctl.ratio(), maxStep);
    CHECK(maxStep < 5e-5, "ratio jumped by %.2e in one update", maxStep);
    CHECK(ctl.ratio() <= RateController::kMaxRatio, "ratio exceeded clamp");
}

// Closed-loop disturbance recovery. The previous version of this test drove
// the controller with a forced constant depth, which breaks the feedback path
// and makes integrator unwinding impossible by construction -- a broken test,
// not a broken controller. This one keeps the loop closed and injects a real
// disturbance: the capture stalls for two seconds, draining the ring.
static void testDisturbanceRecovery() {
    std::printf("closed-loop recovery from a capture stall\n");

    const double   fs       = 48000.0;
    const uint32_t blockOut = 480;
    const double   target   = 2400.0;

    StereoRing r;
    r.init(8192);
    DriftResampler rs;
    rs.reset();
    RateController ctl;
    ctl.configure(fs, target, double(blockOut) / fs);

    r.beginWrite();
    for (uint32_t i = 0; i < uint32_t(target); ++i) r.writeFrame(i, 0.0f, 0.0f);
    r.endWrite(uint32_t(target));

    std::vector<float> out(blockOut * 2);
    double credit = 0.0, phase = 0.0;
    const double perBlock = blockOut * (1.0 + 80e-6);

    const int settle    = int(120 * fs / blockOut);   // 2 min to reach steady state
    const int stallFrom = settle;
    const int stallTo   = settle + int(2 * fs / blockOut);
    const int total     = settle + int(300 * fs / blockOut);

    int  underrunsAfterRecovery = 0;
    bool recovered = false;
    int  recoveryBlocks = 0;

    for (int b = 0; b < total; ++b) {
        const bool stalled = (b >= stallFrom && b < stallTo);
        if (!stalled) {
            credit += perBlock;
            uint32_t n = uint32_t(credit);
            credit -= n;
            const uint32_t space = r.beginWrite();
            if (n > space) n = space;
            for (uint32_t i = 0; i < n; ++i) {
                const float s = float(std::sin(2.0 * std::numbers::pi * 220.0 * phase / fs));
                r.writeFrame(i, s, s);
                phase += 1.0;
            }
            r.endWrite(n);
        }

        const double ratio = ctl.update(double(r.depth()));
        const uint32_t made = rs.produce(r, out.data(), blockOut, ratio);

        if (b >= stallTo) {
            if (!recovered) {
                ++recoveryBlocks;
                if (std::fabs(ctl.smoothed() - target) < 120.0) recovered = true;
            } else if (made < blockOut) {
                ++underrunsAfterRecovery;
            }
        }
    }

    const double recoverySeconds = recoveryBlocks * double(blockOut) / fs;
    std::printf("   recovered in %.1fs, final ratio=%.6f depth=%.0f\n",
                recoverySeconds, ctl.ratio(), ctl.smoothed());

    CHECK(recovered, "never returned to the setpoint after the stall");
    CHECK(recoverySeconds < 90.0, "recovery took %.1fs", recoverySeconds);
    CHECK(underrunsAfterRecovery == 0, "%d underruns after recovery", underrunsAfterRecovery);
    CHECK(std::fabs(ctl.ratio() - (1.0 + 80e-6)) < 30e-6,
          "ratio %.6f drifted off the true clock error after the disturbance", ctl.ratio());
}

// The fade exists to stop a click when a loopback source runs dry mid-block.
// That only works if the envelope reaches zero at the frame where the audio
// actually stops -- an envelope merely aiming at zero for the whole block is
// still near full amplitude when the samples run out, which is the bug this
// replaced.
// Moving the buffer setpoint on a LIVE stream, which is what the settings
// slider now does. The requirement is that the depth migrates to the new value
// without the channel ever starving -- a reconfigure() would reset the
// controller and re-prime, dropping audio, which is why setTarget exists.
static void testLiveSetpointChange(double fromMs, double toMs, const char* label) {
    std::printf("live buffer change %s (%.0fms -> %.0fms)\n", label, fromMs, toMs);

    const double   fs       = 48000.0;
    const uint32_t blockOut = 480;
    const double   ppm      = 90.0;

    auto depthFor = [&](double ms) { return fs * ms / 1000.0; };

    StereoRing r;
    r.init(65536);
    DriftResampler rs;
    rs.reset();
    RateController ctl;
    ctl.configure(fs, depthFor(fromMs), double(blockOut) / fs);

    r.beginWrite();
    for (uint32_t i = 0; i < uint32_t(depthFor(fromMs)); ++i) r.writeFrame(i, 0.0f, 0.0f);
    r.endWrite(uint32_t(depthFor(fromMs)));

    std::vector<float> out(blockOut * 2);
    double credit = 0.0, phase = 0.0;
    const double perBlock = blockOut * (1.0 + ppm * 1e-6);

    const int settle  = int(150 * fs / blockOut);   // reach steady state first
    const int changeAt = settle;
    const int total   = settle + int(400 * fs / blockOut);

    uint32_t underrunsAfterChange = 0;
    double   maxRatioStep = 0.0, prevRatio = 1.0;
    bool     arrived = false;
    int      arriveBlocks = 0;

    for (int b = 0; b < total; ++b) {
        credit += perBlock;
        uint32_t n = uint32_t(credit);
        credit -= n;
        const uint32_t space = r.beginWrite();
        if (n > space) n = space;
        for (uint32_t i = 0; i < n; ++i) {
            const float v = float(std::sin(2.0 * std::numbers::pi * 220.0 * phase / fs));
            r.writeFrame(i, v, v);
            phase += 1.0;
        }
        r.endWrite(n);

        if (b == changeAt) ctl.setTarget(depthFor(toMs));

        const double ratio = ctl.update(double(r.depth()));
        const uint32_t made = rs.produce(r, out.data(), blockOut, ratio);

        if (b > changeAt) {
            if (made < blockOut) ++underrunsAfterChange;
            maxRatioStep = std::max(maxRatioStep, std::fabs(ratio - prevRatio));
            if (!arrived) {
                ++arriveBlocks;
                if (std::fabs(ctl.smoothed() - depthFor(toMs)) < depthFor(toMs) * 0.05) arrived = true;
            }
        }
        prevRatio = ratio;
    }

    const double arriveSeconds = arriveBlocks * double(blockOut) / fs;
    std::printf("   arrived in %.1fs, depth=%.0f (target %.0f), ratio=%.6f, largest step=%.2e\n",
                arriveSeconds, ctl.smoothed(), depthFor(toMs), ctl.ratio(), maxRatioStep);

    // The whole point: no dropout while the setpoint moves.
    CHECK(underrunsAfterChange == 0, "%u underruns during the migration", underrunsAfterChange);
    CHECK(arrived, "never reached the new setpoint");
    CHECK(arriveSeconds < 120.0, "migration took %.1fs", arriveSeconds);
    // Gentleness must survive a setpoint step, not just steady state.
    CHECK(maxRatioStep <= RateController::kMaxRatioStep * 4 + 1e-12,
          "ratio jumped by %.2e during the change", maxRatioStep);
    // And the integrator must not have wound up: once settled the ratio has to
    // come back to the true clock error, not sit offset from it.
    CHECK(std::fabs(ctl.ratio() - (1.0 + ppm * 1e-6)) < 30e-6,
          "ratio %.6f drifted off the true clock error after the change", ctl.ratio());
}

static void testFadeLandsOnTheStarvePoint() {
    std::printf("fade-out lands where the audio stops\n");
    const uint32_t frames = 480, rate = 48000;

    for (uint32_t made : { 300u, 100u, 20u, 5u, 0u }) {
        FadeEnvelope env;
        env.configure(uint32_t(rate * 0.005));
        env.reset(1.0f);                       // channel was fully live
        env.beginBlock(made, frames);

        float atLastAudio = 0.0f;
        float previous = 0.0f;
        float maxStep = 0.0f;
        for (uint32_t f = 0; f < frames; ++f) {
            const float a = env.next(f);
            if (f > 0) maxStep = std::max(maxStep, std::fabs(a - previous));
            previous = a;
            if (made > 0 && f == made - 1) atLastAudio = a;
            if (f >= made) CHECK(a <= 0.0001f, "made=%u: envelope %f is not silent past the audio", made, a);
        }
        std::printf("   made=%3u -> amplitude at the last real sample = %.4f\n", made, atLastAudio);
        // The step into silence is what clicks, so it must be tiny.
        CHECK(atLastAudio < 0.06f, "made=%u: steps to silence from %f", made, atLastAudio);
        CHECK(maxStep < 0.01f, "made=%u: fade step %f is too abrupt", made, maxStep);
    }

    // A deliberate stop must keep the configured fade slope even when the
    // render period is shorter than the fade itself.
    FadeEnvelope forced;
    forced.configure(uint32_t(rate * 0.005));
    forced.reset(1.0f);
    float forcedPrevious = forced.value();
    float forcedMaxStep = 0.0f;
    for (int block = 0; block < 5; ++block) {
        const uint32_t made = std::min(48u, forced.framesUntilSilent());
        forced.beginBlock(made, 48, true);
        for (uint32_t f = 0; f < 48; ++f) {
            const float value = forced.next(f);
            forcedMaxStep = std::max(forcedMaxStep, std::fabs(value - forcedPrevious));
            forcedPrevious = value;
        }
        if (block < 4) CHECK(forced.value() > 0.0f, "forced fade ended before all short periods");
    }
    CHECK(forced.value() == 0.0f, "forced fade did not reach silence after one fade length");
    CHECK(forcedMaxStep < 0.01f, "forced fade step %f is too abrupt", forcedMaxStep);

    // A full block must not be attenuated at all.
    FadeEnvelope full;
    full.configure(uint32_t(rate * 0.005));
    full.reset(1.0f);
    full.beginBlock(frames, frames);
    for (uint32_t f = 0; f < frames; ++f) {
        CHECK(full.next(f) == 1.0f, "a full block must pass at unity");
    }

    // Coming back from silence must ramp, not jump.
    FadeEnvelope rise;
    rise.configure(uint32_t(rate * 0.005));
    rise.reset(0.0f);
    rise.beginBlock(frames, frames);
    const float first = rise.next(0);
    CHECK(first < 0.01f, "fade-in jumped straight to %f", first);
    float prev = first, maxStep = 0.0f;
    for (uint32_t f = 1; f < frames; ++f) {
        const float v = rise.next(f);
        maxStep = std::max(maxStep, v - prev);
        prev = v;
    }
    std::printf("   fade-in reaches %.3f after one block, largest step %.5f\n", prev, maxStep);
    CHECK(maxStep < 0.01f, "fade-in step %f is too abrupt", maxStep);
}

static void testMeter() {
    std::printf("meter\n");
    AtomicPeak p;
    p.publish(0.25f);
    p.publish(0.75f);
    p.publish(0.5f);
    CHECK(p.take() == 0.75f, "max-since-read failed");
    CHECK(p.take() == 0.0f, "take did not reset");

    CHECK(std::fabs(linearToDb(1.0f)) < 1e-4f, "0 dBFS");
    CHECK(std::fabs(linearToDb(0.5f) + 6.0206f) < 0.01f, "-6 dBFS");
    CHECK(dbToNorm(-60.0f) == 0.0f && dbToNorm(0.0f) == 1.0f, "norm endpoints");

    MeterBallistics m;
    m.update(1.0f, 0.016f);
    CHECK(m.levelDb() > -0.1f, "attack should be instant, got %f", m.levelDb());
    for (int i = 0; i < 200; ++i) m.update(0.0f, 0.016f);
    CHECK(m.levelDb() <= kMeterFloorDb + 0.01f, "release floor, got %f", m.levelDb());
}

static uint16_t bytesPerTestSample(SampleType type) {
    switch (type) {
        case SampleType::Float32: return 4;
        case SampleType::Float64: return 8;
        case SampleType::Int16: return 2;
        case SampleType::Int24Packed: return 3;
        case SampleType::Int32: return 4;
        default: return 0;
    }
}

static const char* sampleTypeName(SampleType type) {
    switch (type) {
        case SampleType::Float32: return "Float32";
        case SampleType::Float64: return "Float64";
        case SampleType::Int16: return "Int16";
        case SampleType::Int24Packed: return "Int24Packed";
        case SampleType::Int32: return "Int32";
        default: return "Unknown";
    }
}

static void testFormatConverterRoundTrips() {
    std::printf("sample format round-trips\n");
    constexpr uint32_t frames = 4;
    constexpr size_t guard = 16;
    const std::array<float, frames * 2> source{
        -1.25f, 1.25f, -0.75f, 0.5f, 0.125f, -0.25f, 1.2f, -1.3f};
    const std::array<SampleType, 5> types{
        SampleType::Float32, SampleType::Float64, SampleType::Int16,
        SampleType::Int24Packed, SampleType::Int32};

    StreamFormat invalidStride{};
    invalidStride.sampleRate = 48000;
    invalidStride.channels = 2;
    invalidStride.type = SampleType::Float32;
    invalidStride.blockAlign = 7;
    CHECK(!invalidStride.valid(), "format rejects a block stride smaller than one frame");

    FormatConverter invalidConverter;
    invalidConverter.configure(invalidStride);
    std::array<uint8_t, 16> invalidDestination{};
    invalidDestination.fill(0xA5);
    const std::array<float, 2> invalidSource{0.5f, -0.5f};
    invalidConverter.fromStereoFloat(invalidSource.data(), invalidDestination.data(), 1);
    CHECK(std::all_of(invalidDestination.begin(), invalidDestination.end(),
                      [](uint8_t byte) { return byte == 0xA5; }),
          "invalid output stride is not written");
    std::array<float, 2> invalidDecoded{1.0f, 1.0f};
    invalidConverter.toStereoFloat(invalidDestination.data(), invalidDecoded.data(), 1);
    CHECK(invalidDecoded[0] == 0.0f && invalidDecoded[1] == 0.0f,
          "invalid input stride decodes as silence");

    for (const SampleType type : types) {
        for (const uint16_t channels : {uint16_t{1}, uint16_t{2}, uint16_t{8}}) {
            StreamFormat format{};
            format.sampleRate = 48000;
            format.channels = channels;
            format.type = type;
            format.bitsPerSample = static_cast<uint16_t>(bytesPerTestSample(type) * 8);
            format.validBits = format.bitsPerSample;
            format.blockAlign = static_cast<uint16_t>(channels * bytesPerTestSample(type));
            format.channelMask = channels == 8 ? 0x63Fu : (channels == 2 ? 0x3u : 0u);

            FormatConverter converter;
            converter.configure(format);
            const size_t payload = static_cast<size_t>(frames) * format.blockAlign;
            std::vector<uint8_t> encoded(guard + payload + guard, 0xA5);
            converter.fromStereoFloat(source.data(), encoded.data() + guard, frames);

            bool guardsIntact = true;
            for (size_t i = 0; i < guard; ++i) {
                guardsIntact &= encoded[i] == 0xA5;
                guardsIntact &= encoded[guard + payload + i] == 0xA5;
            }
            CHECK(guardsIntact, "%s %u-channel conversion overwrote guard bytes",
                  sampleTypeName(type), channels);

            std::array<float, frames * 2> decoded{};
            converter.toStereoFloat(encoded.data() + guard, decoded.data(), frames);
            const float tolerance = type == SampleType::Int16 ? 7e-5f : 2e-6f;
            for (uint32_t frame = 0; frame < frames; ++frame) {
                float expectedLeft = std::clamp(source[frame * 2], -1.0f, 1.0f);
                float expectedRight = std::clamp(source[frame * 2 + 1], -1.0f, 1.0f);
                if (channels == 1) {
                    expectedLeft = expectedRight = std::clamp(
                        (source[frame * 2] + source[frame * 2 + 1]) * 0.5f,
                        -1.0f, 1.0f);
                }
                CHECK(std::fabs(decoded[frame * 2] - expectedLeft) <= tolerance &&
                          std::fabs(decoded[frame * 2 + 1] - expectedRight) <= tolerance,
                      "%s %u-channel frame %u round-trip = %.7f/%.7f, expected %.7f/%.7f",
                      sampleTypeName(type), channels, frame, decoded[frame * 2],
                      decoded[frame * 2 + 1], expectedLeft, expectedRight);
            }
        }
    }

    for (const SampleType type : types) {
        StreamFormat format{};
        format.sampleRate = 48000;
        format.channels = 2;
        format.type = type;
        format.bitsPerSample = static_cast<uint16_t>(bytesPerTestSample(type) * 8);
        format.validBits = format.bitsPerSample;
        format.blockAlign = static_cast<uint16_t>(2 * bytesPerTestSample(type));
        format.channelMask = 0x3u;

        FormatConverter converter;
        converter.configure(format);
        const std::array<float, 6> nonFinite{
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()};
        std::vector<uint8_t> encoded(static_cast<size_t>(3) * format.blockAlign, 0xA5);
        converter.fromStereoFloat(nonFinite.data(), encoded.data(), 3);
        std::array<float, 6> decoded{};
        converter.toStereoFloat(encoded.data(), decoded.data(), 3);
        CHECK(std::all_of(decoded.begin(), decoded.end(),
                          [](float value) { return value == 0.0f; }),
              "%s non-finite samples encode as silence", sampleTypeName(type));
    }

    StreamFormat surround{};
    surround.sampleRate = 48000;
    surround.channels = 8;
    surround.type = SampleType::Float32;
    surround.bitsPerSample = surround.validBits = 32;
    surround.blockAlign = 8 * sizeof(float);
    surround.channelMask = 0x63Fu;
    FormatConverter surroundConverter;
    surroundConverter.configure(surround);
    constexpr float kMinus3Db = 0.7071068f;
    const std::array<std::array<float, 2>, 6> expectedSurround{{
        {kMinus3Db, kMinus3Db}, {0.0f, 0.0f},
        {kMinus3Db, 0.0f}, {0.0f, kMinus3Db},
        {kMinus3Db, 0.0f}, {0.0f, kMinus3Db}}};
    for (uint16_t channel = 2; channel < 8; ++channel) {
        std::array<float, 8> frame{};
        frame[channel] = 1.0f;
        std::array<float, 2> decoded{};
        surroundConverter.toStereoFloat(frame.data(), decoded.data(), 1);
        const auto& expected = expectedSurround[channel - 2];
        CHECK(std::fabs(decoded[0] - expected[0]) < 1e-6f &&
                  std::fabs(decoded[1] - expected[1]) < 1e-6f,
              "8-channel input %u downmix = %.7f/%.7f, expected %.7f/%.7f",
              channel, decoded[0], decoded[1], expected[0], expected[1]);
    }
}

static void testJson() {
    std::printf("json\n");
    JsonValue root = JsonValue::object();
    root.set("version", JsonValue(1));
    JsonValue ch = JsonValue::object();
    ch.set("name",   JsonValue(std::string("Game \"Audio\"\n\ttab")));
    ch.set("gain",   JsonValue(0.7));
    ch.set("muted",  JsonValue(false));
    ch.set("id",     JsonValue(std::string("{0.0.0.00000000}.{abc-123}")));
    root.set("game", ch);

    const std::string text = root.dump(2);
    std::string err;
    JsonValue back = JsonValue::parse(text, &err);
    CHECK(err.empty(), "parse error: %s", err.c_str());
    CHECK(back.isObject(), "root not object");

    const JsonValue* g = back.find("game");
    CHECK(g != nullptr, "missing game");
    if (g) {
        CHECK(g->find("name")->asString("") == "Game \"Audio\"\n\ttab", "escape round-trip");
        CHECK(std::fabs(g->find("gain")->asNumber(0) - 0.7) < 1e-9, "number round-trip");
        CHECK(g->find("muted")->asBool(true) == false, "bool round-trip");
        CHECK(g->find("id")->asString("") == "{0.0.0.00000000}.{abc-123}", "device id round-trip");
    }

    // Defaults on a corrupt or partial file.
    JsonValue bad = JsonValue::parse("{ not json", &err);
    CHECK(!err.empty(), "bad json should report an error");
    CHECK(bad.find("anything") == nullptr, "corrupt root must not yield keys");
    CHECK(back.find("nope") == nullptr, "missing key");
    CHECK(JsonValue().asNumber(42.0) == 42.0, "default fallback");

    // Unicode escape handling.
    JsonValue u = JsonValue::parse("{\"k\":\"\\u00e9\\u0041\"}", &err);
    CHECK(err.empty(), "unicode parse: %s", err.c_str());
    CHECK(u.find("k")->asString("") == "\xc3\xa9" "A", "utf8 decode");

    JsonValue bom = JsonValue::parse("\xEF\xBB\xBF{\"bufferMillis\":90}", &err);
    CHECK(err.empty() && bom.find("bufferMillis") &&
              bom.find("bufferMillis")->asNumber(0) == 90,
          "UTF-8 BOM is accepted");
}

static void testRetryPolicy() {
    std::printf("retry policy\n");
    CHECK(!shouldRestart(StreamState::Running, true, false), "running stream must not restart");
    CHECK(!shouldRestart(StreamState::Opening, true, true), "opening stream must not restart");
    CHECK(!shouldRestart(StreamState::Stopped, true, true), "stopped stream must not restart");
    CHECK(shouldRestart(StreamState::Failed, true, false), "retryable failure restarts");
    CHECK(!shouldRestart(StreamState::Failed, false, false), "ambiguous failure waits");
    CHECK(shouldRestart(StreamState::Failed, false, true), "device notification retries ambiguity");
    CHECK(retryBackoffMillis(0) == 0 && retryBackoffMillis(1) == 2000 &&
              retryBackoffMillis(2) == 4000 && retryBackoffMillis(5) == 30000 &&
              retryBackoffMillis(20) == 30000,
          "retry backoff doubles and caps at 30 seconds");
}

static void testMixModeAndRouting() {
    MixMode mode;
    float l=.8f,r=.2f;
    mode.process(l,r,false,1.0f);
    CHECK(std::fabs(l-.8f)<1e-6 && std::fabs(r-.2f)<1e-6,"stereo retains left/right");
    mode.process(l,r,true,1.0f);
    CHECK(std::fabs(l-.5f)<1e-6 && l==r,"mono averages, does not double gain");
    l=1;r=-1;mode.process(l,r,true,1.0f);
    CHECK(l==0 && r==0,"mono cancels opposite polarity");
    mode.reset(false);float previous=1;
    for(int i=0;i<240;++i){l=1;r=0;mode.process(l,r,true,1.f/240);CHECK(std::fabs(l-previous)<.003f,"mode switch ramps smoothly");previous=l;}
    CHECK(std::fabs(l-r)<.0001f,"mono ramp converges in 5 ms at 48 kHz");
    SourceRoute device;device.audible=true;device.endpoint=L"speakers";
    SourceRoute app;app.kind=SourceKind::Application;app.audible=true;app.processId=42;app.routingKnown=true;app.appEndpoints={L"speakers"};
    CHECK(sourceOverlap(device,app)==Overlap::Confirmed,"app routed through captured device warns");
    app.appEndpoints={L"chat"};CHECK(sourceOverlap(device,app)==Overlap::None,"separate playback route does not warn");
    app.routingKnown=false;CHECK(sourceOverlap(device,app)==Overlap::Possible,"unknown routing qualified as possible");
    app.audible=false;CHECK(sourceOverlap(device,app)==Overlap::None,"muting/excluding clears warning");
    CHECK(sourceOverlap(device,device)==Overlap::Confirmed,"same endpoint twice warns");
    SourceRoute mic=device;mic.kind=SourceKind::Microphone;CHECK(sourceOverlap(device,mic)==Overlap::None,"microphone is not playback overlap");
    app.audible=true;CHECK(sourceOverlap(app,app)==Overlap::Confirmed,"same app twice warns");
    StereoRing visual;visual.init(4096);Spectrum spectrum;
    for(uint32_t i=0;i<2048;++i){float v=std::sin(6.283185307f*float(i)*1000.f/48000.f);visual.writeFrame(i,v,-v);}visual.endWrite(2048);
    spectrum.update(visual,48000,.016f,true);
    auto peak=std::max_element(spectrum.levels.begin(),spectrum.levels.end());
    CHECK(*peak>-2.f,"spectrum sees opposite-polarity stereo without cancellation");
    CHECK(peak-spectrum.levels.begin()>30 && peak-spectrum.levels.begin()<38,"1 kHz maps to the correct log frequency band");
    for(int i=0;i<200;++i)spectrum.update(visual,48000,.016f,false);
    CHECK(*std::max_element(spectrum.levels.begin(),spectrum.levels.end())<=-60.f,"stopped spectrum decays to silence");
}

int main() {
    std::printf("== audio-monitor DSP/config tests ==\n\n");
    testRing();
    testResamplerUnityIsExact();
    testResamplerInterpolates();
    testDriftConvergence(+100.0, "producer fast");
    testDriftConvergence(-100.0, "producer slow");
    testDriftConvergence(+400.0, "extreme");
    testControllerIsGentle();
    testDisturbanceRecovery();
    testLiveSetpointChange(50.0, 150.0, "increase");
    testLiveSetpointChange(150.0, 40.0, "decrease");
    testLiveSetpointChange(250.0, 20.0, "full decrease");
    testLiveSetpointChange(150.0, 20.0, "decrease to minimum");
    testFadeLandsOnTheStarvePoint();
    testMeter();
    testFormatConverterRoundTrips();
    testJson();
    testRetryPolicy();
    testMixModeAndRouting();

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
