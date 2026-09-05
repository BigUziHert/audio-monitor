#include "audio/OutputBus.h"
#include "audio/RateController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace audiomon;

namespace {
int failures = 0;
void check(bool ok, const char* description) {
    if (!ok) { std::printf("FAIL: %s\n", description); ++failures; }
}

// Independent producer/consumer schedules exercise packet-phase changes as
// well as resampler correction. Time is simulated, so twenty minutes of clock
// drift can be checked without twenty minutes of wall-clock waiting.
void driftSession(uint32_t nativeRate, uint32_t callbackFrames, double ppm, double seconds) {
    OutputBus bus;
    bus.onRenderFormat(nativeRate, callbackFrames * 2);
    bus.onRenderPeriod(callbackFrames);
    std::vector<float> source(480 * 2, 0.3f);
    std::vector<float> output(callbackFrames * 2);
    const double outputPeriod = double(callbackFrames) / (double(nativeRate) * (1.0 + ppm * 1e-6));
    double nextProduce = 0.0, nextRender = 0.003;
    double ratioSum = 0.0;
    uint64_t ratioSamples = 0;
    uint32_t maximumDepth = 0;
    while (std::min(nextProduce, nextRender) < seconds) {
        if (nextProduce <= nextRender) {
            bus.publish(source.data(), 480, 1.0f, false);
            nextProduce += 0.01;
        } else {
            bus.renderMix(output.data(), callbackFrames);
            if (nextRender >= seconds - 300.0) {
                ratioSum += bus.resamplingRatio();
                ++ratioSamples;
            }
            maximumDepth = std::max(maximumDepth, bus.depthFrames());
            nextRender += outputPeriod;
        }
    }
    const double expected = 48000.0 / (double(nativeRate) * (1.0 + ppm * 1e-6));
    const double meanRatio = ratioSamples ? ratioSum / double(ratioSamples) : 0.0;
    std::printf("output drift %u Hz / %u frames / %+.0f ppm: ratio %.8f, expected %.8f, "
                "max depth %u, starved %llu, dropped %llu, trims %llu\n",
                nativeRate, callbackFrames, ppm, meanRatio, expected, maximumDepth,
                static_cast<unsigned long long>(bus.starvedFrames()),
                static_cast<unsigned long long>(bus.droppedFrames()),
                static_cast<unsigned long long>(bus.latencyCorrections()));
    check(bus.starvedFrames() == 0, "sustained independent clocks never starve an output");
    check(bus.droppedFrames() == 0, "sustained independent clocks never overflow or trim audio");
    check(bus.latencyCorrections() == 0, "normal crystal drift does not trigger latency correction");
    check(maximumDepth < bus.effectiveTargetFrames() + 960,
          "long-session output queue remains near its setpoint");
    check(std::fabs(meanRatio / expected - 1.0) < 30e-6,
          "long-window output correction tracks clock drift despite producer packet phase");
}

void latencyRecovery() {
    OutputBus bus;
    bus.onRenderFormat(48000, 960); // shared maximum, normally 480-frame callbacks
    check(bus.effectiveTargetFrames() > 960, "startup reserves the advertised maximum period");
    bus.onRenderPeriod(480);
    std::vector<float> source(480 * 2, 0.5f), output(480 * 2);
    bus.publish(source.data(), 480, 1.0f, false);
    bus.renderMix(output.data(), 480);
    check(bus.effectiveTargetFrames() == 1200,
          "reported 10 ms endpoint period removes the extra maximum-buffer reserve");
    for (int i = 0; i < 8; ++i) {
        bus.publish(source.data(), 480, 1.0f, false);
        bus.renderMix(output.data(), 480);
    }
    check(!bus.priming() && output.back() > 0.45f, "output is audible before a render hitch");
    // The producer stays healthy while rendering is descheduled for 60 ms.
    for (int i = 0; i < 6; ++i) bus.publish(source.data(), 480, 1.0f, false);
    check(bus.depthFrames() > 1920, "hitch created a real queued-audio backlog");
    bus.renderMix(output.data(), 480);
    check(output.front() > 0.45f && output.back() == 0.0f,
          "latency correction fades the old stream to silence");
    check(bus.latencyCorrections() == 1 && bus.depthFrames() == bus.effectiveTargetFrames(),
          "one callback discards excess latency and retains the newest normal-sized tail");
    bus.renderMix(output.data(), 480);
    check(!bus.priming() && output.back() > 0.45f,
          "retained live tail resumes without waiting to refill the queue");
    for (int i = 0; i < 200; ++i) {
        bus.publish(source.data(), 480, 1.0f, false);
        bus.renderMix(output.data(), 480);
    }
    check(bus.latencyCorrections() == 1 && bus.starvedFrames() == 0,
          "hitch recovery does not cause repeated corrections or later starvation");
    bus.clearStatistics();
    check(bus.latencyCorrections() == 0 && bus.droppedFrames() == 0,
          "session reset clears latency diagnostics");
}

void interruptedTrim() {
    OutputBus bus;
    bus.onRenderFormat(48000, 48); // 1 ms callback spans a 5 ms fade
    std::vector<float> source(1440 * 2, 0.5f), output(48 * 2);
    bus.publish(source.data(), 1440, 1.0f, false);
    for (int i = 0; i < 5; ++i) bus.renderMix(output.data(), 48);
    bus.publish(source.data(), 1440, 1.0f, false);
    bus.renderMix(output.data(), 48);
    check(output.back() > 0.0f, "short callback preserves the configured latency-correction fade");
    bus.requestReset(); // real discontinuity invalidates the retained-tail option
    for (int i = 0; i < 5; ++i) bus.renderMix(output.data(), 48);
    check(bus.depthFrames() == 0 && bus.latencyCorrections() == 0,
          "a real discontinuity during latency correction discards the entire old timeline");
}

void controllerPeriodInvariance() {
    double reference = 0.0;
    for (double period : {0.01, 0.005, 0.02}) {
        RateController controller;
        controller.configure(48000.0, 960.0, period);
        const int steps = static_cast<int>(std::lround(1.0 / period));
        for (int i = 0; i < steps; ++i) controller.update(8000.0);
        if (period == 0.01) reference = controller.ratio();
        check(std::fabs(controller.ratio() - reference) < 1e-10,
              "controller slew per second is independent of callback size");
    }
}

void variableLongCallbacks() {
    OutputBus bus;
    bus.onRenderFormat(48000, 3840); // shared capacity: two 40 ms periods
    const uint32_t maximumReserve = bus.effectiveTargetFrames();
    bus.onRenderPeriod(1920);
    const uint32_t normalReserve = bus.effectiveTargetFrames();
    check(normalReserve > 1920 && normalReserve < maximumReserve,
          "native period metadata avoids reserving two long endpoint periods twice");
    std::vector<float> source(4096 * 2, 0.5f), output(1920 * 2);
    bus.publish(source.data(), 4096, 1.0f, false);
    bus.renderMix(output.data(), 1); // one-off partial shared callback
    for (int i = 0; i < 20; ++i) {
        bus.renderMix(output.data(), 1920);
        bus.publish(source.data(), 1920, 1.0f, false);
    }
    check(bus.effectiveTargetFrames() == normalReserve && bus.starvedFrames() == 0,
          "a tiny partial callback cannot shrink the safety target for a long-period device");
}
}

int main() {
    controllerPeriodInvariance();
    variableLongCallbacks();
    latencyRecovery();
    interruptedTrim();
    driftSession(48000, 480, +400.0, 1200.0);
    driftSession(48000, 480, -400.0, 1200.0);
    driftSession(44100, 441, +150.0, 600.0);
    driftSession(96000, 480, -150.0, 600.0);
    std::printf("Output clock/latency test: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
