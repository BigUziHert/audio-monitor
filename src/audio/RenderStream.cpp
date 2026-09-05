#include "audio/RenderStream.h"
#include "audio/RealtimeThread.h"
#include "util/Log.h"
#include "util/Text.h"

#include <audiosessiontypes.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace audiomon {
namespace {

// Converts a frame count to a REFERENCE_TIME, rounding rather than truncating.
// Truncating here is a classic source of an off-by-one-frame buffer that then
// fails the alignment retry a second time.
REFERENCE_TIME framesToRefTime(UINT32 frames, uint32_t rate) {
    if (rate == 0) return 0;
    return static_cast<REFERENCE_TIME>((10000000ULL * frames + rate / 2) / rate);
}

} // namespace

RenderStream::~RenderStream() { stop(); }

std::wstring RenderStream::resolvedName() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return resolvedName_;
}
std::wstring RenderStream::resolvedId() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return resolvedId_;
}
std::string RenderStream::lastError() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return lastError_;
}
StreamDiagnosticInfo RenderStream::diagnosticInfo() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return {resolvedName_, resolvedId_, lastError_, diagnosticFormat_};
}
void RenderStream::setError(const char* what, HRESULT hr) {
    std::lock_guard<std::mutex> lock(infoMutex_);
    lastError_ = std::string(what) + ": " + log::hrString(hr);
}

void RenderStream::logOpenFailureOnce() {
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (lastError_ == lastLoggedOpenError_) return;
    LOG_ERR("render: open failed: %s", lastError_.c_str());
    lastLoggedOpenError_ = lastError_;
}

void RenderStream::start(DeviceManager& devices, const DeviceRef& ref,
                         IMixSource* mixer, bool preferExclusive) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
    devices_         = &devices;
    mixer_           = mixer;
    preferExclusive_ = preferExclusive;

    // See the note in CaptureStream::start: the previous device's identity
    // must not survive into the window before the new one resolves.
    {
        std::lock_guard<std::mutex> info(infoMutex_);
        resolvedId_.clear();
        resolvedName_.clear();
        lastError_.clear();
        diagnosticFormat_.clear();
    }
    quit_.store(false, std::memory_order_relaxed);
    wantsRetry_.store(false, std::memory_order_release);
    devicePeriodHns_ = 0;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state_.store(StreamState::Opening, std::memory_order_release);
    thread_ = std::thread(&RenderStream::threadMain, this, ref);
}

void RenderStream::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    stopLocked();
}

void RenderStream::stopLocked() {
    quit_.store(true, std::memory_order_relaxed);
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    state_.store(StreamState::Stopped, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Exclusive-mode format negotiation.
//
// GetMixFormat describes the shared-mode ENGINE format, which is always float
// -- it says nothing about what the hardware accepts exclusively. HDMI carries
// LPCM on the wire and has no float representation, so integer PCM is tried
// first here and float last. In exclusive mode IsFormatSupported is a
// yes/no oracle with no closest-match, so the candidate grid is ours to build.
// ---------------------------------------------------------------------------
bool RenderStream::tryExclusive(IMMDevice* device) {
    std::vector<WAVEFORMATEXTENSIBLE> candidates;

    // The endpoint's own configured format first: whatever the Sound control
    // panel says is by definition supported.
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.put())) && props) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_AudioEngine_DeviceFormat, &pv)) &&
            pv.vt == VT_BLOB && pv.blob.pBlobData &&
            pv.blob.cbSize >= sizeof(WAVEFORMATEX)) {
            const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(pv.blob.pBlobData);
            WAVEFORMATEXTENSIBLE ext{};
            if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                pv.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
                std::memcpy(&ext, wfx, sizeof(WAVEFORMATEXTENSIBLE));
                candidates.push_back(ext);
            }
        }
        PropVariantClear(&pv);
    }

    // Then a grid, integer first. 48k before 44.1k: the capture card and the
    // rest of the machine are almost certainly at 48k, and matching it means
    // the render side needs no rate conversion at all.
    for (uint32_t rate : { 48000u, 44100u }) {
        for (uint16_t bits : { uint16_t(16), uint16_t(24), uint16_t(32) }) {
            WAVEFORMATEXTENSIBLE f{};
            buildPcmFormat(f, rate, 2, bits);
            candidates.push_back(f);
        }
    }
    for (uint32_t rate : { 48000u, 44100u }) {
        WAVEFORMATEXTENSIBLE f{};
        buildFloat32Format(f, rate, 2);
        candidates.push_back(f);
    }

    for (const auto& cand : candidates) {
        const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(&cand);

        // Fresh client per attempt: a failed Initialize leaves the object
        // spent, and reusing it returns AUDCLNT_E_ALREADY_INITIALIZED.
        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid()))) {
            continue;
        }
        LOG_INFO("render: probing %u Hz %u ch %u-bit tag=0x%X cbSize=%u",
                 wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample,
                 wfx->wFormatTag, wfx->cbSize);
        const HRESULT supportHr =
            client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, wfx, nullptr);
        if (supportHr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
            LOG_WARN("render: exclusive mode is disabled for this endpoint in its "
                     "Advanced properties; using shared mode");
            setError("exclusive not allowed", supportHr);
            return false;
        }
        if (supportHr != S_OK) continue;
        LOG_INFO("render: format accepted by IsFormatSupported");

        REFERENCE_TIME hnsDefault = 0, hnsMin = 0;
        client->GetDevicePeriod(&hnsDefault, &hnsMin);
        REFERENCE_TIME period = hnsDefault ? hnsDefault : 100000;
        REFERENCE_TIME acceptedPeriod = period;

        // In exclusive event-driven mode buffer duration and periodicity must
        // be identical, or Initialize returns BUFDURATION_PERIOD_NOT_EQUAL.
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                        period, period, wfx, nullptr);

        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            // The driver wants a specific frame count. Ask the spent client
            // what it wants, convert back to a duration, then start over with
            // a brand-new client.
            UINT32 aligned = 0;
            if (SUCCEEDED(client->GetBufferSize(&aligned)) && aligned > 0) {
                const REFERENCE_TIME alignedPeriod = framesToRefTime(aligned, wfx->nSamplesPerSec);
                client.reset();
                if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                            client.putVoid()))) {
                    continue;
                }
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                        alignedPeriod, alignedPeriod, wfx, nullptr);
                acceptedPeriod = alignedPeriod;
                LOG_INFO("render: buffer alignment retry at %u frames -> %s",
                         aligned, log::hrString(hr).c_str());
            }
        }

        if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) {
            // A policy setting, not contention: the endpoint's "Allow
            // applications to take exclusive control" box is unchecked. No
            // amount of retrying will change it.
            LOG_WARN("render: exclusive mode is disabled for this endpoint in its "
                     "Advanced properties; using shared mode");
            setError("exclusive not allowed", hr);
            return false;
        }
        if (hr == AUDCLNT_E_DEVICE_IN_USE) {
            LOG_WARN("render: endpoint is held exclusively by another application");
            setError("device in use", hr);
            return false;
        }
        if (FAILED(hr)) continue;   // format rejected; try the next candidate

        LOG_INFO("render: exclusive Initialize succeeded");
        if (!parseWaveFormat(wfx, format_)) {
            LOG_WARN("render: negotiated format could not be parsed; trying next candidate");
            continue;
        }
        LOG_INFO("render: parsed %s (blockAlign=%u, mask=0x%X)",
                 format_.describe().c_str(), format_.blockAlign, format_.channelMask);

        if (FAILED(client->GetBufferSize(&bufferFrames_)) || bufferFrames_ == 0) continue;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), render_.putVoid()))) continue;

        client_ = client;
        devicePeriodHns_ = acceptedPeriod;
        exclusive_.store(true, std::memory_order_release);
        LOG_INFO("render: exclusive mode, %s, %u frame buffer",
                 format_.describe().c_str(), bufferFrames_);
        return true;
    }

    LOG_WARN("render: no exclusive-mode format was accepted; falling back to shared");
    return false;
}

bool RenderStream::trySharedFallback(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
    if (FAILED(hr)) { setError("Activate(IAudioClient)", hr); return false; }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { setError("GetMixFormat", hr); return false; }

    REFERENCE_TIME hnsDefault = 0, hnsMin = 0;
    client->GetDevicePeriod(&hnsDefault, &hnsMin);
    if (hnsDefault <= 0) hnsDefault = 100000;

    // Shared mode: periodicity must be 0, and the buffer may be longer than
    // one period.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                            hnsDefault * 2, 0, mix, nullptr);
    const bool parsed = parseWaveFormat(mix, format_);
    CoTaskMemFree(mix);

    if (FAILED(hr)) { setError("Initialize(shared)", hr); return false; }
    if (!parsed)    { setError("unsupported shared mix format", E_FAIL); return false; }

    if (FAILED(client->GetBufferSize(&bufferFrames_)) || bufferFrames_ == 0) {
        setError("GetBufferSize", E_FAIL);
        return false;
    }
    hr = client->GetService(__uuidof(IAudioRenderClient), render_.putVoid());
    if (FAILED(hr)) { setError("GetService(IAudioRenderClient)", hr); return false; }

    client_ = client;
    devicePeriodHns_ = hnsDefault;
    exclusive_.store(false, std::memory_order_release);
    LOG_INFO("render: shared mode, %s, %u frame buffer", format_.describe().c_str(), bufferFrames_);
    return true;
}

bool RenderStream::openDevice(const DeviceRef& ref) {
    ComPtr<IMMDevice> device;
    std::wstring      gotId, gotName;
    const ResolveResult rr = devices_->resolve(ref, eRender, device, &gotId, &gotName);
    if (rr == ResolveResult::Ambiguous) {
        setError("output name matches more than one endpoint -- pick one in Settings", E_FAIL);
        wantsRetry_.store(false, std::memory_order_release);   // retrying cannot help
        return false;
    }
    if (rr == ResolveResult::NotFound || !device) {
        setError("output device not found", E_FAIL);
        // The Elgato commonly enumerates after we do on a cold boot. This is
        // an expected, retryable state rather than a failure.
        wantsRetry_.store(true, std::memory_order_release);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        resolvedId_   = gotId;
        resolvedName_ = gotName;
    }

    // Never take exclusive control of an endpoint the system is using as a
    // default -- that would break every other application on the machine,
    // which is precisely what this app exists to avoid.
    bool allowExclusive = preferExclusive_;
    if (allowExclusive && devices_->isDefaultForAnyRole(gotId)) {
        LOG_WARN("render: '%s' is a system default endpoint; refusing exclusive mode and "
                 "using shared instead", toUtf8(gotName).c_str());
        allowExclusive = false;
    }

    if (allowExclusive && tryExclusive(device.get())) return true;
    if (trySharedFallback(device.get())) return true;

    wantsRetry_.store(true, std::memory_order_release);
    return false;
}

void RenderStream::closeDevice() {
    if (client_) client_->Stop();
    render_.reset();
    client_.reset();
    if (bufferEvent_) { CloseHandle(bufferEvent_); bufferEvent_ = nullptr; }
}

void RenderStream::renderLoop() {
    const bool excl     = exclusive_.load(std::memory_order_relaxed);
    HANDLE     waits[2] = { stopEvent_, bufferEvent_ };

    LARGE_INTEGER qpcFrequency{};
    LARGE_INTEGER lastWake{};
    QueryPerformanceFrequency(&qpcFrequency);
    const uint64_t periodTicks =
        qpcFrequency.QuadPart > 0 && devicePeriodHns_ > 0
            ? (static_cast<uint64_t>(qpcFrequency.QuadPart) *
                   static_cast<uint64_t>(devicePeriodHns_) +
               5000000ULL) /
                  10000000ULL
            : 0;

    // A GetBuffer failure that is not DEVICE_INVALIDATED would otherwise spin
    // here forever, once per event, counting underruns and never recovering.
    // Give up after a run of them and let the supervisor rebuild the device.
    int consecutiveFailures = 0;
    constexpr int kMaxConsecutiveFailures = 100;

    while (!quit_.load(std::memory_order_relaxed)) {
        // Finite timeout: if the Elgato is unplugged the event simply stops
        // being signalled, and an INFINITE wait would hang this thread forever
        // with no path to recovery.
        const DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (wr == WAIT_OBJECT_0) break;
        if (quit_.load(std::memory_order_relaxed)) break;
        if (wr == WAIT_TIMEOUT) {
            setError("render event stalled", AUDCLNT_E_DEVICE_INVALIDATED);
            wantsRetry_.store(true, std::memory_order_release);
            break;
        }
        if (wr != WAIT_OBJECT_0 + 1) {
            const DWORD error = (wr == WAIT_FAILED) ? GetLastError() : ERROR_GEN_FAILURE;
            setError("render event wait failed", HRESULT_FROM_WIN32(error));
            wantsRetry_.store(true, std::memory_order_release);
            break;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (periodTicks > 0 && lastWake.QuadPart > 0 && now.QuadPart > lastWake.QuadPart) {
            const uint64_t elapsed = static_cast<uint64_t>(now.QuadPart - lastWake.QuadPart);
            // Round to the nearest device period. Small scheduler jitter around
            // one period is normal; intervals around two or more mean one or
            // more render opportunities were missed.
            const uint64_t elapsedPeriods = (elapsed + periodTicks / 2) / periodTicks;
            if (elapsedPeriods > 1) {
                underruns_.fetch_add(elapsedPeriods - 1, std::memory_order_relaxed);
            }
        }
        lastWake = now;

        UINT32 toWrite = bufferFrames_;
        if (!excl) {
            // Shared mode only: the engine may still hold part of the buffer.
            UINT32 padding = 0;
            const HRESULT phr = client_->GetCurrentPadding(&padding);
            if (FAILED(phr)) {
                // This is how an unplug surfaces in shared mode. Breaking
                // without flagging a retry would leave the render thread dead
                // and the supervisor with no reason to rebuild it -- silent,
                // permanent loss of output.
                setError("GetCurrentPadding", phr);
                wantsRetry_.store(true, std::memory_order_release);
                break;
            }
            toWrite = (padding < bufferFrames_) ? (bufferFrames_ - padding) : 0;
            if (toWrite == 0) continue;
        }
        // In exclusive event-driven mode we write exactly GetBufferSize()
        // frames every event -- GetCurrentPadding reports the full buffer here,
        // so the shared-mode "size - padding" idiom would compute zero and
        // render nothing but silence.

        BYTE* out = nullptr;
        HRESULT hr = render_->GetBuffer(toWrite, &out);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
            setError("GetBuffer", hr);
            wantsRetry_.store(true, std::memory_order_release);
            break;
        }
        if (FAILED(hr) || !out) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            if (++consecutiveFailures >= kMaxConsecutiveFailures) {
                setError("GetBuffer failed repeatedly", hr);
                wantsRetry_.store(true, std::memory_order_release);
                break;
            }
            continue;
        }
        consecutiveFailures = 0;

        mixer_->renderMix(mixBuffer_.data(), toWrite);
        converter_.fromStereoFloat(mixBuffer_.data(), out, toWrite);

        hr = render_->ReleaseBuffer(toWrite, 0);
        if (FAILED(hr)) {
            setError("ReleaseBuffer", hr);
            wantsRetry_.store(true, std::memory_order_release);
            break;
        }
    }
}

void RenderStream::threadMain(DeviceRef ref) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needsUninit = SUCCEEDED(coHr);

    enableDenormalFlush();

    // Per-thread, and must be registered from inside the thread being boosted.
    // "Pro Audio" is the tightest class, which is what keeps a fullscreen game
    // from starving this thread.
    MmcssRegistration mmcss;
    if (!mmcss.acquire(L"Pro Audio", AVRT_PRIORITY_CRITICAL)) {
        LOG_WARN("render: MMCSS 'Pro Audio' unavailable (%lu); fell back to "
                 "THREAD_PRIORITY_ABOVE_NORMAL", mmcss.lastError());
    }

    if (!openDevice(ref)) {
        logOpenFailureOnce();
        state_.store(StreamState::Failed, std::memory_order_release);
    } else {
        {
            std::lock_guard<std::mutex> lock(infoMutex_);
            lastLoggedOpenError_.clear();
        }
        LOG_INFO("render: configuring converter for %s, %u frames/block",
                 format_.describe().c_str(), bufferFrames_);
        converter_.configure(format_);
        sampleRate_.store(format_.sampleRate, std::memory_order_release);
        blockFrames_.store(bufferFrames_, std::memory_order_release);
        {
            std::lock_guard<std::mutex> info(infoMutex_);
            diagnosticFormat_ = format_.describe() + ", buffer=" + std::to_string(bufferFrames_) +
                " frames, period=" + std::to_string(double(devicePeriodHns_) / 10000.0) + " ms";
        }
        mixBuffer_.assign(static_cast<size_t>(bufferFrames_) * 2, 0.0f);
        LOG_INFO("render: mixBuffer %zu floats", mixBuffer_.size());

        // Auto-reset. A manual-reset event stays signalled and turns the
        // render loop into a 100%-CPU spin.
        bufferEvent_ = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        HRESULT hr = bufferEvent_ ? client_->SetEventHandle(bufferEvent_) : E_FAIL;

        if (SUCCEEDED(hr)) {
            LOG_INFO("render: notifying mixer of format");
            mixer_->onRenderFormat(format_.sampleRate, bufferFrames_);
            const uint32_t nominalFrames = exclusive_.load(std::memory_order_relaxed)
                ? bufferFrames_
                : static_cast<uint32_t>(std::min<uint64_t>(bufferFrames_,
                    std::max<uint64_t>(1, (uint64_t(devicePeriodHns_) * format_.sampleRate +
                                          9999999ULL) / 10000000ULL)));
            mixer_->onRenderPeriod(nominalFrames);
            LOG_INFO("render: mixer configured; pre-rolling silence");

            // Pre-roll one silent buffer: exclusive mode starts the DMA
            // immediately and an unfilled first buffer is an audible glitch.
            BYTE* pre = nullptr;
            if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &pre)) && pre) {
                render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
            }

            hr = client_->Start();
            if (SUCCEEDED(hr)) {
                LOG_INFO("render: started; entering render loop");
                state_.store(StreamState::Running, std::memory_order_release);
                renderLoop();
                LOG_INFO("render: loop exited");
            } else {
                setError("IAudioClient::Start", hr);
            }
        } else {
            setError("SetEventHandle", hr);
        }

        if (state_.load(std::memory_order_acquire) != StreamState::Running) {
            state_.store(StreamState::Failed, std::memory_order_release);
            wantsRetry_.store(true, std::memory_order_release);
        }
    }

    closeDevice();
    if (state_.load(std::memory_order_acquire) == StreamState::Running) {
        state_.store(wantsRetry_.load(std::memory_order_acquire)
                         ? StreamState::Failed : StreamState::Stopped,
                     std::memory_order_release);
    }

    if (needsUninit) CoUninitialize();
}

} // namespace audiomon
