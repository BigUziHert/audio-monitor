// Host-native tests for the parts of the mixer that are not Windows-specific:
// the SPSC ring, the drift resampler, the rate controller and the config JSON.
//
// These run on any platform (see scripts/run_tests.sh) and are the only part
// of the project that can be executed without the real audio hardware.

#include "audio/RingBuffer.h"
#include "audio/Resampler.h"
#include "audio/RateController.h"
#include "audio/Meter.h"
#include "config/Json.h"

#include <cmath>
#include <cstdio>
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

// Feeds a sine through the resampler at ratio 1.0 and checks it survives.
static void testResamplerFidelity() {
    std::printf("resampler fidelity (ratio 1.0)\n");
    StereoRing r;
    r.init(8192);
    DriftResampler rs;
    rs.reset();

    const double freq = 440.0, fs = 48000.0;
    double       phase = 0.0;
    std::vector<float> out(512 * 2);

    double maxErr = 0.0;
    long   n      = 0;
    for (int block = 0; block < 40; ++block) {
        const uint32_t space = r.beginWrite();
        uint32_t w = 0;
        for (; w < space && w < 600; ++w) {
            const float s = float(std::sin(2.0 * M_PI * freq * phase / fs));
            r.writeFrame(w, s, s);
            phase += 1.0;
        }
        r.endWrite(w);

        const uint32_t made = rs.produce(r, out.data(), 512, 1.0);
        if (block < 2) continue;                     // let the kernel prime
        for (uint32_t i = 0; i < made; ++i) {
            // At ratio 1.0 Catmull-Rom is an identity on the sample grid, so
            // the only error is the fixed 1-sample kernel latency.
            ++n;
            const double err = std::fabs(double(out[i * 2]) - double(out[i * 2 + 1]));
            if (err > maxErr) maxErr = err;
        }
    }
    CHECK(n > 10000, "only produced %ld frames", n);
    CHECK(maxErr < 1e-6, "L/R diverged, max err %g", maxErr);
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
    ctl.configure(fs, target);

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
            const float s = float(std::sin(2.0 * M_PI * 220.0 * phase / fs));
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
    ctl.configure(48000.0, 2400.0);

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
    ctl.configure(fs, target, blockOut);

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
                const float s = float(std::sin(2.0 * M_PI * 220.0 * phase / fs));
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
}

int main() {
    std::printf("== audio-monitor DSP/config tests ==\n\n");
    testRing();
    testResamplerFidelity();
    testDriftConvergence(+100.0, "producer fast");
    testDriftConvergence(-100.0, "producer slow");
    testDriftConvergence(+400.0, "extreme");
    testControllerIsGentle();
    testDisturbanceRecovery();
    testMeter();
    testJson();

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
