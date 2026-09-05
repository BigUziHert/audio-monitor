#include "audio/AudioEngine.h"
#include "audio/Diagnostics.h"
#include "audio/OutputBus.h"
#include "util/Log.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace audiomon {
struct AudioEngineTestAccess {
    static void prepare(AudioEngine& engine) {
        engine.sourceCount_ = 1;
        engine.outputCount_ = 0;
        engine.config_.sources.resize(1);
        engine.config_.clearOutputs();
        engine.diagnosticSession_ = 1;
        engine.diagnostics_.beginSession(1, "synthetic configured selection");
        auto& ch = *engine.channels_[0];
        ch.stream.configure("synthetic", CaptureMode::Loopback);
        ch.stream.sampleRate_.store(48000);
        ch.stream.state_.store(StreamState::Running);
        ch.stream.flowing_.store(true);
        ch.stream.resolvedName_ = L"retained-test-device";
        ch.stream.resolvedId_ = L"retained-test-id";
        ch.stream.lastError_ = "retained-test-error";
        ch.stream.diagnosticFormat_ = "retained-test-format";
        engine.setBufferMillis(20);
        engine.onRenderFormat(48000, 480);
        ch.lastEpoch = ch.stream.epoch(); // injection starts on the acknowledged timeline
        engine.running_.store(true);
        const auto count = std::min<uint32_t>(1440, ch.stream.ring().beginWrite());
        for (uint32_t i = 0; i < count; ++i) ch.stream.ring().writeFrame(i, 0.1f, 0.1f);
        ch.stream.ring().endWrite(count);
    }
    static DiagnosticSample sample(const AudioEngine& engine) { return engine.collectDiagnostics(); }
    static void prepareOutput(AudioEngine& engine) {
        engine.config_.hasOutput = true;
        engine.config_.output.deviceNameMatch = L"synthetic output selection";
        engine.outputCount_ = 1;
        auto& render = *engine.renders_[0];
        render.state_.store(StreamState::Running);
        render.resolvedName_ = L"retained-output-device";
        render.resolvedId_ = L"retained-output-id";
        render.diagnosticFormat_ = "retained-output-format";
        render.lastError_ = "retained-output-error";
        engine.recordDiagnostics(); // the periodic active observation
    }
    static void pauseAndDisable(AudioEngine& engine) {
        engine.renders_[0]->stop();
        engine.config_.sources[0].enabled = false;
        engine.channels_[0]->stream.stop();
        engine.recordDiagnostics(); // later paused observations must not erase identities
        engine.recordDiagnostics();
    }
    static void changeSelections(AudioEngine& engine) {
        engine.config_.sources[0].deviceNameMatch = L"new source";
        engine.config_.output.deviceNameMatch = L"new output";
    }
};
}

namespace {
using namespace audiomon;
int failures = 0;
void check(bool passed, const char* label) {
    if (!passed) { std::printf("FAIL: %s\n", label); ++failures; }
}
bool nearlyEqual(double a, double b) { return std::fabs(a-b) < 0.001; }

DiagnosticSample sampleAt(uint64_t elapsed, uint32_t depth = 960) {
    DiagnosticSample sample;
    sample.session = 1; sample.elapsedMillis = elapsed; sample.utcMillis = 1700000000000ull + elapsed;
    sample.running = true; sample.monitoring = true; sample.monitoringGeneration = 3;
    sample.sourceCount = 1; sample.outputCount = 1; sample.bufferMillis = 20;
    DiagnosticStream stream;
    stream.state = StreamState::Running; stream.valid = true; stream.flowing = true;
    stream.priming = false; stream.epoch = 3; stream.queueRate = 48000; stream.nativeRate = 48000;
    stream.targetFrames = 960; stream.queueFrames = depth; stream.capacityFrames = 8192;
    stream.correctionPpm = 200;
    sample.sources[0] = sample.outputs[0] = stream;
    return sample;
}

void trends() {
    std::vector<DiagnosticSample> samples;
    for (uint64_t i = 0; i <= 60; ++i) samples.push_back(sampleAt(i*5000, 960 + uint32_t(i*24)));
    const auto growth = diagnosticQueueTrend(samples, false, 0);
    check(growth.growing && growth.samples == 61 && nearlyEqual(growth.slopeMillisPerMinute, 6),
          "known 6 ms/min backlog growth detected");
    check(nearlyEqual(growth.meanCorrectionPpm, 200), "correction ppm is separate from nominal rate conversion");
    auto stopped = samples.back(); stopped.elapsedMillis += 5000; stopped.running = stopped.monitoring = false;
    stopped.sources[0].valid = stopped.outputs[0].valid = false;
    samples.push_back(stopped);
    check(diagnosticQueueTrend(samples, true, 0).growing, "stop retains newest active output trend");
    samples.back() = sampleAt(305000, 1500); samples.back().sources[0].epoch++;
    check(diagnosticQueueTrend(samples, false, 0).samples == 1, "source epoch breaks comparison");
    samples.back() = sampleAt(305000, 1500); samples.back().outputs[0].targetFrames++;
    check(diagnosticQueueTrend(samples, true, 0).samples == 1, "buffer slider is not clock drift");
    samples.back() = sampleAt(305000, 1500); samples.back().monitoringGeneration += 2;
    check(diagnosticQueueTrend(samples, true, 0).samples == 1, "pause/resume generation breaks output comparison");
    samples.back() = sampleAt(305000, 1500); samples.back().sources[0].starvationEvents++;
    check(diagnosticQueueTrend(samples, false, 0).samples == 1, "shortfall between samples breaks trend");
    samples.back().session++;
    check(diagnosticQueueTrend(samples, false, 0).samples == 1, "engine restart never splices sessions");
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = sampleAt(i*5000, 960 + (i%2 ? 240 : 0));
    check(!diagnosticQueueTrend(samples, false, 0).growing, "packet phase oscillation is not sustained growth");
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = sampleAt(i*5000, 2600 - uint32_t(i*24));
    check(!diagnosticQueueTrend(samples, false, 0).growing, "draining backlog is not growing latency");
    samples.resize(3);
    check(!diagnosticQueueTrend(samples, false, 0).growing, "short observations do not assert drift");
    samples.clear();
    for (uint64_t seconds = 0; seconds <= 3900; seconds += 5) {
        const auto depth = 960 + static_cast<uint32_t>(seconds > 3600 ? (seconds-3600)*480/300 : 0);
        samples.push_back(sampleAt(seconds*1000, depth));
    }
    const auto recent = diagnosticQueueTrend(samples, false, 0);
    check(recent.growing && recent.durationSeconds <= 300 && nearlyEqual(recent.slopeMillisPerMinute, 2),
          "one healthy hour cannot dilute a new five-minute 10 ms leak");
    for (const uint64_t period : {100ull, 150ull, 200ull}) {
        samples.clear();
        for (uint64_t seconds = 0; seconds <= 900; seconds += 5)
            samples.push_back(sampleAt(seconds*1000, 960 + static_cast<uint32_t>((seconds%period)*480/period)));
        check(!diagnosticQueueTrend(samples, false, 0).growing,
              "slow bounded 10 ms packet-phase sawtooth does not assert a leak");
    }
    samples.clear();
    for (uint64_t seconds = 0; seconds <= 60; seconds += 5)
        samples.push_back(sampleAt(seconds*1000, 960 + static_cast<uint32_t>(seconds*8)));
    check(!diagnosticQueueTrend(samples, false, 0).growing, "one-minute phase ramp is insufficient evidence");
}

void historyAndReport() {
    DiagnosticHistory history;
    for (uint64_t i = 1; i <= 40; ++i) history.beginSession(i, "test topology");
    for (size_t i = 0; i < DiagnosticHistory::kMaxSamples + 10; ++i)
        history.record(sampleAt(i*5000));
    const auto copy = history.copy();
    check(copy.samples.size() == DiagnosticHistory::kMaxSamples && copy.discardedSamples == 10,
          "sample history bounded with explicit eviction count");
    check(copy.sessions.size() == 32 && copy.sessions.front().id == 9, "topology history bounded");
    check(copy.samples.front().elapsedMillis == 50000, "oldest sample evicted in order");
    auto current = copy.samples.back(); current.outputs[0].latencyCorrections = 7;
    const auto report = formatDiagnosticReport(current, copy, "Synthetic device", "log-tail-marker");
    check(report.find("NOT measured end-to-end latency or A/V sync") != std::string::npos,
          "report does not claim measured A/V sync");
    check(report.find("source-native frames") != std::string::npos &&
          report.find("output-native frames") != std::string::npos, "report identifies distinct frame units");
    check(report.find("latency_correction_events") != std::string::npos &&
          report.find("log-tail-marker") != std::string::npos && report.find("Synthetic device") != std::string::npos,
          "report includes recoveries, runtime log and device context");
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        for (uint64_t i = 0; i < 2000; ++i) history.record(sampleAt(8000000 + i*5000));
        finished.store(true, std::memory_order_release);
    });
    do { check(history.copy().samples.size() <= DiagnosticHistory::kMaxSamples, "concurrent history copy bounded"); }
    while (!finished.load(std::memory_order_acquire));
    writer.join();
}

void audioMirrors() {
    AudioEngine engine;
    check(engine.diagnosticReport().find("Configured sources: 0; outputs: 0") != std::string::npos,
          "fresh engine does not invent a configured output");
    AudioEngineTestAccess::prepare(engine);
    float block[960]{};
    engine.renderMix(block, 480);
    const auto first = AudioEngineTestAccess::sample(engine);
    check(first.sources[0].valid && first.sources[0].queueRate == 48000 &&
          first.sources[0].targetFrames == 960 && first.sources[0].queueFrames == 1440,
          "source diagnostics use actual consumer depth and native target");
    for (int i = 0; i < 5; ++i) engine.renderMix(block, 480);
    const auto starved = AudioEngineTestAccess::sample(engine);
    check(starved.sources[0].starvedFrames > 0 && starved.sources[0].starvationEvents > 0,
          "flowing source shortfall records frames and events");
    engine.stop();
    const auto stoppedReport = engine.diagnosticReport();
    check(stoppedReport.find("retained-test-device") != std::string::npos &&
          stoppedReport.find("retained-test-id") != std::string::npos &&
          stoppedReport.find("retained-test-error") != std::string::npos &&
          stoppedReport.find("retained-test-format") != std::string::npos,
          "final session sample retains capture identity, format and error after stop clears runtime");
    AudioEngine retained;
    AudioEngineTestAccess::prepare(retained);
    AudioEngineTestAccess::prepareOutput(retained);
    AudioEngineTestAccess::pauseAndDisable(retained);
    retained.stop();
    const auto retainedReport = retained.diagnosticReport();
    const auto currentBegin = retainedReport.find("CURRENT DEVICE DETAILS");
    const auto currentEnd = retainedReport.find("SESSION TOPOLOGIES", currentBegin);
    const auto currentDevices = retainedReport.substr(currentBegin, currentEnd-currentBegin);
    check(currentDevices.find("retained-test-device") != std::string::npos &&
          currentDevices.find("retained-output-device") != std::string::npos &&
          currentDevices.find("retained-output-error") != std::string::npos &&
          currentDevices.find("retained-output-format") != std::string::npos,
          "periodic paused samples retain last-known output and disabled-source runtime details");
    AudioEngineTestAccess::changeSelections(retained);
    const auto changedReport = retained.diagnosticReport();
    const auto changedBegin = changedReport.find("CURRENT DEVICE DETAILS");
    const auto changedDevices = changedReport.substr(changedBegin,
        changedReport.find("SESSION TOPOLOGIES", changedBegin)-changedBegin);
    check(changedDevices.find("retained-test-device") == std::string::npos &&
          changedDevices.find("retained-output-device") == std::string::npos,
          "last-known runtime identity is never attributed to a new route selection");
    OutputBus bus;
    bus.onRenderFormat(48000, 480); bus.onRenderPeriod(480);
    bus.publish(nullptr, 1440, 1, false); bus.renderMix(block, 480);
    check(bus.diagnosticDepthFrames() == 1440 && bus.diagnosticDepthFrames() <= bus.capacityFrames(),
          "output observer uses bounded consumer-published depth");
}

void lifecycleExport() {
    AudioEngine engine;
    std::atomic<bool> done{false};
    std::atomic<int> starts{0};
    std::thread worker([&] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Config config; config.clearOutputs(); config.sources.resize(1);
        config.sources[0].enabled = false; config.sources[0].label = "disabled diagnostic fixture";
        for (int i = 0; i < 12; ++i) {
            if (engine.start(config, false)) starts.fetch_add(1);
            engine.stop();
        }
        done.store(true, std::memory_order_release);
        if (SUCCEEDED(hr)) CoUninitialize();
    });
    size_t exports = 0;
    do {
        const auto report = engine.diagnosticReport();
        check(report.find("Audio Monitor audio diagnostics v1") == 0, "concurrent lifecycle export completes");
        const auto header = report.find("Session: ") + 9;
        const auto session = std::stoull(report.substr(header));
        size_t row = 0;
        while ((row = report.find("\ncurrent,", row)) != std::string::npos) {
            check(std::stoull(report.substr(row + 9)) == session, "report current rows cannot splice sessions");
            ++row;
        }
        ++exports;
        std::this_thread::yield();
    } while (!done.load(std::memory_order_acquire));
    worker.join();
    check(starts.load() == 12 && exports > 0, "hardware-free lifecycle/export stress exercised real sessions");
    check(engine.diagnosticReport().find("Session 1 (initial configured selections):") != std::string::npos,
          "prior session topology survives stop and restart");
}

void recentLog() {
    const std::string payload(950, 'x');
    for (int i = 0; i < 160; ++i) log::write("TEST", "%d %s", i, payload.c_str());
    log::write("TEST", "final-diagnostic-log-marker");
    const auto tail = log::recentText();
    check(tail.size() <= 128*1024 && tail.find("final-diagnostic-log-marker") != std::string::npos,
          "recent log tail bounded and available without an open file");
}
}

int main() {
    trends(); historyAndReport(); audioMirrors(); lifecycleExport(); recentLog();
    std::printf("Audio diagnostics: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
