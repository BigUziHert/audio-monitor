#pragma once
//
// Cancellable one-shot scheduler for AudioEngine's fixed-rate mix pump.
//
// The Windows implementation prefers a high-resolution waitable timer and
// falls back to an ordinary waitable timer on older systems.  A separate
// manual-reset event keeps shutdown immediate even when the next mix deadline
// is still in the future.
//
#include <windows.h>

#include <chrono>
#include <cstdint>

namespace audiomon {

class MixPumpScheduler {
public:
    enum class WaitResult { Deadline, Stop, Failed };

    // Narrow injectable surface used by the deterministic unit tests.  The
    // production instance uses systemApi(); callers that inject an Api must
    // keep it and its context alive for the scheduler's lifetime.
    struct Api {
        void* context = nullptr;
        HANDLE (*createStopEvent)(void* context) noexcept = nullptr;
        HANDLE (*createTimer)(void* context, bool highResolution) noexcept = nullptr;
        BOOL (*setRelativeTimer)(void* context, HANDLE timer,
                                 int64_t dueTime100ns) noexcept = nullptr;
        DWORD (*waitForStopOrTimer)(void* context, HANDLE stopEvent,
                                    HANDLE timer) noexcept = nullptr;
        DWORD (*waitForStop)(void* context, HANDLE stopEvent,
                             DWORD timeoutMillis) noexcept = nullptr;
        BOOL (*signalEvent)(void* context, HANDLE event) noexcept = nullptr;
        BOOL (*cancelTimer)(void* context, HANDLE timer) noexcept = nullptr;
        BOOL (*closeHandle)(void* context, HANDLE handle) noexcept = nullptr;
        DWORD (*getLastError)(void* context) noexcept = nullptr;
    };

    explicit MixPumpScheduler(const Api* api = nullptr) noexcept;
    ~MixPumpScheduler();

    MixPumpScheduler(const MixPumpScheduler&) = delete;
    MixPumpScheduler& operator=(const MixPumpScheduler&) = delete;

    // open()/close() are externally serialized with the pump worker. open()
    // first closes any previous pair, which makes repeated engine starts safe.
    bool open() noexcept;
    void close() noexcept;
    bool signalStop() noexcept;
    WaitResult waitFor(std::chrono::steady_clock::duration delay) noexcept;

    bool ready() const noexcept { return stopEvent_ && timer_; }
    bool highResolution() const noexcept { return highResolution_; }
    bool degraded() const noexcept { return degraded_; }
    DWORD lastError() const noexcept { return lastError_; }

    // Windows relative due times are negative 100-nanosecond ticks. Rounding
    // up prevents a requested deadline from being scheduled early.
    static int64_t relativeDueTime100ns(
        std::chrono::steady_clock::duration delay) noexcept;
    static DWORD timeoutMillis(
        std::chrono::steady_clock::duration delay) noexcept;

    static const Api& systemApi() noexcept;

private:
    const Api* api_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE timer_ = nullptr;
    bool highResolution_ = false;
    bool degraded_ = false;
    DWORD lastError_ = ERROR_SUCCESS;

    WaitResult waitForStop(std::chrono::steady_clock::duration delay) noexcept;
    WaitResult degradeAndWait(std::chrono::steady_clock::duration delay,
                              DWORD error) noexcept;
};

} // namespace audiomon
