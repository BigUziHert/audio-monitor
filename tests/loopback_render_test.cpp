#include "audio/CaptureStream.h"
#include "audio/AudioEngine.h"
#include "audio/DeviceManager.h"
#include "audio/RenderStream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace audiomon {
struct AudioEngineTestAccess {
    static StreamState outputWorkerState(const AudioEngine& engine, size_t output) {
        return engine.renders_[output]->state();
    }
};
}

namespace {

using namespace audiomon;

template <typename Stream>
bool waitForRunning(Stream& stream, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const StreamState state = stream.state();
        if (state == StreamState::Running) return true;
        if (state == StreamState::Failed) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return stream.state() == StreamState::Running;
}

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

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) {
        std::printf("CoInitializeEx failed: 0x%08lX\n",
                    static_cast<unsigned long>(coHr));
        return 1;
    }

    int failures = 0;
    bool skipped = false;
    {
        DeviceManager devices;
        if (!devices.start(nullptr)) {
            std::printf("Failed to start the audio device enumerator\n");
            ++failures;
        } else {
            const auto endpoints = devices.list(eRender);
            if (endpoints.empty()) {
                std::printf("No active render endpoint; skipping hardware test\n");
                skipped = true;
            } else {
                const DeviceInfo& endpoint = endpoints.front();
                const DeviceRef ref{endpoint.id, endpoint.name};

                CaptureStream capture;
                capture.configure("loopback-render-test", CaptureMode::Loopback);
                for (int attempt = 0; attempt < 3; ++attempt) {
                    capture.start(devices, ref);
                    if (!waitForRunning(capture, std::chrono::seconds(8))) {
                        std::printf("Loopback attempt %d failed: %s\n", attempt + 1,
                                    capture.lastError().c_str());
                        ++failures;
                    } else {
                        if (capture.sampleRate() == 0) {
                            std::printf("Loopback attempt %d reported a zero sample rate\n",
                                        attempt + 1);
                            ++failures;
                        }
                        if (capture.resolvedId() != endpoint.id) {
                            std::printf("Loopback attempt %d resolved the wrong endpoint\n",
                                        attempt + 1);
                            ++failures;
                        }
                    }
                    capture.stop();
                    if (capture.state() != StreamState::Stopped) {
                        std::printf("Loopback attempt %d did not stop cleanly\n", attempt + 1);
                        ++failures;
                    }
                }

                SilentMix mixer;
                RenderStream render;
                render.start(devices, ref, &mixer, false);
                if (!waitForRunning(render, std::chrono::seconds(8))) {
                    std::printf("Shared render failed: %s\n", render.lastError().c_str());
                    ++failures;
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (render.state() != StreamState::Running) {
                        std::printf("Shared render stopped during the one-second run: %s\n",
                                    render.lastError().c_str());
                        ++failures;
                    }
                    if (render.underruns() != 0) {
                        std::printf("Shared render counted %llu underruns\n",
                                    static_cast<unsigned long long>(render.underruns()));
                        ++failures;
                    }
                }
                render.stop();
                if (render.state() != StreamState::Stopped) {
                    std::printf("Shared render did not stop cleanly\n");
                    ++failures;
                }

                if (endpoints.size() >= 2) {
                    Config multi = Config::defaults();
                    multi.sources.clear();
                    multi.exclusiveOutput = false;
                    multi.output.deviceId = endpoints[0].id;
                    multi.output.deviceNameMatch = endpoints[0].name;
                    ChannelConfig second;
                    second.deviceId = endpoints[1].id;
                    second.deviceNameMatch = endpoints[1].name;
                    multi.additionalOutputs.push_back(second);

                    AudioEngine engine;
                    if (!engine.start(multi, false)) {
                        std::printf("Multi-output engine failed to start\n");
                        ++failures;
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        if (!engine.running() || engine.monitoring() ||
                            AudioEngineTestAccess::outputWorkerState(engine, 0) != StreamState::Stopped ||
                            AudioEngineTestAccess::outputWorkerState(engine, 1) != StreamState::Stopped) {
                            std::printf("Meter-only engine opened a playback worker\n");
                            ++failures;
                        }
                        engine.setMonitoring(true);
                        const auto deadline = std::chrono::steady_clock::now() +
                                              std::chrono::seconds(8);
                        bool bothRunning = false;
                        while (std::chrono::steady_clock::now() < deadline) {
                            bothRunning = engine.outputStatus(0).state == StreamState::Running &&
                                          engine.outputStatus(1).state == StreamState::Running;
                            if (bothRunning) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(25));
                        }
                        if (!bothRunning) {
                            std::printf("Two-output engine did not open both shared endpoints: %s | %s\n",
                                        engine.outputStatus(0).error.c_str(),
                                        engine.outputStatus(1).error.c_str());
                            ++failures;
                        } else {
                            std::this_thread::sleep_for(std::chrono::milliseconds(300));
                            if (engine.outputStatus(0).state != StreamState::Running ||
                                engine.outputStatus(1).state != StreamState::Running) {
                                std::printf("A multi-output branch stopped during the smoke run\n");
                                ++failures;
                            }

                            const auto pauseAt = std::chrono::steady_clock::now();
                            engine.setMonitoring(false);
                            if (std::chrono::steady_clock::now() - pauseAt >=
                                std::chrono::milliseconds(100)) {
                                std::printf("Pausing waited on output worker shutdown\n");
                                ++failures;
                            }
                            const auto pauseDeadline = std::chrono::steady_clock::now() +
                                                       std::chrono::seconds(2);
                            while (std::chrono::steady_clock::now() < pauseDeadline &&
                                   (AudioEngineTestAccess::outputWorkerState(engine, 0) != StreamState::Stopped ||
                                    AudioEngineTestAccess::outputWorkerState(engine, 1) != StreamState::Stopped)) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            if (!engine.running() || engine.monitoring() ||
                                AudioEngineTestAccess::outputWorkerState(engine, 0) != StreamState::Stopped ||
                                AudioEngineTestAccess::outputWorkerState(engine, 1) != StreamState::Stopped) {
                                std::printf("Pausing did not asynchronously close both output workers\n");
                                ++failures;
                            }
                            // Multiple clicks in one UI frame must converge on
                            // the latest request without reopening captures.
                            for (int toggle = 0; toggle < 11; ++toggle)
                                engine.setMonitoring((toggle & 1) == 0);
                            const auto resumeDeadline = std::chrono::steady_clock::now() +
                                                        std::chrono::seconds(8);
                            while (std::chrono::steady_clock::now() < resumeDeadline &&
                                   (engine.outputStatus(0).state != StreamState::Running ||
                                    engine.outputStatus(1).state != StreamState::Running)) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                            }
                            if (!engine.monitoring() ||
                                engine.outputStatus(0).state != StreamState::Running ||
                                engine.outputStatus(1).state != StreamState::Running) {
                                std::printf("Rapid resume did not reopen both output branches\n");
                                ++failures;
                            }
                        }
                    }
                    engine.stop();
                } else {
                    std::printf("Only one active render endpoint; skipping two-output smoke test\n");
                }
            }
            devices.stop();
        }
    }

    CoUninitialize();
    if (skipped && failures == 0) return 77;
    std::printf("Loopback/render hardware test: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
