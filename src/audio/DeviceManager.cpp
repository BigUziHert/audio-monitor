#include "audio/DeviceManager.h"
#include "util/Log.h"

#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cwctype>

namespace audiomon {
namespace {

std::wstring toLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool containsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return false;
    return toLower(haystack).find(toLower(needle)) != std::wstring::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// IMMNotificationClient.
//
// Every method must return promptly. We do not call back into the enumerator,
// do not release any device, and do not touch a stream. We set a flag and let
// a worker thread deal with it.
// ---------------------------------------------------------------------------
class DeviceManager::Notifier : public IMMNotificationClient {
public:
    explicit Notifier(std::function<void()> cb) : cb_(std::move(cb)) {}
    virtual ~Notifier() = default;

    // --- IUnknown ---
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(ref_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const long n = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // --- IMMNotificationClient ---
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { fire(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR)              override { fire(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR)            override { fire(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        // Fires constantly (including for volume). Not a rebuild trigger.
        return S_OK;
    }

    // We never FOLLOW the default -- doing so would silently re-target when the
    // user plugs in a monitor with speakers, which is exactly the behaviour
    // this application exists to avoid. But we do need to know it changed, for
    // the opposite reason: if the endpoint we hold exclusively has just become
    // a system default, we must give exclusive control back so we do not break
    // every other application. The supervisor decides; this only wakes it.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
        fire();
        return S_OK;
    }

private:
    void fire() { if (cb_) cb_(); }

    std::atomic<long>     ref_{1};
    std::function<void()> cb_;
};

// ---------------------------------------------------------------------------

DeviceManager::~DeviceManager() { stop(); }

bool DeviceManager::start(std::function<void()> onDevicesChanged) {
    onChanged_ = std::move(onDevicesChanged);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumerator_.putVoid());
    if (FAILED(hr)) {
        LOG_ERR("CoCreateInstance(MMDeviceEnumerator) failed: %s", log::hrString(hr).c_str());
        return false;
    }

    notifier_ = new Notifier(onChanged_);
    hr = enumerator_->RegisterEndpointNotificationCallback(notifier_);
    if (FAILED(hr)) {
        LOG_WARN("RegisterEndpointNotificationCallback failed: %s -- hot-plug recovery disabled",
                 log::hrString(hr).c_str());
        notifier_->Release();
        notifier_ = nullptr;
    }
    return true;
}

void DeviceManager::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (notifier_) {
        // Unregister BEFORE dropping the enumerator: a callback can be in
        // flight right now, and this is the call that waits for it to finish.
        if (enumerator_) enumerator_->UnregisterEndpointNotificationCallback(notifier_);
        notifier_->Release();
        notifier_ = nullptr;
    }
    enumerator_.reset();
}

std::wstring DeviceManager::friendlyName(IMMDevice* dev) {
    if (!dev) return L"";
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, props.put()))) return L"";

    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        name = pv.pwszVal;
    }
    PropVariantClear(&pv);
    return name;
}

std::wstring DeviceManager::deviceId(IMMDevice* dev) {
    if (!dev) return L"";
    LPWSTR raw = nullptr;
    if (FAILED(dev->GetId(&raw)) || !raw) return L"";
    std::wstring id(raw);
    CoTaskMemFree(raw);
    return id;
}

std::vector<DeviceInfo> DeviceManager::list(EDataFlow flow) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceInfo> out;
    if (!enumerator_) return out;

    // Record the default endpoints only so the UI can label them. Nothing in
    // this application opens a device because it is the default.
    std::wstring defaultId;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, def.put())) && def) {
            defaultId = deviceId(def.get());
        }
    }

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.put())) || !coll) {
        return out;
    }

    UINT count = 0;
    coll->GetCount(&count);
    out.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.put())) || !dev) continue;
        DeviceInfo info;
        info.id        = deviceId(dev.get());
        info.name      = friendlyName(dev.get());
        info.isRender  = (flow == eRender);
        info.isDefault = (!defaultId.empty() && info.id == defaultId);
        out.push_back(std::move(info));
    }
    return out;
}

ResolveResult DeviceManager::resolve(const DeviceRef& ref, EDataFlow flow,
                                     ComPtr<IMMDevice>& out,
                                     std::wstring* outId, std::wstring* outName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out.reset();
    if (!enumerator_ || ref.empty()) return ResolveResult::NotFound;

    // 1. Exact ID. Cheap and unambiguous when nothing has changed.
    if (!ref.id.empty()) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(enumerator_->GetDevice(ref.id.c_str(), dev.put())) && dev) {
            DWORD state = 0;
            if (SUCCEEDED(dev->GetState(&state)) && state == DEVICE_STATE_ACTIVE) {
                // GetDevice does not filter by data flow, so confirm it.
                ComPtr<IMMEndpoint> ep;
                EDataFlow actual = eRender;
                if (SUCCEEDED(dev->QueryInterface(__uuidof(IMMEndpoint), ep.putVoid())) && ep &&
                    SUCCEEDED(ep->GetDataFlow(&actual)) && actual == flow) {
                    // Report what the device says its ID is, not the string we
                    // were handed -- they can differ in case, and persisting
                    // the canonical form keeps later comparisons honest.
                    if (outId)   *outId   = deviceId(dev.get());
                    if (outName) *outName = friendlyName(dev.get());
                    out = dev;
                    return ResolveResult::MatchedById;
                }
            }
        }
    }

    // 2. Friendly-name substring. This is what survives a driver reinstall,
    //    which reissues the endpoint ID and would otherwise orphan the config.
    if (!ref.nameMatch.empty()) {
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.put())) || !coll) {
            return ResolveResult::NotFound;
        }
        UINT count = 0;
        coll->GetCount(&count);

        // Prefer the shortest matching name: with "Game" configured and both
        // "Game (Arctis)" and "Game Chat Mixer (Arctis)" present, the shorter
        // is the more specific match rather than an arbitrary first hit.
        ComPtr<IMMDevice> best;
        std::wstring      bestName;
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(coll->Item(i, dev.put())) || !dev) continue;
            const std::wstring name = friendlyName(dev.get());
            if (!containsNoCase(name, ref.nameMatch)) continue;
            if (!best || name.size() < bestName.size()) { best = dev; bestName = name; }
        }
        if (best) {
            if (outId)   *outId   = deviceId(best.get());
            if (outName) *outName = bestName;
            out = best;
            return ResolveResult::MatchedByName;
        }
    }

    return ResolveResult::NotFound;
}

bool DeviceManager::isDefaultForAnyRole(const std::wstring& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enumerator_ || id.empty()) return false;

    for (ERole role : { eConsole, eMultimedia, eCommunications }) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, role, dev.put())) && dev) {
            if (deviceId(dev.get()) == id) return true;
        }
    }
    return false;
}

} // namespace audiomon
