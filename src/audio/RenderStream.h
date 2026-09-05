#pragma once
//
// One playback destination. Its callback pulls from that destination's
// OutputBus, which resamples the fixed-rate canonical mix onto this device's
// independent clock.
//
// Opened in exclusive mode when requested and safe, so a dedicated capture
// endpoint gets the shortest path to hardware with no engine mixing in between.
// Falls back to shared mode rather than failing,
// because "no audio to the stream PC" is a much worse outcome than "a few more
// milliseconds of latency".
//
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

// Implemented by the mixer. Called on the render thread every device period;
// everything it does must be real-time safe.
class IMixSource {
public:
    virtual ~IMixSource() = default;
    // Fill `frames` interleaved stereo frames. Must always fill the whole
    // block -- silence if there is nothing to play.
    virtual void renderMix(float* dst, uint32_t frames) noexcept = 0;
    // Called on the render thread when the stream (re)starts, so the mixer can
    // reconfigure its rate controllers to the new clock.
    virtual void onRenderFormat(uint32_t sampleRate, uint32_t blockFrames) noexcept = 0;
    // The endpoint's ordinary period can be shorter than its maximum buffer.
    // Optional metadata lets downstream queues reserve for the real clock
    // without mistaking a one-off partial callback for that normal period.
    virtual void onRenderPeriod(uint32_t nominalFrames) noexcept { (void)nominalFrames; }
};

class RenderStream {
public:
    RenderStream() = default;
    ~RenderStream();

    RenderStream(const RenderStream&) = delete;
    RenderStream& operator=(const RenderStream&) = delete;

    void start(DeviceManager& devices, const DeviceRef& ref, IMixSource* mixer, bool preferExclusive);
    void stop();

    StreamState  state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool         exclusive() const noexcept { return exclusive_.load(std::memory_order_acquire); }
    uint32_t     sampleRate() const noexcept { return sampleRate_.load(std::memory_order_acquire); }
    uint32_t     blockFrames() const noexcept { return blockFrames_.load(std::memory_order_acquire); }
    uint64_t     underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    // Explicit session boundary. Automatic device restarts deliberately do
    // not clear this counter, so status consumers always observe monotonic
    // diagnostics while monitoring remains active.
    void clearStatistics() noexcept { underruns_.store(0, std::memory_order_relaxed); }

    std::wstring resolvedName() const;
    std::wstring resolvedId() const;
    std::string  lastError() const;

    // True once the device has stopped for a reason that a retry might fix
    // (late enumeration on boot, cable replug, driver restart).
    bool wantsRetry() const noexcept { return wantsRetry_.load(std::memory_order_acquire); }

private:
    void stopLocked();          // caller holds lifecycleMutex_
    void threadMain(DeviceRef ref);
    bool openDevice(const DeviceRef& ref);
    bool tryExclusive(IMMDevice* device);
    bool trySharedFallback(IMMDevice* device);
    void closeDevice();
    void renderLoop();
    void setError(const char* what, HRESULT hr);
    void logOpenFailureOnce();

    DeviceManager* devices_ = nullptr;
    IMixSource*    mixer_   = nullptr;
    bool           preferExclusive_ = true;

    // Reachable from both the UI thread and the supervisor thread; see the
    // note in CaptureStream.
    std::mutex  lifecycleMutex_;

    std::thread thread_;
    HANDLE      stopEvent_   = nullptr;
    HANDLE      bufferEvent_ = nullptr;

    ComPtr<IAudioClient>       client_;
    ComPtr<IAudioRenderClient> render_;
    StreamFormat               format_{};
    FormatConverter            converter_;
    std::vector<float>         mixBuffer_;    // preallocated stereo float scratch
    UINT32                     bufferFrames_ = 0;
    REFERENCE_TIME             devicePeriodHns_ = 0;

    std::atomic<StreamState> state_{StreamState::Stopped};
    std::atomic<bool>        exclusive_{false};
    std::atomic<uint32_t>    sampleRate_{0};
    std::atomic<uint32_t>    blockFrames_{0};
    std::atomic<uint64_t>    underruns_{0};
    std::atomic<bool>        wantsRetry_{false};
    std::atomic<bool>        quit_{false};

    mutable std::mutex infoMutex_;
    std::wstring       resolvedName_;
    std::wstring       resolvedId_;
    std::string        lastError_;
    std::string        lastLoggedOpenError_;
};

} // namespace audiomon
