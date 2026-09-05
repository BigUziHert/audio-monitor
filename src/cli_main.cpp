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
//   audiomon-cli --verbose   also echo startup breadcrumbs to the console
//
#include "audio/AudioEngine.h"
#include "config/Config.h"
#include "util/Log.h"

#include <windows.h>
#include <cstdio>
#include <csignal>
#include <exception>
#include <stdexcept>
#include <thread>

using namespace audiomon;

namespace {

volatile std::sig_atomic_t g_stop = 0;
bool g_ansi = false;   // set once we know the console understands escapes

// MSVC's abort() calls __fastfail(FAST_FAIL_FATAL_APP_EXIT), which reports the
// same 0xC0000409 status as a stack-buffer overrun -- so a bare exit code
// cannot distinguish "memory corruption" from "std::terminate". This handler
// makes the difference visible: an uncaught exception prints its what(), while
// terminate with no active exception points at a noexcept function throwing or
// a joinable std::thread being destroyed.
[[noreturn]] void onTerminate() {
    std::fputs("\n*** FATAL: std::terminate ***\n", stdout);
    if (std::exception_ptr e = std::current_exception()) {
        try { std::rethrow_exception(e); }
        catch (const std::exception& ex) {
            std::printf("    uncaught exception: %s\n", ex.what());
        }
        catch (...) {
            std::fputs("    uncaught exception of non-standard type\n", stdout);
        }
    } else {
        std::fputs("    no active exception -- a noexcept function threw, or a\n"
                   "    joinable std::thread was destroyed/assigned over\n", stdout);
    }
    std::fflush(stdout);
    std::_Exit(3);
}

// Windows consoles do not interpret ANSI escapes unless the mode is set
// explicitly. Windows Terminal enables it, classic conhost often does not, so
// probe rather than assume -- otherwise the "clear screen" redraw prints
// literal escape sequences all over the output.
void setupConsole() {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return;
    if (SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) g_ansi = true;
}

void clearScreen() {
    if (g_ansi) { std::printf("\033[H\033[J"); return; }
    // Fallback: scroll the previous frame away. Ugly but readable, and far
    // better than a screen of escape sequences.
    std::printf("\n\n");
}

// Console output is UTF-8 (set above), so wide strings are converted rather
// than passed to %ls, whose behaviour depends on the C locale and mangles
// anything outside ASCII -- device names routinely contain parentheses,
// dashes and the occasional non-ASCII character.
std::string u8(const std::wstring& w) { return audiomon::toUtf8(w); }

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
        std::printf("  %-52s %s\n    id: %s\n", u8(d.name).c_str(),
                    d.isDefault ? "[system default]" : "", u8(d.id).c_str());
    }
    std::printf("\n--- Capture endpoints (microphones) ---\n");
    for (const auto& d : dm.list(eCapture)) {
        std::printf("  %-52s %s\n    id: %s\n", u8(d.name).c_str(),
                    d.isDefault ? "[system default]" : "", u8(d.id).c_str());
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
    setupConsole();
    std::set_terminate(onTerminate);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) { std::printf("CoInitializeEx failed: 0x%08lX\n", coHr); return 1; }

    log::init(Config::appDataDir());

    bool listOnly = false;
    bool verbose  = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list")    listOnly = true;
        if (arg == "--verbose") verbose  = true;
    }
    // Startup breadcrumbs are off by default now that bring-up is done, but
    // kept behind a flag: they are how the ucrtbase crash was finally located,
    // and the next device-specific failure will want them too.
    log::setEcho(verbose);

    if (listOnly) { listDevices(); log::shutdown(); CoUninitialize(); return 0; }

    bool usedDefaults = false;
    Config cfg = Config::load(&usedDefaults);
    std::printf("config: %s%s\n", u8(Config::configPath()).c_str(),
                usedDefaults ? "  (missing or unreadable -- using autodetected defaults)" : "");
    // Raw, unbuffered breadcrumbs. Deliberately not going through the logging
    // subsystem: we already learned the hard way that a crash here leaves no
    // trace, and this narrows it to a single statement.
    if (verbose) { std::fputs("[bringup] constructing AudioEngine\n", stdout); std::fflush(stdout); }
    AudioEngine engine;
    if (verbose) { std::fputs("[bringup] AudioEngine constructed\n", stdout); std::fflush(stdout); }

    if (verbose) { std::fputs("[bringup] calling engine.start\n", stdout); std::fflush(stdout); }
    try {
        if (!engine.start(cfg)) { std::printf("engine failed to start\n"); return 1; }
    } catch (const std::exception& ex) {
        std::printf("\n*** engine.start threw: %s\n", ex.what());
        return 1;
    }
    if (verbose) { std::fputs("[bringup] engine.start returned\n", stdout); std::fflush(stdout); }

    // Give the worker threads a moment to resolve and open devices.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    // Stop echoing to the console before the meter display takes over the
    // screen: a log line arriving mid-redraw tears the display, which is
    // exactly what a device reconnect would do at the worst moment. The file
    // still receives everything.
    log::setEcho(false);

    std::vector<MeterBallistics> ball(cfg.sources.size() + cfg.outputCount());

    auto last = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        clearScreen();
        std::printf("audio-monitor  --  Ctrl+C to stop\n\n");
        for (size_t output = 0; output < cfg.outputCount(); ++output) {
            const OutputStatus os = engine.outputStatus(output);
            std::printf("OUTPUT %zu  %-8s %s  %u Hz  %u frames  underruns=%llu  dropped=%llu\n",
                        output + 1, stateName(os.state),
                        os.exclusive ? "exclusive" : "shared   ",
                        os.sampleRate, os.blockFrames,
                        static_cast<unsigned long long>(os.underruns),
                        static_cast<unsigned long long>(os.dropped));
            std::printf("          %s\n",
                        os.deviceName.empty() ? "(unresolved)" : u8(os.deviceName).c_str());
            if (!os.error.empty() && os.state != StreamState::Running)
                std::printf("          last error: %s\n", os.error.c_str());
        }
        if (engine.mixPumpMissedPeriods() != 0)
            std::printf("MIX PUMP  missed periods=%llu\n",
                        static_cast<unsigned long long>(engine.mixPumpMissedPeriods()));
        std::printf("\n");

        for (int i = 0; i < static_cast<int>(cfg.sources.size()); ++i) {
            const ChannelStatus cs = engine.channelStatus(i);
            ball[i].update(std::max(engine.channelPeak(i).l.take(),
                                    engine.channelPeak(i).r.take()), dt);

            std::printf("%s %-7s %-7s ", cfg.sources[i].label.c_str(), stateName(cs.state),
                        cs.flowing ? "flowing" : "quiet");
            printMeterBar(ball[i].levelDb());
            std::printf(" %6.1f dB  depth=%5u  ratio=%.6f  %uHz\n",
                        ball[i].levelDb(), cs.depth, cs.ratio, cs.sampleRate);
            std::printf("     %s\n", cs.deviceName.empty()
                        ? (cs.error.empty() ? "(unresolved)" : "(see error below)")
                        : u8(cs.deviceName).c_str());
            if (!cs.error.empty() && cs.state != StreamState::Running) {
                std::printf("     %s\n", cs.error.c_str());
            }
        }

        for (size_t output = 0; output < cfg.outputCount(); ++output) {
            auto &meter = ball[cfg.sources.size() + output];
            meter.update(std::max(engine.outputPeak(output).l.take(),
                                  engine.outputPeak(output).r.take()), dt);
            std::printf("\nMIX %zu  ", output + 1);
            printMeterBar(meter.levelDb());
            std::printf(" %6.1f dB\n", meter.levelDb());
        }
    }

    std::printf("\nstopping...\n");
    engine.stop();
    log::shutdown();
    CoUninitialize();
    return 0;
}
