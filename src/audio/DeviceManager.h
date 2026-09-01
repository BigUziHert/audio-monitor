#pragma once
//
// Endpoint discovery, resolution and hot-plug notification.
//
// Two rules govern everything here:
//
//  1. PASSIVE. We never call GetDefaultAudioEndpoint to decide what to open,
//     and OnDefaultDeviceChanged is deliberately a no-op. Following the
//     default would be an easy accident that silently moves the user's audio
//     when they plug in a monitor -- and this app's whole reason to exist is
//     to leave the system's device configuration alone.
//
//  2. NOTHING HEAVY IN A CALLBACK. IMMNotificationClient methods fire on a
//     WASAPI-owned thread; releasing an IAudioClient or rebuilding a stream
//     from inside one can deadlock against the audio service. The callbacks
//     set a flag and signal an event, and a worker thread does the work.
//
#include "util/ComPtr.h"

#include <windows.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audiomon {

// How a channel names the endpoint it wants. The ID is authoritative, but a
// driver reinstall changes it, so a friendly-name substring is kept as a
// fallback -- that is what makes the config survive a reinstall.
struct DeviceRef {
    std::wstring id;         // e.g. "{0.0.0.00000000}.{9a8b...}"
    std::wstring nameMatch;  // e.g. "Elgato"

    bool empty() const { return id.empty() && nameMatch.empty(); }
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool         isRender = false;
    bool         isDefault = false;
};

enum class ResolveResult {
    NotFound,
    MatchedById,
    MatchedByName,
    // More than one endpoint matched the name substring. Deliberately NOT
    // resolved: picking one would be a guess, and guessing wrong here means
    // silently routing audio into the wrong device.
    Ambiguous,
};

class DeviceManager {
public:
    DeviceManager() = default;
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // Caller must already be in an MTA. Registers the notification client.
    bool start(std::function<void()> onDevicesChanged);
    void stop();

    std::vector<DeviceInfo> list(EDataFlow flow) const;

    // ID first, then a case-insensitive friendly-name substring. `outName` and
    // `outId` receive what was actually opened so the caller can re-persist a
    // refreshed ID after a driver reinstall.
    ResolveResult resolve(const DeviceRef& ref, EDataFlow flow,
                          ComPtr<IMMDevice>& out,
                          std::wstring* outId = nullptr,
                          std::wstring* outName = nullptr) const;

    // Safety gate before taking exclusive control: refuse if this endpoint is
    // the default for ANY role, because exclusive mode on the user's default
    // output would break every other application on the machine.
    bool isDefaultForAnyRole(const std::wstring& id) const;

    static std::wstring friendlyName(IMMDevice* dev);
    static std::wstring deviceId(IMMDevice* dev);

private:
    class Notifier;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    Notifier*                   notifier_ = nullptr;   // COM-refcounted
    std::function<void()>       onChanged_;
    mutable std::mutex          mutex_;                // guards enumerator_ use
};

} // namespace audiomon
