#include "audio/AppAudio.h"
#include "audio/CaptureStream.h"
#include <chrono>
#include <thread>
#include <cstdio>
using namespace audiomon;
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 1;
    if (!processCaptureSupported()) {
        CoUninitialize();
        return 77;
    }
    int result = 0;
    {
        DeviceManager devices;
        if (!devices.start(nullptr))
            return 1;
        wchar_t image[32768]{};
        GetModuleFileNameW(nullptr, image, 32768);
        CaptureStream capture;
        capture.configure("app-capture-test", CaptureMode::Application);
        for (int attempt = 0; attempt < 3; ++attempt) {
            capture.start(devices, {L"", image});
            for (int i = 0; i < 80 && capture.state() == StreamState::Opening; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (capture.state() != StreamState::Running || capture.processId() != GetCurrentProcessId() ||
                capture.sampleRate() != 48000) {
                std::printf("App capture failed: %s\n", capture.lastError().c_str());
                result = 1;
            }
            capture.stop();
            if (capture.state() != StreamState::Stopped)
                result = 1;
        }
    }
    CoUninitialize();
    std::printf("Process capture activation / restart: %s\n", result ? "FAILED" : "PASSED");
    return result;
}
