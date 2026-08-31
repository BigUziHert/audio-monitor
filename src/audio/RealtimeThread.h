#pragma once
//
// Per-thread setup shared by every audio thread.
//
// Both pieces here are per-thread state that is NOT inherited: MMCSS
// registration applies to the thread that calls it, and MXCSR is saved and
// restored across context switches. Setting either one on a parent thread and
// assuming the audio threads pick it up is a silent no-op.
//
#include <windows.h>
#include <avrt.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

namespace audiomon {

// Denormals arise naturally on a mix path fed by decaying silence and cost
// hundreds of cycles each. FTZ handles results, DAZ handles inputs -- both are
// needed, and they are separate bits from separate headers.
inline void enableDenormalFlush() noexcept {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

// RAII for MMCSS. Scoped as a local inside the thread function so the handle
// can only ever be reverted by the thread that registered it -- reverting from
// another thread silently does nothing.
class MmcssRegistration {
public:
    MmcssRegistration() = default;
    MmcssRegistration(const MmcssRegistration&) = delete;
    MmcssRegistration& operator=(const MmcssRegistration&) = delete;

    // taskName is "Pro Audio" for the render thread, "Audio" for captures.
    // Returns false if MMCSS declined; the caller carries on regardless --
    // this is an optimisation, never a dependency.
    bool acquire(const wchar_t* taskName, AVRT_PRIORITY priority = AVRT_PRIORITY_NORMAL) noexcept {
        taskIndex_ = 0;                       // must be zero-seeded
        handle_    = AvSetMmThreadCharacteristicsW(taskName, &taskIndex_);
        if (!handle_) {
            lastError_ = GetLastError();
            // Fall back to a plain priority bump so a machine with the MMCSS
            // service disabled still gets something.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            return false;
        }
        AvSetMmThreadPriority(handle_, priority);
        return true;
    }

    ~MmcssRegistration() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }

    DWORD lastError() const noexcept { return lastError_; }

private:
    HANDLE handle_    = nullptr;
    DWORD  taskIndex_ = 0;
    DWORD  lastError_ = 0;
};

} // namespace audiomon
