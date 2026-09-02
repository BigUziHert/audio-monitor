#include "audio/AppAudio.h"
#include <audiopolicy.h>
#include <tlhelp32.h>
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#define AUDIOMON_PROCESS_CAPTURE 1
#endif
#include <algorithm>
#include <map>
#include <set>

namespace audiomon {
namespace {
struct Process {
    DWORD id, parent;
    std::wstring path;
};
std::vector<Process> processes() {
    std::vector<Process> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
        do {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768];
            DWORD size = 32768;
            if (QueryFullProcessImageNameW(process, 0, path, &size))
                result.push_back({entry.th32ProcessID, entry.th32ParentProcessID, std::wstring(path, size)});
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}
const Process *root(const std::vector<Process> &all, DWORD id) {
    const Process *p = nullptr;
    for (const auto &entry : all)
        if (entry.id == id) {
            p = &entry;
            break;
        }
    if (!p)
        return nullptr;
    // Electron apps (including Discord) create child processes with the same image.
    for (size_t depth = 0; depth < all.size(); ++depth) {
        const Process *parent = nullptr;
        for (const auto &entry : all)
            if (entry.id == p->parent && entry.id != p->id &&
                _wcsicmp(entry.path.c_str(), p->path.c_str()) == 0) {
                parent = &entry;
                break;
            }
        if (!parent)
            break;
        p = parent;
    }
    return p;
}
} // namespace

std::vector<AppAudioInfo> listAudioApps() {
    const auto all = processes();
    std::map<DWORD, AppAudioInfo> apps;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator.putVoid())))
        return {};
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put())))
        return {};
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, device.put())))
            continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.putVoid())))
            continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.put())))
            continue;
        int n = 0;
        sessions->GetCount(&n);
        for (int j = 0; j < n; ++j) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(sessions->GetSession(j, control.put())) ||
                FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2), control2.putVoid())))
                continue;
            DWORD pid = 0;
            AudioSessionState state = AudioSessionStateExpired;
            if (FAILED(control2->GetProcessId(&pid)) || !pid || pid == GetCurrentProcessId())
                continue;
            control->GetState(&state);
            if (state == AudioSessionStateExpired)
                continue;
            const auto *p = root(all, pid);
            if (!p || p->id == GetCurrentProcessId())
                continue;
            auto &app = apps[p->id];
            app.processId = p->id;
            app.path = p->path;
            auto slash = p->path.find_last_of(L"\\/");
            app.name = p->path.substr(slash == std::wstring::npos ? 0 : slash + 1);
            if (app.name.size() > 4)
                app.name.resize(app.name.size() - 4);
            app.routingKnown = true;
            // Only active, audible sessions are evidence of current overlap.
            ComPtr<ISimpleAudioVolume> volume;
            BOOL muted = FALSE;
            float gain = 1.0f;
            if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), volume.putVoid()))) {
                volume->GetMute(&muted);
                volume->GetMasterVolume(&gain);
            }
            if (state == AudioSessionStateActive && !muted && gain > 0.0001f) {
                auto id = DeviceManager::deviceId(device.get());
                if (std::find(app.endpoints.begin(), app.endpoints.end(), id) == app.endpoints.end())
                    app.endpoints.push_back(id);
            }
        }
    }
    std::vector<AppAudioInfo> result;
    for (auto &entry : apps)
        result.push_back(std::move(entry.second));
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    return result;
}

uint32_t findAppProcess(const std::wstring &path) {
    auto all = processes();
    std::set<DWORD> roots;
    for (const auto &p : all)
        if (_wcsicmp(p.path.c_str(), path.c_str()) == 0) {
            if (const auto *r = root(all, p.id))
                roots.insert(r->id);
        }
    // Multiple independent instances are ambiguous; never attach to a random one.
    return roots.size() == 1 ? *roots.begin() : 0;
}

bool processCaptureSupported() {
#ifdef AUDIOMON_PROCESS_CAPTURE
    using VersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    const auto fn =
        reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return fn && fn(&version) == 0 && version.dwBuildNumber >= 20348;
#else
    return false;
#endif
}

#ifdef AUDIOMON_PROCESS_CAPTURE
namespace {
// The async operation owns a reference until the callback completes. All callback
// state lives here, so cancellation/timeouts never leave pointers into a dead stream.
class Activation final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
  public:
    std::atomic<ULONG> refs{1};
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    PROPVARIANT property{};
    ~Activation() {
        if (ready)
            CloseHandle(ready);
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        else if (iid == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject *>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override {
        ComPtr<IUnknown> unknown;
        HRESULT activated = E_FAIL;
        result = op->GetActivateResult(&activated, unknown.put());
        if (SUCCEEDED(result))
            result = activated;
        if (SUCCEEDED(result) && unknown)
            result = unknown->QueryInterface(__uuidof(IAudioClient), client.putVoid());
        SetEvent(ready);
        return S_OK;
    }
};
} // namespace
#endif
HRESULT activateProcessCapture(uint32_t processId, HANDLE cancel, ComPtr<IAudioClient> &client) {
#ifdef AUDIOMON_PROCESS_CAPTURE
    if (!processCaptureSupported())
        return E_NOTIMPL;
    auto *callback = new Activation();
    if (!callback->ready) {
        callback->Release();
        return E_OUTOFMEMORY;
    }
    callback->parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    callback->parameters.ProcessLoopbackParams.TargetProcessId = processId;
    callback->parameters.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    callback->property.vt = VT_BLOB;
    callback->property.blob.cbSize = sizeof(callback->parameters);
    callback->property.blob.pBlobData = reinterpret_cast<BYTE *>(&callback->parameters);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &callback->property, callback, operation.put());
    if (SUCCEEDED(hr)) {
        HANDLE handles[] = {cancel, callback->ready};
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 5000);
        hr = wait == WAIT_OBJECT_0 + 1 ? callback->result : HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (SUCCEEDED(hr))
            client = std::move(callback->client);
    }
    callback->Release();
    return hr;
#else
    (void)processId;
    (void)cancel;
    (void)client;
    return E_NOTIMPL;
#endif
}
} // namespace audiomon
