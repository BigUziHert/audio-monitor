#pragma once
//
// One capture source: either WASAPI loopback on a render endpoint (the Arctis
// Game and Chat outputs) or ordinary shared-mode capture (the Yeti).
//
// Both are strictly passive. Loopback attaches to a render endpoint without
// altering it; shared-mode capture leaves the mic available to Discord at the
// same time. Neither path touches volume, mute or default-device state.
//
// Threading: one event-driven worker per source (polling fallback on older
// loopback implementations), owning its own COM apartment and its
// own WASAPI objects. It is the only writer to the ring. The mixer thread is
// the only reader. Nothing is shared but the ring and a few atomics.
//
#include "audio/RingBuffer.h"
#include "audio/WaveFormat.h"
#include "audio/DeviceManager.h"
#include "audio/StreamTypes.h"
#include "util/ComPtr.h"

#include <audioclient.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audiomon {

enum class CaptureMode { Loopback, Microphone, Application };

class CaptureStream {
public:
    CaptureStream() = default;
    ~CaptureStream();

    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;

    // Allocates the ring once, at startup. It cannot be resized later: the
    // mix pump reads the ring even while a stream is down (for the depth
    // readout and to drop stale audio), so reallocating under it would race.
    void configure(const char* label, CaptureMode mode, uint32_t ringMillis = 250);

    // Starts (or restarts) the capture worker against the given endpoint.
    // Non-blocking: resolution and device setup happen on the worker.
    void start(DeviceManager& devices, const DeviceRef& ref);
    void stop();

    // --- read by the mixer thread ---
    StereoRing& ring() noexcept { return ring_; }

    StreamState state() const noexcept { return state_.load(std::memory_order_acquire); }

    // True when packets have arrived recently. Loopback delivers nothing at
    // all while its endpoint is silent, so the mixer drains the buffered tail
    // and freezes the rate controller rather than chasing an empty ring.
    bool flowing() const noexcept { return flowing_.load(std::memory_order_acquire); }
    bool retryable() const noexcept { return retryable_.load(std::memory_order_acquire); }

    // Bumped on any timeline break (discontinuity, reconnect, overflow drop).
    // The mixer resets that channel's rate controller when it changes.
    uint32_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }

    // Native rate of the source, needed to seed the resampler ratio when it
    // differs from the render rate. 0 until the device is open.
    uint32_t sampleRate() const noexcept { return sampleRate_.load(std::memory_order_acquire); }

    // --- diagnostics, read by the UI ---
    const std::string&  label() const { return label_; }
    std::wstring        resolvedName() const;
    std::wstring        resolvedId() const;
    uint64_t            droppedFrames() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    std::string         lastError() const;
    uint32_t processId() const { return processId_.load(std::memory_order_relaxed); }

private:
    friend struct AudioEngineTestAccess;
    void stopLocked();          // caller holds lifecycleMutex_
    void threadMain(DeviceRef ref);
    bool openDevice(const DeviceRef& ref);
    bool openProcess(const std::wstring& path);
    void closeDevice();
    void drainPackets();
    void setError(const char* what, HRESULT hr);

    std::string  label_;
    CaptureMode  mode_       = CaptureMode::Loopback;
    uint32_t     ringMillis_ = 400;

    StereoRing   ring_;
    DeviceManager* devices_ = nullptr;

    // start() and stop() can both be reached from the UI thread (a device
    // change in Settings) and from the supervisor thread (an automatic
    // restart). Without this they would race on thread_ -- assigning over a
    // joinable std::thread calls std::terminate.
    std::mutex   lifecycleMutex_;

    std::thread  thread_;
    HANDLE       stopEvent_  = nullptr;
    HANDLE       timer_      = nullptr;
    bool         eventDriven_ = false; // worker-owned: timer_ is an audio event
    HANDLE       process_    = nullptr;
    std::atomic<uint32_t> processId_{0};

    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioCaptureClient> capture_;
    StreamFormat                format_{};
    FormatConverter             converter_;
    std::vector<float>          scratch_;      // preallocated; never grows in the loop

    std::atomic<StreamState> state_{StreamState::Stopped};
    std::atomic<bool>        flowing_{false};
    std::atomic<bool>        retryable_{true};
    std::atomic<uint32_t>    epoch_{0};
    std::atomic<uint32_t>    sampleRate_{0};
    std::atomic<uint64_t>    dropped_{0};
    std::atomic<bool>        quit_{false};

    mutable std::mutex   infoMutex_;
    std::wstring         resolvedName_;
    std::wstring         resolvedId_;
    std::string          lastError_;
    std::string          lastLoggedOpenError_;

    // Wall-clock of the last packet, for the flowing/idle decision.
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER lastPacketQpc_{};
};

} // namespace audiomon
