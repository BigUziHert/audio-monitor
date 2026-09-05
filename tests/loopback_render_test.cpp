#include "audio/CaptureStream.h"
#include "audio/DeviceManager.h"
#include "audio/RenderStream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

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
            }
            devices.stop();
        }
    }

    CoUninitialize();
    if (skipped && failures == 0) return 77;
    std::printf("Loopback/render hardware test: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
