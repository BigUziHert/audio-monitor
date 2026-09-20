#include "util/SingleInstance.h"

#include <algorithm>

namespace audiomon {

SingleInstance::~SingleInstance() {
    if (owned_) ReleaseMutex(mutex_);
    if (mutex_) CloseHandle(mutex_);
}

SingleInstance::Result SingleInstance::enter(const wchar_t* mutexName,
    const wchar_t* windowClass, UINT showMessage, bool trayLaunch,
    std::chrono::milliseconds timeout) {
    if (mutex_ || !mutexName || !windowClass || !showMessage) {
        error_ = ERROR_INVALID_PARAMETER;
        return Result::Error;
    }
    // Existing named mutexes ignore CreateMutex's initial-owner flag. Always
    // acquire explicitly so a departing or crashed owner can be replaced.
    mutex_ = CreateMutexW(nullptr, FALSE, mutexName);
    if (!mutex_) {
        error_ = GetLastError();
        return Result::Error;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const DWORD wait = WaitForSingleObject(mutex_, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            owned_ = true;
            return Result::Primary;
        }
        if (wait != WAIT_TIMEOUT) {
            error_ = GetLastError();
            return Result::Error;
        }
        // A repeated sign-in/tray launch must not surface an existing mixer.
        if (trayLaunch) return Result::Existing;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error_ = ERROR_TIMEOUT;
            return Result::Error;
        }
        if (const HWND window = FindWindowW(windowClass, nullptr)) {
            DWORD ownerProcess = 0;
            GetWindowThreadProcessId(window, &ownerProcess);
            if (ownerProcess) AllowSetForegroundWindow(ownerProcess);
            DWORD_PTR acknowledged = 0;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const UINT waitMs = static_cast<UINT>(std::max<long long>(1,
                std::min<long long>(200, remaining.count())));
            if (SendMessageTimeoutW(window, showMessage, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                    waitMs, &acknowledged) &&
                (acknowledged == 1 || (IsWindowVisible(window) && !IsIconic(window))))
                return Result::Existing;
            // A window may exist before its mixer is ready, or disappear while
            // its old process still owns audio devices. Wait for an explicit
            // acknowledgement or ownership; never start a second engine.
        }
        Sleep(20);
    }
}

} // namespace audiomon
