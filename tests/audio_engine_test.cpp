#include "audio/AudioEngine.h"
#include "audio/OutputBus.h"
#include "audio/WaveFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>

namespace audiomon {

struct FakeMixPumpSchedulerApi {
    static HANDLE stopHandle() noexcept {
        return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
    }
    static HANDLE timerHandle() noexcept {
        return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(2));
    }

    bool failStop = false;
    bool failHighResolution = false;
    bool failNormalTimer = false;
    bool failSetTimer = false;
    DWORD waitResult = WAIT_OBJECT_0 + 1;
    DWORD error = ERROR_GEN_FAILURE;
    int stopCreates = 0;
    int highTimerCreates = 0;
    int normalTimerCreates = 0;
    int setCalls = 0;
    int waitCalls = 0;
    int stopWaitCalls = 0;
    int signalCalls = 0;
    int cancelCalls = 0;
    int stopCloses = 0;
    int timerCloses = 0;
    int64_t lastDueTime100ns = 0;
    DWORD lastStopTimeoutMillis = 0;
    DWORD stopWaitResult = WAIT_TIMEOUT;

    static HANDLE createStop(void* context) noexcept {
        auto& fake = *static_cast<FakeMixPumpSchedulerApi*>(context);
        ++fake.stopCreates;
        return fake.failStop ? nullptr : stopHandle();
    }
    static HANDLE createTimer(void* context, bool highResolution) noexcept {
        auto& fake = *static_cast<FakeMixPumpSchedulerApi*>(context);
        if (highResolution) {
            ++fake.highTimerCreates;
            if (fake.failHighResolution) return nullptr;
        } else {
            ++fake.normalTimerCreates;
            if (fake.failNormalTimer) return nullptr;
        }
        return timerHandle();
    }
    static BOOL setTimer(void* context, HANDLE, int64_t dueTime100ns) noexcept {
        auto& fake = *static_cast<FakeMixPumpSchedulerApi*>(context);
        ++fake.setCalls;
        fake.lastDueTime100ns = dueTime100ns;
        return fake.failSetTimer ? FALSE : TRUE;
    }
    static DWORD wait(void* context, HANDLE, HANDLE) noexcept {
        auto& fake = *static_cast<FakeMixPumpSchedulerApi*>(context);
        ++fake.waitCalls;
        return fake.waitResult;
    }
    static DWORD waitForStop(void* context, HANDLE, DWORD timeoutMillis) noexcept {
        auto& fake = *static_cast<FakeMixPumpSchedulerApi*>(context);
        ++fake.stopWaitCalls;
        fake.lastStopTimeoutMillis = timeoutMillis;
        return fake.stopWaitResult;
    }
    static BOOL signal(void* context, HANDLE) noexcept {
        ++static_cast<FakeMixPumpSchedulerApi*>(context)->signalCalls;
        return TRUE;
    }
    static BOOL cancel(void* context, HANDLE) noexcept {
        ++static_cast<FakeMixPumpSchedulerApi*>(context)->cancelCalls;
        return TRUE;
    }
    static BOOL close(void* context, HANDLE handle) noexcept {
        auto& fake = *static_cast<FakeMixPumpSchedulerApi*>(context);
        if (handle == stopHandle()) ++fake.stopCloses;
        if (handle == timerHandle()) ++fake.timerCloses;
        return TRUE;
    }
    static DWORD lastError(void* context) noexcept {
        return static_cast<FakeMixPumpSchedulerApi*>(context)->error;
    }

    MixPumpScheduler::Api api() noexcept {
        return {
            this, &createStop, &createTimer, &setTimer, &wait, &waitForStop, &signal,
            &cancel, &close, &lastError,
        };
    }
};

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
    static void missPumpPeriods(AudioEngine& engine, uint64_t periods) {
        engine.handleMixPumpDiscontinuity(periods);
    }
    static uint64_t pumpMisses(AudioEngine& engine) {
        return engine.mixPumpMissedPeriods();
    }
    static uint64_t overflowOutput(AudioEngine& engine, const float* source,
                                   uint32_t frames) {
        engine.outputBuses_[0]->clearStatistics();
        engine.outputBuses_[0]->publish(source, frames, 1.0f, false);
        return engine.outputStatus(0).dropped;
    }
    static bool invalidSelectionPreservesOutput(AudioEngine& engine) {
        engine.config_.output.deviceId = L"original";
        engine.setChannelDevice(500, {L"invalid", L"invalid"});
        return engine.config_.output.deviceId == L"original";
    }
    static void activateMeterSession(AudioEngine& engine) {
        engine.running_.store(true);
        engine.monitoringState_.store(2);
    }
    static void pumpBlock(AudioEngine& engine) { engine.pumpBlock(); }
    static uint32_t sourceEpoch(AudioEngine& engine) {
        return engine.channels_[0]->stream.epoch();
    }
    static OutputBus& outputBus(AudioEngine& engine) { return *engine.outputBuses_[0]; }
    static void reopenOutputGate(AudioEngine& engine) {
        engine.outputReadyState_[0].store(engine.monitoringState_.load());
        engine.outputGates_[0]->onRenderFormat(48000, 480);
    }
    static void renderOutputGate(AudioEngine& engine, float* samples, uint32_t frames) {
        engine.outputGates_[0]->renderMix(samples, frames);
    }
    static void toggleWithLifecycleLocksHeld(AudioEngine& engine) {
        std::lock_guard<std::mutex> lifecycle(engine.lifecycleMutex_);
        std::lock_guard<std::mutex> config(engine.configMutex_);
        for (int i = 0; i < 1000; ++i) engine.setMonitoring((i & 1) != 0);
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
        AudioEngineTestAccess::prepare(engine, 20);
        AudioEngineTestAccess::activateMeterSession(engine);
        const uint32_t epoch = AudioEngineTestAccess::sourceEpoch(engine);
        AudioEngineTestAccess::feed(engine, 2400);
        AudioEngineTestAccess::pumpBlock(engine);
        check(engine.channelPeak(0).l.take() > 0.45f && engine.running() && !engine.monitoring(),
              "paused capture session meters source audio through the normal mixer");
        bool continuous = true;
        bool noOutput = true;
        for (int i = 0; i < 20; ++i) {
            engine.setMonitoring((i & 1) == 0);
            AudioEngineTestAccess::feed(engine, 480);
            AudioEngineTestAccess::pumpBlock(engine);
            continuous = continuous && engine.channelPeak(0).l.take() > 0.45f &&
                AudioEngineTestAccess::sourceEpoch(engine) == epoch &&
                engine.channelStatus(0).state == StreamState::Running;
            noOutput = noOutput && engine.outputPeak().l.take() == 0.0f &&
                AudioEngineTestAccess::outputBus(engine).publishedFrames() == 0;
        }
        check(continuous, "rapid monitoring toggles preserve capture epoch and every source meter block");
        check(noOutput, "paused or not-yet-open outputs receive no mix samples or peaks");
        const uint32_t count = engine.visualSamples().beginRead();
        bool silentVisual = true;
        for (uint32_t i = 0; i < count; ++i) {
            float l = 0, r = 0;
            engine.visualSamples().readFrame(i, l, r);
            silentVisual = silentVisual && l == 0.0f && r == 0.0f;
        }
        engine.visualSamples().endRead(count);
        check(silentVisual, "paused or unopened forwarding publishes silent Live Mix samples");
        const auto before = std::chrono::steady_clock::now();
        AudioEngineTestAccess::toggleWithLifecycleLocksHeld(engine);
        const auto elapsed = std::chrono::steady_clock::now() - before;
        check(elapsed < std::chrono::milliseconds(100),
              "monitoring requests never wait for stream/config lifecycle work");
        engine.setMonitoring(false);
        engine.setOutputDevice(0, {L"paused-selection", L"Paused selection"});
        check(engine.outputStatus().state == StreamState::Stopped &&
                  engine.outputStatus().sampleRate == 0,
              "changing a paused output selection does not open a playback stream");
    }
    {
        AudioEngine engine;
        AudioEngineTestAccess::activateMeterSession(engine);
        engine.setMonitoring(true);
        AudioEngineTestAccess::reopenOutputGate(engine);
        std::vector<float> oldMix(4800 * 2, 0.75f);
        auto& bus = AudioEngineTestAccess::outputBus(engine);
        bus.publish(oldMix.data(), 4800, 1.0f, false);
        AudioEngineTestAccess::renderOutputGate(engine, block.data(), 480);
        check(audible(block), "active forwarding gate consumes audible mix samples");
        engine.setMonitoring(false);
        AudioEngineTestAccess::renderOutputGate(engine, block.data(), 480);
        check(std::all_of(block.begin(), block.end(), [](float v) { return v == 0.0f; }),
              "pausing silences already-buffered output before supervisor shutdown");
        engine.setMonitoring(true);
        AudioEngineTestAccess::renderOutputGate(engine, block.data(), 480);
        check(std::all_of(block.begin(), block.end(), [](float v) { return v == 0.0f; }) &&
                  engine.outputStatus().state == StreamState::Opening,
              "rapid resume cannot consume the preceding forwarding generation");
        AudioEngineTestAccess::reopenOutputGate(engine);
        check(bus.depthFrames() == 0, "resumed render consumer discards every queued old frame");
        std::vector<float> newMix(2400 * 2, -0.25f);
        bus.publish(newMix.data(), 2400, 1.0f, false);
        AudioEngineTestAccess::renderOutputGate(engine, block.data(), 480);
        check(audible(block) && std::all_of(block.begin(), block.end(),
                  [](float v) { return v <= 0.0f; }),
              "resumed forwarding fades in only fresh samples");
    }
    {
        check(MixPumpScheduler::relativeDueTime100ns(std::chrono::nanoseconds(1)) == -1 &&
                  MixPumpScheduler::relativeDueTime100ns(std::chrono::nanoseconds(101)) == -2 &&
                  MixPumpScheduler::relativeDueTime100ns(std::chrono::milliseconds(10)) == -100000,
              "mix-pump deadlines round up to relative 100 ns timer ticks");
        check(MixPumpScheduler::timeoutMillis(std::chrono::nanoseconds(1)) == 1 &&
                  MixPumpScheduler::timeoutMillis(std::chrono::milliseconds(10)) == 10,
              "degraded mix-pump deadlines round up to millisecond timeouts");

        FakeMixPumpSchedulerApi fake;
        auto api = fake.api();
        MixPumpScheduler scheduler(&api);
        check(scheduler.open() && scheduler.highResolution() &&
                  fake.highTimerCreates == 1 && fake.normalTimerCreates == 0,
              "mix-pump scheduler prefers a high-resolution timer");
        check(scheduler.waitFor(std::chrono::milliseconds(10)) ==
                      MixPumpScheduler::WaitResult::Deadline &&
                  fake.setCalls == 1 && fake.waitCalls == 1 &&
                  fake.lastDueTime100ns == -100000,
              "mix-pump scheduler arms a one-shot timer for each deadline");
        fake.waitResult = WAIT_OBJECT_0;
        check(scheduler.signalStop() &&
                  scheduler.waitFor(std::chrono::milliseconds(10)) ==
                      MixPumpScheduler::WaitResult::Stop &&
                  fake.signalCalls == 1,
              "mix-pump scheduler selects its independent stop event");
        check(scheduler.open() && fake.cancelCalls == 1 && fake.stopCloses == 1 &&
                  fake.timerCloses == 1,
              "reopening the mix-pump scheduler closes its previous handles");
        scheduler.close();
        check(fake.cancelCalls == 2 && fake.stopCloses == 2 && fake.timerCloses == 2,
              "closing the mix-pump scheduler releases both handles exactly once");
    }
    {
        FakeMixPumpSchedulerApi fake;
        fake.failSetTimer = true;
        auto api = fake.api();
        MixPumpScheduler scheduler(&api);
        check(scheduler.open() &&
                  scheduler.waitFor(std::chrono::milliseconds(10)) ==
                      MixPumpScheduler::WaitResult::Deadline &&
                  scheduler.degraded() && fake.setCalls == 1 &&
                  fake.stopWaitCalls == 1 && fake.lastStopTimeoutMillis == 10 &&
                  scheduler.lastError() == ERROR_GEN_FAILURE,
              "timer-arm failure degrades to a stop-event timeout");
        fake.failSetTimer = false;
        fake.stopWaitResult = WAIT_OBJECT_0;
        check(scheduler.waitFor(std::chrono::milliseconds(10)) ==
                      MixPumpScheduler::WaitResult::Stop &&
                  fake.setCalls == 1 && fake.stopWaitCalls == 2,
              "degraded scheduling stays latched and remains immediately cancellable");
    }
    {
        FakeMixPumpSchedulerApi fake;
        fake.waitResult = WAIT_FAILED;
        auto api = fake.api();
        MixPumpScheduler scheduler(&api);
        check(scheduler.open() &&
                  scheduler.waitFor(std::chrono::milliseconds(7)) ==
                      MixPumpScheduler::WaitResult::Deadline &&
                  scheduler.degraded() && fake.waitCalls == 1 &&
                  fake.stopWaitCalls == 1 && fake.lastStopTimeoutMillis == 7,
              "timer-wait failure degrades without stopping the pump");
    }
    {
        FakeMixPumpSchedulerApi fake;
        fake.failHighResolution = true;
        auto api = fake.api();
        MixPumpScheduler scheduler(&api);
        check(scheduler.open() && !scheduler.highResolution() &&
                  fake.highTimerCreates == 1 && fake.normalTimerCreates == 1,
              "mix-pump scheduler falls back to a normal waitable timer");
    }
    {
        FakeMixPumpSchedulerApi fake;
        fake.failHighResolution = true;
        fake.failNormalTimer = true;
        auto api = fake.api();
        MixPumpScheduler scheduler(&api);
        check(!scheduler.open() && !scheduler.ready() && fake.stopCloses == 1 &&
                  fake.timerCloses == 0 && scheduler.lastError() == ERROR_GEN_FAILURE,
              "failed timer creation closes the partially opened scheduler");
    }
    {
        MixPumpScheduler scheduler;
        const bool opened = scheduler.open();
        const auto started = std::chrono::steady_clock::now();
        const bool signalled = opened && scheduler.signalStop();
        const auto result = opened
            ? scheduler.waitFor(std::chrono::hours(1))
            : MixPumpScheduler::WaitResult::Failed;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        check(opened && signalled && result == MixPumpScheduler::WaitResult::Stop &&
                  elapsed.count() < 200,
              "the real mix-pump stop event cancels a long deadline immediately");
    }
    {
        OutputBus first(64, 8), second(64, 8), muted(64, 8);
        first.onRenderFormat(48000, 4);
        second.onRenderFormat(48000, 4);
        muted.onRenderFormat(48000, 4);
        std::vector<float> source(32 * 2);
        for (size_t i = 0; i < source.size(); i += 2) {
            source[i] = 0.8f;
            source[i + 1] = -0.4f;
        }
        first.publish(source.data(), 32, 1.0f, false);
        second.publish(source.data(), 32, 0.25f, false);
        muted.publish(source.data(), 32, 1.0f, true);
        std::vector<float> firstOut(8), secondOut(8), mutedOut(8);
        first.renderMix(firstOut.data(), 4);
        second.renderMix(secondOut.data(), 4);
        muted.renderMix(mutedOut.data(), 4);
        check(firstOut[0] > 0.0f && firstOut[6] > firstOut[0] &&
                  std::fabs(firstOut[1] / firstOut[0] + 0.5f) < 0.0001f,
              "first output bus fades the canonical mix in without changing stereo balance");
        check(std::fabs(secondOut[0] / firstOut[0] - 0.25f) < 0.0001f &&
                  std::fabs(secondOut[1] / firstOut[1] - 0.25f) < 0.0001f,
              "output bus gain is independent per destination");
        check(!audible(mutedOut), "muting one output bus emits silence");

        OutputBus transitioning(1024, 8);
        transitioning.onRenderFormat(48000, 4);
        transitioning.publish(source.data(), 32, 1.0f, false);
        std::vector<float> transitionOut(8);
        // Four frames per call; replenish the deterministic source while the
        // presence envelope reaches unity.
        for (int i = 0; i < 60; ++i) {
            transitioning.publish(source.data(), 4, 1.0f, false);
            transitioning.renderMix(transitionOut.data(), 4);
        }
        transitioning.requestReset();
        transitioning.renderMix(transitionOut.data(), 4);
        check(transitionOut[0] > transitionOut[6] && transitionOut[6] > 0.0f,
              "output timeline reset begins with real audio fading down");
        for (int i = 1; i < 60; ++i) {
            transitioning.publish(source.data(), 4, 1.0f, false);
            transitioning.renderMix(transitionOut.data(), 4);
        }
        check(transitioning.priming() && transitioning.depthFrames() == 0,
              "output timeline reset drops stale audio after the fade");

        OutputBus stalled(8, 4);
        stalled.onRenderFormat(48000, 2);
        stalled.publish(source.data(), 16, 1.0f, false);
        std::vector<float> overflowOut(4);
        stalled.renderMix(overflowOut.data(), 2);
        check(stalled.droppedFrames() == 8 && stalled.depthFrames() == 0 &&
                  !audible(overflowOut),
              "a stalled output drops only its branch and resets its stale timeline");
    }
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
        AudioEngineTestAccess::prepare(engine, 50);
        AudioEngineTestAccess::feed(engine, 3000);
        engine.renderMix(block.data(), 480);
        AudioEngineTestAccess::feed(engine, 960);
        check(AudioEngineTestAccess::depth(engine) > 0,
              "pump-discontinuity fixture has queued capture audio");
        AudioEngineTestAccess::missPumpPeriods(engine, 3);
        check(AudioEngineTestAccess::depth(engine) == 2400 &&
                  AudioEngineTestAccess::pumpMisses(engine) == 3,
              "missed pump periods trim stale capture latency to its target and remain observable");
        AudioEngineTestAccess::missPumpPeriods(engine, 2);
        check(AudioEngineTestAccess::pumpMisses(engine) == 5,
              "pump missed-period diagnostic is monotonic");

        std::vector<float> oversized((OutputBus::kDefaultRingCapacityFrames + 1) * 2,
                                     0.25f);
        check(AudioEngineTestAccess::overflowOutput(
                  engine, oversized.data(), OutputBus::kDefaultRingCapacityFrames + 1) == 1,
              "output status exposes frames dropped by its fan-out bus");
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
