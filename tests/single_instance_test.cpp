#include "util/SingleInstance.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using audiomon::SingleInstance;

namespace {

constexpr UINT kShowMessage = WM_APP + 17;

std::wstring uniqueName() {
    static unsigned sequence = 0;
    return L"AudioMonitorInstanceTest-" + std::to_wstring(GetCurrentProcessId()) +
           L"-" + std::to_wstring(++sequence);
}

// A hidden test window and independently owned mutex model the real process
// lifecycle without launching Audio Monitor or touching the user's devices.
class Owner {
public:
    std::wstring windowClass = uniqueName();
    std::wstring mutexName = L"Local\\" + windowClass;
    std::atomic<unsigned> notifications{0};
    std::atomic<unsigned> notReady{0};
    bool started = false;

    Owner(std::chrono::milliseconds createDelay = 0ms,
          std::chrono::milliseconds readyDelay = 0ms,
          std::chrono::milliseconds lifetime = -1ms) {
        std::promise<bool> acquired;
        auto ready = acquired.get_future();
        thread_ = std::thread([this, createDelay, readyDelay, lifetime,
                               acquired = std::move(acquired)]() mutable {
            HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
            if (!mutex) { acquired.set_value(false); return; }
            WNDCLASSW wc{};
            wc.lpfnWndProc = windowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = windowClass.c_str();
            if (!RegisterClassW(&wc)) {
                ReleaseMutex(mutex);
                CloseHandle(mutex);
                acquired.set_value(false);
                return;
            }
            const auto begin = std::chrono::steady_clock::now();
            readyAt_ = begin + readyDelay;
            acquired.set_value(true);
            HWND window = nullptr;
            while (!stop_) {
                const auto elapsed = std::chrono::steady_clock::now() - begin;
                if (lifetime >= 0ms && elapsed >= lifetime) break;
                if (!window && createDelay >= 0ms && elapsed >= createDelay) {
                    window = CreateWindowW(windowClass.c_str(), L"", WS_OVERLAPPED,
                        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, this);
                }
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                Sleep(2);
            }
            if (window) DestroyWindow(window);
            UnregisterClassW(windowClass.c_str(), wc.hInstance);
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        });
        started = ready.get();
    }

    ~Owner() {
        stop_ = true;
        thread_.join();
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* owner = reinterpret_cast<Owner*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == kShowMessage && owner) {
            if (std::chrono::steady_clock::now() < owner->readyAt_) {
                ++owner->notReady;
                return 0;
            }
            ++owner->notifications;
            return 1;
        }
        return DefWindowProcW(window, message, wp, lp);
    }

    std::atomic<bool> stop_{false};
    std::chrono::steady_clock::time_point readyAt_{};
    std::thread thread_;
};

} // namespace

int main() {
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("FAIL: %s\n", label); ++failed; }
    };
    {
        const auto name = L"Local\\" + uniqueName();
        SingleInstance instance;
        check(instance.enter(name.c_str(), L"UnusedTestClass", kShowMessage, false) ==
                  SingleInstance::Result::Primary,
              "first launch owns the instance lock");
    }
    {
        Owner owner;
        SingleInstance instance;
        check(owner.started && instance.enter(owner.mutexName.c_str(), owner.windowClass.c_str(),
                  kShowMessage, true, 500ms) == SingleInstance::Result::Existing &&
                  owner.notifications == 0,
              "duplicate tray launch does not request a visible window");
    }
    {
        Owner owner(80ms);
        SingleInstance instance;
        check(owner.started && instance.enter(owner.mutexName.c_str(), owner.windowClass.c_str(),
                  kShowMessage, false, 1000ms) == SingleInstance::Result::Existing &&
                  owner.notifications == 1,
              "manual launch waits for the first instance to create its window");
    }
    {
        Owner owner(0ms, 100ms);
        SingleInstance instance;
        check(owner.started && instance.enter(owner.mutexName.c_str(), owner.windowClass.c_str(),
                  kShowMessage, false, 1000ms) == SingleInstance::Result::Existing &&
                  owner.notReady > 0 && owner.notifications == 1,
              "existing HWND must acknowledge mixer readiness before launch exits");
    }
    {
        Owner owner(-1ms, 0ms, 80ms);
        SingleInstance instance;
        check(owner.started && instance.enter(owner.mutexName.c_str(), owner.windowClass.c_str(),
                  kShowMessage, false, 1000ms) == SingleInstance::Result::Primary &&
                  owner.notifications == 0,
              "manual launch takes ownership only after the previous process finishes teardown");
    }
    {
        Owner owner(-1ms);
        SingleInstance instance;
        check(owner.started && instance.enter(owner.mutexName.c_str(), owner.windowClass.c_str(),
                  kShowMessage, false, 80ms) == SingleInstance::Result::Error &&
                  instance.error() == ERROR_TIMEOUT,
              "a stalled owner produces a bounded error instead of a second engine");
    }
    {
        const auto name = L"Local\\" + uniqueName();
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, name.c_str());
        SingleInstance instance;
        check(event && instance.enter(name.c_str(), L"UnusedTestClass", kShowMessage, false) ==
                  SingleInstance::Result::Error && instance.error() != ERROR_SUCCESS,
              "failure to create the named mutex never starts a primary instance");
        if (event) CloseHandle(event);
    }
    {
        const auto name = L"Local\\" + uniqueName();
        HANDLE abandoned = nullptr;
        std::thread owner([&] { abandoned = CreateMutexW(nullptr, TRUE, name.c_str()); });
        owner.join();
        SingleInstance instance;
        check(abandoned && instance.enter(name.c_str(), L"UnusedTestClass", kShowMessage, true) ==
                  SingleInstance::Result::Primary,
              "a crashed owner's abandoned mutex can be acquired safely");
        if (abandoned) CloseHandle(abandoned);
    }
    std::printf("single-instance: %s (%d failures)\n", failed ? "FAILED" : "passed", failed);
    return failed ? 1 : 0;
}
