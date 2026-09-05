#include "audio/MixPumpScheduler.h"

#include <algorithm>
#include <chrono>

namespace audiomon {
namespace {

HANDLE createSystemStopEvent(void*) noexcept {
    return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

HANDLE createSystemTimer(void*, bool highResolution) noexcept {
    const DWORD flags = highResolution ? CREATE_WAITABLE_TIMER_HIGH_RESOLUTION : 0;
    return CreateWaitableTimerExW(nullptr, nullptr, flags,
                                  TIMER_MODIFY_STATE | SYNCHRONIZE);
}

BOOL setSystemRelativeTimer(void*, HANDLE timer, int64_t dueTime100ns) noexcept {
    LARGE_INTEGER due{};
    due.QuadPart = dueTime100ns;
    return SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
}

DWORD waitForSystemStopOrTimer(void*, HANDLE stopEvent, HANDLE timer) noexcept {
    const HANDLE handles[] = {stopEvent, timer};
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE);
}

DWORD waitForSystemStop(void*, HANDLE stopEvent, DWORD timeoutMillis) noexcept {
    return WaitForSingleObject(stopEvent, timeoutMillis);
}

BOOL signalSystemEvent(void*, HANDLE event) noexcept {
    return SetEvent(event);
}

BOOL cancelSystemTimer(void*, HANDLE timer) noexcept {
    return CancelWaitableTimer(timer);
}

BOOL closeSystemHandle(void*, HANDLE handle) noexcept {
    return CloseHandle(handle);
}

DWORD systemLastError(void*) noexcept {
    return GetLastError();
}

} // namespace

const MixPumpScheduler::Api& MixPumpScheduler::systemApi() noexcept {
    static const Api api{
        nullptr,
        &createSystemStopEvent,
        &createSystemTimer,
        &setSystemRelativeTimer,
        &waitForSystemStopOrTimer,
        &waitForSystemStop,
        &signalSystemEvent,
        &cancelSystemTimer,
        &closeSystemHandle,
        &systemLastError,
    };
    return api;
}

MixPumpScheduler::MixPumpScheduler(const Api* api) noexcept
    : api_(api ? api : &systemApi()) {}

MixPumpScheduler::~MixPumpScheduler() {
    close();
}

bool MixPumpScheduler::open() noexcept {
    close();
    lastError_ = ERROR_SUCCESS;

    if (!api_ || !api_->createStopEvent || !api_->createTimer ||
        !api_->setRelativeTimer || !api_->waitForStopOrTimer ||
        !api_->waitForStop || !api_->signalEvent || !api_->cancelTimer ||
        !api_->closeHandle || !api_->getLastError) {
        lastError_ = ERROR_INVALID_PARAMETER;
        return false;
    }

    stopEvent_ = api_->createStopEvent(api_->context);
    if (!stopEvent_) {
        lastError_ = api_->getLastError(api_->context);
        return false;
    }

    timer_ = api_->createTimer(api_->context, true);
    highResolution_ = timer_ != nullptr;
    if (!timer_)
        timer_ = api_->createTimer(api_->context, false);

    if (!timer_) {
        lastError_ = api_->getLastError(api_->context);
        api_->closeHandle(api_->context, stopEvent_);
        stopEvent_ = nullptr;
        highResolution_ = false;
        return false;
    }

    lastError_ = ERROR_SUCCESS;
    degraded_ = false;
    return true;
}

void MixPumpScheduler::close() noexcept {
    if (!api_) return;
    if (timer_) {
        if (api_->cancelTimer) api_->cancelTimer(api_->context, timer_);
        if (api_->closeHandle) api_->closeHandle(api_->context, timer_);
        timer_ = nullptr;
    }
    if (stopEvent_) {
        if (api_->closeHandle) api_->closeHandle(api_->context, stopEvent_);
        stopEvent_ = nullptr;
    }
    highResolution_ = false;
    degraded_ = false;
}

bool MixPumpScheduler::signalStop() noexcept {
    if (!stopEvent_ || !api_ || !api_->signalEvent) {
        lastError_ = ERROR_INVALID_HANDLE;
        return false;
    }
    if (!api_->signalEvent(api_->context, stopEvent_)) {
        lastError_ = api_->getLastError(api_->context);
        return false;
    }
    return true;
}

int64_t MixPumpScheduler::relativeDueTime100ns(
    std::chrono::steady_clock::duration delay) noexcept {
    using namespace std::chrono;
    const auto ns = duration_cast<nanoseconds>(delay).count();
    if (ns <= 0) return -1;
    const int64_t ticks = ns / 100 + (ns % 100 != 0 ? 1 : 0);
    return -std::max<int64_t>(ticks, 1);
}

DWORD MixPumpScheduler::timeoutMillis(
    std::chrono::steady_clock::duration delay) noexcept {
    using namespace std::chrono;
    const auto ns = duration_cast<nanoseconds>(delay).count();
    if (ns <= 0) return 0;
    constexpr uint64_t nsPerMillisecond = 1'000'000;
    const uint64_t positiveNs = static_cast<uint64_t>(ns);
    const uint64_t millis = positiveNs / nsPerMillisecond +
                            (positiveNs % nsPerMillisecond != 0 ? 1 : 0);
    return static_cast<DWORD>(std::min<uint64_t>(millis, INFINITE - 1ULL));
}

MixPumpScheduler::WaitResult MixPumpScheduler::waitForStop(
    std::chrono::steady_clock::duration delay) noexcept {
    const DWORD result = api_->waitForStop(api_->context, stopEvent_, timeoutMillis(delay));
    if (result == WAIT_OBJECT_0) return WaitResult::Stop;
    if (result == WAIT_TIMEOUT) return WaitResult::Deadline;
    lastError_ = api_->getLastError(api_->context);
    return WaitResult::Failed;
}

MixPumpScheduler::WaitResult MixPumpScheduler::degradeAndWait(
    std::chrono::steady_clock::duration delay, DWORD error) noexcept {
    degraded_ = true;
    // A successful arm followed by a failed multi-object wait can leave the
    // timer pending. The degraded path no longer observes it, so cancel it
    // once and wait exclusively on the stop event from here on.
    api_->cancelTimer(api_->context, timer_);
    lastError_ = error;
    return waitForStop(delay);
}

MixPumpScheduler::WaitResult MixPumpScheduler::waitFor(
    std::chrono::steady_clock::duration delay) noexcept {
    if (!ready()) {
        lastError_ = ERROR_INVALID_HANDLE;
        return WaitResult::Failed;
    }
    if (delay <= std::chrono::steady_clock::duration::zero())
        return WaitResult::Deadline;

    if (degraded_) return waitForStop(delay);

    if (!api_->setRelativeTimer(api_->context, timer_, relativeDueTime100ns(delay))) {
        return degradeAndWait(delay, api_->getLastError(api_->context));
    }

    const DWORD result = api_->waitForStopOrTimer(api_->context, stopEvent_, timer_);
    if (result == WAIT_OBJECT_0) return WaitResult::Stop;
    if (result == WAIT_OBJECT_0 + 1) return WaitResult::Deadline;

    return degradeAndWait(delay, api_->getLastError(api_->context));
}

} // namespace audiomon
