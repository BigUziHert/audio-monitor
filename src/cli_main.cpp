//
// Headless bring-up tool.
//
// This exists so the audio path can be proved end to end -- devices resolved,
// three captures running, a mix reaching the Elgato -- before any window
// exists. If this prints moving meters and the capture card receives audio,
// the hard part works and everything after it is presentation.
//
//   audiomon-cli --list      enumerate endpoints and exit
//   audiomon-cli             run the mixer, print meters until Ctrl+C
//
#include "audio/AudioEngine.h"
#include "config/Config.h"
#include "util/Log.h"

#include <windows.h>
#include <cstdio>
#include <clocale>
#include <csignal>
#include <thread>

using namespace audiomon;

namespace {

volatile std::sig_atomic_t g_stop = 0;

BOOL WINAPI consoleHandler(DWORD) { g_stop = 1; return TRUE; }

const char* stateName(StreamState s) {
    switch (s) {
        case StreamState::Stopped: return "stopped";
        case StreamState::Opening: return "opening";
        case StreamState::Running: return "running";
        case StreamState::Failed:  return "FAILED";
    }
    return "?";
}

void listDevices() {
    DeviceManager dm;
    if (!dm.start(nullptr)) { std::printf("failed to open the device enumerator\n"); return; }

    std::printf("\n--- Render endpoints (loopback sources and the output) ---\n");
    for (const auto& d : dm.list(eRender)) {
        std::printf("  %-52ls %s\n    id: %ls\n", d.name.c_str(),
                    d.isDefault ? "[system default]" : "", d.id.c_str());
    }
    std::printf("\n--- Capture endpoints (microphones) ---\n");
    for (const auto& d : dm.list(eCapture)) {
        std::printf("  %-52ls %s\n    id: %ls\n", d.name.c_str(),
                    d.isDefault ? "[system default]" : "", d.id.c_str());
    }
    std::printf("\n");
    dm.stop();
}

void printMeterBar(float db) {
    const int width = 28;
    const int lit   = int(dbToNorm(db) * width + 0.5f);
    std::putchar('[');
    for (int i = 0; i < width; ++i) std::putchar(i < lit ? '#' : ' ');
    std::putchar(']');
}

} // namespace

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) { std::printf("CoInitializeEx failed: 0x%08lX\n", coHr); return 1; }

    log::init(Config::appDataDir());

    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--list") listOnly = true;
    }

    if (listOnly) { listDevices(); log::shutdown(); CoUninitialize(); return 0; }

    bool usedDefaults = false;
    Config cfg = Config::load(&usedDefaults);
    std::printf("config: %ls%s\n", Config::configPath().c_str(),
                usedDefaults ? "  (not found -- using autodetected defaults)" : "");
    std::printf("matching  game~\"%ls\"  chat~\"%ls\"  mic~\"%ls\"  out~\"%ls\"\n\n",
                cfg.game.deviceNameMatch.c_str(), cfg.chat.deviceNameMatch.c_str(),
                cfg.mic.deviceNameMatch.c_str(),  cfg.output.deviceNameMatch.c_str());

    AudioEngine engine;
    if (!engine.start(cfg)) { std::printf("engine failed to start\n"); return 1; }

    // Give the worker threads a moment to resolve and open devices.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    const char* names[kChannelCount] = { "Game", "Chat", "Mic " };
    MeterBallistics ball[kChannelCount + 1];

    auto last = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        const OutputStatus os = engine.outputStatus();

        std::printf("\033[H\033[J");   // home + clear
        std::printf("audio-monitor  --  Ctrl+C to stop\n\n");
        std::printf("OUTPUT  %-8s %s  %u Hz  %u frames  underruns=%llu\n",
                    stateName(os.state), os.exclusive ? "exclusive" : "shared   ",
                    os.sampleRate, os.blockFrames,
                    static_cast<unsigned long long>(os.underruns));
        std::printf("        %ls\n", os.deviceName.empty() ? L"(unresolved)" : os.deviceName.c_str());
        if (!os.error.empty() && os.state != StreamState::Running) {
            std::printf("        last error: %s\n", os.error.c_str());
        }
        std::printf("\n");

        for (int i = 0; i < kChannelCount; ++i) {
            const ChannelStatus cs = engine.channelStatus(i);
            ball[i].update(std::max(engine.channelPeak(i).l.take(),
                                    engine.channelPeak(i).r.take()), dt);

            std::printf("%s %-7s %-7s ", names[i], stateName(cs.state),
                        cs.flowing ? "flowing" : "quiet");
            printMeterBar(ball[i].levelDb());
            std::printf(" %6.1f dB  depth=%5u  ratio=%.6f  %uHz\n",
                        ball[i].levelDb(), cs.depth, cs.ratio, cs.sampleRate);
            std::printf("     %ls\n", cs.deviceName.empty()
                        ? (cs.error.empty() ? L"(unresolved)" : L"(see error below)")
                        : cs.deviceName.c_str());
            if (!cs.error.empty() && cs.state != StreamState::Running) {
                std::printf("     %s\n", cs.error.c_str());
            }
        }

        ball[kChannelCount].update(std::max(engine.outputPeak().l.take(),
                                            engine.outputPeak().r.take()), dt);
        std::printf("\nMIX  ");
        printMeterBar(ball[kChannelCount].levelDb());
        std::printf(" %6.1f dB\n", ball[kChannelCount].levelDb());
    }

    std::printf("\nstopping...\n");
    engine.stop();
    log::shutdown();
    CoUninitialize();
    return 0;
}
