#include "audio/CaptureStream.h"
#include "audio/AppAudio.h"
#include "audio/RealtimeThread.h"
#include "util/Log.h"
#include "util/Text.h"

#include <audiosessiontypes.h>
#include <avrt.h>
#include <algorithm>
#include <mutex>

namespace audiomon {
namespace {

// Loopback delivers no packets at all while its endpoint is silent, so "no
// packets" is a normal state, not a fault. After this timeout the mixer drains
// any buffered tail, then emits silence without chasing the idle capture clock.
constexpr double kIdleAfterSeconds = 0.10;

// Ring capacity is derived from this, in frames per millisecond.
constexpr uint32_t kMaxSupportedRateKHz = 192;

} // namespace

CaptureStream::~CaptureStream() { stop(); }

void CaptureStream::configure(const char* label, CaptureMode mode, uint32_t ringMillis) {
    label_      = label;
    mode_       = mode;
    ringMillis_ = ringMillis;
    // Sized for the highest native rate any endpoint might report, not for 48 kHz:
    // the ring stores frames at the source's NATIVE rate (resampling happens
    // downstream), so a headset switched to 96 or 192 kHz in the Sound control
    // panel would otherwise overflow a ring built for 48. At 250 ms this is
    // 64K frames, half a megabyte per channel -- and 1.3 seconds of headroom
    // in the ordinary 48 kHz case.
    ring_.init(kMaxSupportedRateKHz * ringMillis_);
    QueryPerformanceFrequency(&qpcFreq_);
}

std::wstring CaptureStream::resolvedName() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return resolvedName_;
}
std::wstring CaptureStream::resolvedId() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return resolvedId_;
}
std::string CaptureStream::lastError() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return lastError_;
}

void CaptureStream::setError(const char* what, HRESULT hr) {
    std::lock_guard<std::mutex> lock(infoMutex_);
    lastError_ = std::string(what) + ": " + log::hrString(hr);
}

void CaptureStream::start(DeviceManager& devices, const DeviceRef& ref) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
    devices_ = &devices;

    // Forget the previous device's identity NOW, not when the new one resolves.
    // These are only ever written on a successful open, so without this a
    // restart leaves the old endpoint's ID visible for the milliseconds the
    // worker needs to resolve -- and a config save in that window (Restore
    // defaults triggers one in the same frame) would persist the OLD pin back
    // to disk, silently undoing the very change that caused the restart. The
    // UI would also keep showing the old device's name next to 'unavailable'
    // if the new resolve failed.
    {
        std::lock_guard<std::mutex> info(infoMutex_);
        resolvedId_.clear();
        resolvedName_.clear();
        lastError_.clear();
    }
    // A restart is a timeline break: whatever is still sitting in the ring
    // predates this device and may even be at a different sample rate. Bumping
    // the epoch makes the mixer drop it and re-prime.
    epoch_.fetch_add(1, std::memory_order_release);
    dropped_.store(0, std::memory_order_relaxed);
    retryable_.store(true, std::memory_order_release);
    quit_.store(false, std::memory_order_relaxed);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset: a stop stays stopped
    state_.store(StreamState::Opening, std::memory_order_release);
    thread_ = std::thread(&CaptureStream::threadMain, this, ref);
}

void CaptureStream::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
}

void CaptureStream::stopLocked() {
    {
        std::lock_guard<std::mutex> info(infoMutex_);
        resolvedId_.clear();
        resolvedName_.clear();
        lastError_.clear();
    }
    quit_.store(true, std::memory_order_relaxed);
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    state_.store(StreamState::Stopped, std::memory_order_release);
    flowing_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> info(infoMutex_);
        resolvedId_.clear();
        resolvedName_.clear();
        lastError_.clear();
    }
}

bool CaptureStream::openDevice(const DeviceRef& ref) {
    if (mode_ == CaptureMode::Application) return openProcess(ref.nameMatch);
    const EDataFlow flow = (mode_ == CaptureMode::Loopback) ? eRender : eCapture;

    ComPtr<IMMDevice> device;
    std::wstring      gotId, gotName;
    const ResolveResult rr = devices_->resolve(ref, flow, device, &gotId, &gotName);
    if (rr == ResolveResult::Ambiguous) {
        retryable_.store(false, std::memory_order_release);
        setError("device name matches more than one endpoint -- pick one in Settings", E_FAIL);
        return false;
    }
    retryable_.store(true, std::memory_order_release);
    if (rr == ResolveResult::NotFound || !device) {
        setError("device not found", E_FAIL);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        resolvedId_   = gotId;
        resolvedName_ = gotName;
    }
    if (rr == ResolveResult::MatchedByName) {
        // Distinguish first run from a driver reinstall: saying the id
        // "changed" when there never was one reads as a problem.
        LOG_INFO("%s: matched '%s' by name (%s); id will be saved",
                 label_.c_str(), toUtf8(gotName).c_str(),
                 ref.id.empty() ? "no id configured yet" : "configured id no longer resolves");
    }

    LOG_INFO("%s: resolved '%s'", label_.c_str(), toUtf8(gotName).c_str());

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client_.putVoid());
    if (FAILED(hr)) { setError("Activate(IAudioClient)", hr); return false; }
    LOG_INFO("%s: activated", label_.c_str());

    // Loopback and shared capture both accept only the engine's mix format.
    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { setError("GetMixFormat", hr); return false; }

    const bool parsed = parseWaveFormat(mix, format_);
    if (!parsed) {
        CoTaskMemFree(mix);
        setError("unsupported mix format (compressed passthrough?)", E_FAIL);
        return false;
    }
    LOG_INFO("%s: mix format %s (blockAlign=%u, mask=0x%X)", label_.c_str(),
             format_.describe().c_str(), format_.blockAlign, format_.channelMask);
    converter_.configure(format_);
    sampleRate_.store(format_.sampleRate, std::memory_order_release);

    REFERENCE_TIME hnsDefault = 0, hnsMin = 0;
    client_->GetDevicePeriod(&hnsDefault, &hnsMin);

    // No EVENTCALLBACK here, deliberately. With LOOPBACK, a silent endpoint
    // produces no packets and therefore never signals the event -- an
    // event-driven wait would block forever the moment the game goes quiet.
    // We poll instead. A generous capture buffer costs only memory, not
    // latency, and absorbs a scheduling hiccup without losing packets.
    DWORD flags = AUDCLNT_STREAMFLAGS_NOPERSIST;
    if (mode_ == CaptureMode::Loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    const REFERENCE_TIME hnsBuffer = 5000000;   // 500 ms
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, hnsBuffer, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) { setError("IAudioClient::Initialize", hr); return false; }

    LOG_INFO("%s: initialized", label_.c_str());

    UINT32 bufferFrames = 0;
    hr = client_->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) { setError("GetBufferSize", hr); return false; }
    LOG_INFO("%s: buffer %u frames, scratch %zu floats", label_.c_str(),
             bufferFrames, static_cast<size_t>(bufferFrames) * 2);

    // Preallocate the conversion scratch for a worst-case packet. Nothing in
    // the drain loop may allocate.
    scratch_.assign(static_cast<size_t>(bufferFrames) * 2, 0.0f);

    hr = client_->GetService(__uuidof(IAudioCaptureClient), capture_.putVoid());
    if (FAILED(hr)) { setError("GetService(IAudioCaptureClient)", hr); return false; }

    hr = client_->Start();
    if (FAILED(hr)) { setError("IAudioClient::Start", hr); return false; }
    LOG_INFO("%s: started", label_.c_str());

    // Poll at roughly half the device period so we never sit on a full packet.
    LONG period100ns = static_cast<LONG>(hnsDefault ? hnsDefault / 2 : 50000);
    period100ns = std::clamp<LONG>(period100ns, 10000, 100000);   // 1ms .. 10ms

    timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (timer_) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(period100ns);
        SetWaitableTimer(timer_, &due, period100ns / 10000 ? period100ns / 10000 : 1,
                         nullptr, nullptr, FALSE);
    }

    LOG_INFO("%s: open on '%s' (%s, %s)", label_.c_str(), toUtf8(resolvedName()).c_str(),
             mode_ == CaptureMode::Loopback ? "loopback" : "microphone",
             format_.describe().c_str());
    return true;
}

bool CaptureStream::openProcess(const std::wstring& path) {
    if (!processCaptureSupported()) {
        setError("App capture requires Windows build 20348+ and a recent Windows SDK", E_NOTIMPL);
        return false;
    }
    const uint32_t pid = findAppProcess(path);
    if (!pid) { setError("App is closed or has multiple independent instances; open one instance", E_FAIL); return false; }
    process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process_) { setError("Cannot access app", HRESULT_FROM_WIN32(GetLastError())); return false; }
    HRESULT hr = activateProcessCapture(pid, stopEvent_, client_);
    if (FAILED(hr) || !client_) { setError("App audio activation failed", hr); return false; }
    WAVEFORMATEXTENSIBLE format;
    buildFloat32Format(format, 48000, 2);
    parseWaveFormat(&format.Format, format_);
    converter_.configure(format_);
    sampleRate_.store(48000, std::memory_order_release);
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 0, 0, &format.Format, nullptr);
    if (FAILED(hr)) { setError("App audio Initialize", hr); return false; }
    UINT32 frames = 0;
    hr = client_->GetBufferSize(&frames);
    if (FAILED(hr)) { setError("App capture buffer", hr); return false; }
    scratch_.assign(static_cast<size_t>(frames) * 2, 0.0f);
    hr = client_->GetService(__uuidof(IAudioCaptureClient), capture_.putVoid());
    if (FAILED(hr)) { setError("App capture service", hr); return false; }
    timer_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!timer_) { setError("App capture event", E_OUTOFMEMORY); return false; }
    hr = client_->SetEventHandle(timer_);
    if (SUCCEEDED(hr)) hr = client_->Start();
    if (FAILED(hr)) { setError("App capture start", hr); return false; }
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        resolvedName_ = path.substr(path.find_last_of(L"\\/") + 1);
    }
    processId_.store(pid, std::memory_order_relaxed);
    return true;
}

void CaptureStream::closeDevice() {
    if (client_) client_->Stop();
    capture_.reset();
    client_.reset();
    if (timer_) {
        if (mode_ != CaptureMode::Application) CancelWaitableTimer(timer_);
        CloseHandle(timer_); timer_ = nullptr;
    }
    if (process_) { CloseHandle(process_); process_ = nullptr; }
    processId_.store(0, std::memory_order_relaxed);
}

void CaptureStream::drainPackets() {
    UINT32 packetFrames = 0;
    HRESULT hr = capture_->GetNextPacketSize(&packetFrames);
    if (FAILED(hr)) { setError("GetNextPacketSize", hr);
                      state_.store(StreamState::Failed, std::memory_order_release); return; }

    while (packetFrames > 0) {
        BYTE*  data      = nullptr;
        UINT32 frames    = 0;
        DWORD  flags     = 0;
        UINT64 devPos    = 0, qpcPos = 0;

        hr = capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);

        // AUDCLNT_S_BUFFER_EMPTY is a SUCCESS code, so a bare FAILED() test
        // would fall through and use an uninitialised pointer. It also must
        // not be paired with ReleaseBuffer.
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        if (FAILED(hr)) {
            setError("IAudioCaptureClient::GetBuffer", hr);
            state_.store(StreamState::Failed, std::memory_order_release);
            return;
        }
        if (frames == 0) { capture_->ReleaseBuffer(0); break; }

        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            // The frames in this packet do not follow the previous ones. The
            // rate controller's history is meaningless now.
            epoch_.fetch_add(1, std::memory_order_release);
        }

        const uint32_t cap = static_cast<uint32_t>(scratch_.size() / 2);
        const uint32_t n   = std::min<uint32_t>(frames, cap);

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            // The pointer is valid but its contents are undefined -- these are
            // real frames that must be written as actual zeros.
            std::fill_n(scratch_.data(), static_cast<size_t>(n) * 2, 0.0f);
        } else {
            converter_.toStereoFloat(data, scratch_.data(), n);
        }

        const uint32_t space = ring_.beginWrite();
        const uint32_t w     = std::min(space, n);
        for (uint32_t i = 0; i < w; ++i) {
            ring_.writeFrame(i, scratch_[i * 2], scratch_[i * 2 + 1]);
        }
        // Publish liveness before publishing the frames. The render thread can
        // then distinguish a newly resumed producer from a stable idle tail.
        QueryPerformanceCounter(&lastPacketQpc_);
        flowing_.store(true, std::memory_order_release);
        ring_.endWrite(w);

        if (w < n) {
            // Ring full: the mixer is not keeping up, or we were descheduled.
            // Drop the excess and mark a timeline break -- but still release
            // the whole packet. Holding it to apply back-pressure would stall
            // the engine and glitch the user's own headset output.
            dropped_.fetch_add(n - w, std::memory_order_relaxed);
            epoch_.fetch_add(1, std::memory_order_release);
        }

        // Capture packets are atomic: release exactly what GetBuffer reported.
        hr = capture_->ReleaseBuffer(frames);
        if (FAILED(hr)) {
            setError("ReleaseBuffer", hr);
            state_.store(StreamState::Failed, std::memory_order_release);
            return;
        }

        hr = capture_->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            setError("GetNextPacketSize", hr);
            state_.store(StreamState::Failed, std::memory_order_release);
            return;
        }
    }
}

void CaptureStream::threadMain(DeviceRef ref) {
    // MTA so the WASAPI interfaces can be used directly from this thread with
    // no marshalling.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsUninit = SUCCEEDED(coHr);

    // These threads convert samples, so they need denormal flushing too --
    // MXCSR is per-thread and is not inherited from whoever spawned us.
    enableDenormalFlush();

    // Capture is not as latency-critical as render, but it must not be starved
    // by a game either. "Audio" rather than "Pro Audio" leaves the tighter
    // class for the render thread.
    MmcssRegistration mmcss;
    if (!mmcss.acquire(L"Audio")) {
        LOG_WARN("%s: MMCSS unavailable (%lu); fell back to a plain priority bump",
                 label_.c_str(), mmcss.lastError());
    }

    if (!openDevice(ref)) {
        const std::string error = lastError();
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(infoMutex_);
            changed = error != lastLoggedOpenError_;
            if (changed) lastLoggedOpenError_ = error;
        }
        if (changed) LOG_ERR("%s: open failed: %s", label_.c_str(), error.c_str());
        state_.store(StreamState::Failed, std::memory_order_release);
    } else {
        {
            std::lock_guard<std::mutex> lock(infoMutex_);
            lastLoggedOpenError_.clear();
        }
        state_.store(StreamState::Running, std::memory_order_release);
        QueryPerformanceCounter(&lastPacketQpc_);

        HANDLE waits[2] = { stopEvent_, timer_ };
        const DWORD waitCount = timer_ ? 2 : 1;

        while (!quit_.load(std::memory_order_relaxed)) {
            const DWORD wr = WaitForMultipleObjects(waitCount, waits, FALSE, 100);
            if (wr == WAIT_OBJECT_0) break;                 // stop requested
            if (quit_.load(std::memory_order_relaxed)) break;

            if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                setError("App closed; waiting for it to reopen", E_FAIL);
                state_.store(StreamState::Failed, std::memory_order_release);
                break;
            }

            drainPackets();
            if (state_.load(std::memory_order_acquire) == StreamState::Failed) break;

            // Idle detection is wall-clock, not device position: the device
            // clock does not advance across a silent gap, so it cannot measure
            // one.
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double since = qpcFreq_.QuadPart
                ? double(now.QuadPart - lastPacketQpc_.QuadPart) / double(qpcFreq_.QuadPart)
                : 0.0;
            flowing_.store(since < kIdleAfterSeconds, std::memory_order_release);
        }
    }

    closeDevice();
    flowing_.store(false, std::memory_order_release);

    if (needsUninit) CoUninitialize();
}

} // namespace audiomon
