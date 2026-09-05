#include "audio/AudioEngine.h"
#include "audio/CaptureStream.h"
#include "audio/DeviceManager.h"
#include "audio/RenderStream.h"
#include "config/Config.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

namespace {

using namespace audiomon;

class SilentMix final : public IMixSource {
public:
    void renderMix(float* dst, uint32_t frames) noexcept override {
        std::fill_n(dst, static_cast<size_t>(frames) * 2, 0.0f);
    }
    void onRenderFormat(uint32_t, uint32_t) noexcept override {}
};

} // namespace

int main() {
    using namespace audiomon;

    DeviceManager devices;
    CaptureStream capture;
    capture.configure("lifecycle-test", CaptureMode::Loopback);
    SilentMix mixer;
    RenderStream render;
    const DeviceRef emptyRef{};

    std::atomic<bool> begin{false};
    const auto hammer = [&](int offset) {
        while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 200; ++i) {
            if (((i + offset) & 1) == 0) {
                capture.start(devices, emptyRef);
                render.start(devices, emptyRef, &mixer, false);
            } else {
                capture.stop();
                render.stop();
            }
        }
    };

    std::thread first(hammer, 0);
    std::thread second(hammer, 1);
    begin.store(true, std::memory_order_release);
    first.join();
    second.join();

    capture.stop();
    render.stop();

    int failures = 0;
    if (capture.state() != StreamState::Stopped) {
        std::printf("CaptureStream was not Stopped after lifecycle stress\n");
        ++failures;
    }
    if (render.state() != StreamState::Stopped) {
        std::printf("RenderStream was not Stopped after lifecycle stress\n");
        ++failures;
    }

    // AudioEngine owns several subordinate workers and its scheduler handles.
    // Hammer complete transitions from two MTA callers to ensure one stop can
    // never tear down resources underneath a partially-created start.
    {
        AudioEngine engine;
        Config config;
        config.sources.clear();
        config.output = ChannelConfig{}; // resolves quickly without opening hardware
        config.additionalOutputs.clear();
        config.exclusiveOutput = false;

        std::atomic<bool> engineBegin{false};
        const auto hammerEngine = [&](int offset) {
            const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            while (!engineBegin.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < 100; ++i) {
                if (((i + offset) & 1) == 0)
                    engine.start(config);
                else
                    engine.stop();
            }
            if (SUCCEEDED(coHr)) CoUninitialize();
        };

        std::thread firstEngine(hammerEngine, 0);
        std::thread secondEngine(hammerEngine, 1);
        engineBegin.store(true, std::memory_order_release);
        firstEngine.join();
        secondEngine.join();
        engine.stop();
        if (engine.running()) {
            std::printf("AudioEngine was still Running after concurrent lifecycle stress\n");
            ++failures;
        }
    }

    std::printf("Concurrent stream/engine lifecycle test: %s\n",
                failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
