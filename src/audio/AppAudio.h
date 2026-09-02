#pragma once
#include "audio/DeviceManager.h"
#include <audioclient.h>

namespace audiomon {
struct AppAudioInfo {
    uint32_t processId = 0;
    std::wstring path;
    std::wstring name;
    std::vector<std::wstring> endpoints;
    bool routingKnown = false;
};
// Read-only session discovery; never changes endpoint or session volume/routing.
std::vector<AppAudioInfo> listAudioApps();
uint32_t findAppProcess(const std::wstring &path);
bool processCaptureSupported();
HRESULT activateProcessCapture(uint32_t processId, HANDLE cancel, ComPtr<IAudioClient> &client);
} // namespace audiomon
