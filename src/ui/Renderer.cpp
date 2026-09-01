#include "ui/Renderer.h"
#include "ui/Theme.h"
#include "util/Log.h"

#include <windows.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

namespace audiomon::ui {

bool Renderer::createDeviceObjects(void* hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;      // track the window
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = 0;   // see MakeWindowAssociation below
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = static_cast<HWND>(hwnd);
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
        D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &got, &context_);

    // A machine mid-driver-update, or an RDP session, may have no hardware
    // device available. WARP is slow but this UI is a few hundred triangles.
    if (hr == DXGI_ERROR_UNSUPPORTED || FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
            D3D11_SDK_VERSION, &sd, &swapChain_, &device_, &got, &context_);
    }
    if (FAILED(hr)) {
        LOG_ERR("ui: D3D11 device creation failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    // DXGI installs a hook on the output window and acts on Alt+Enter by
    // default. Alt+Enter is reflex for anyone who plays games, and a mixer
    // panel has no business going fullscreen-exclusive -- worse, this window
    // gets hidden to the tray, and hiding a window that owns a fullscreen
    // swapchain can leave the display mode changed.
    {
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(swapChain_->GetParent(IID_PPV_ARGS(&factory))) && factory) {
            factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);
            factory->Release();
        }
    }

    createRenderTarget();
    return true;
}

void Renderer::createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        device_->CreateRenderTargetView(back, nullptr, &rtv_);
        back->Release();
    }
}

void Renderer::releaseRenderTarget() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

bool Renderer::ensureCreated(void* hwnd) {
    if (device_) return true;
    hwnd_ = hwnd;
    if (!createDeviceObjects(hwnd)) { destroy(); return false; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // No imgui.ini: window geometry is ours to manage, and writing a settings
    // file on every move is pointless churn for a fixed-layout panel.
    io.IniFilename = nullptr;

    applyTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device_, context_);
    imguiInit_ = true;
    return true;
}

void Renderer::destroy() {
    if (imguiInit_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInit_ = false;
    }
    releaseRenderTarget();
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (context_)   { context_->Release();   context_   = nullptr; }
    if (device_)    { device_->Release();    device_    = nullptr; }
}

void Renderer::onResize(uint32_t w, uint32_t h) {
    // Defer the actual resize to the next frame: WM_SIZE can arrive while a
    // drag is in progress, and resizing the swapchain per pixel is wasteful.
    pendingW_ = w;
    pendingH_ = h;
}

void Renderer::beginFrame() {
    if (pendingW_ && pendingH_ && swapChain_) {
        releaseRenderTarget();
        swapChain_->ResizeBuffers(0, pendingW_, pendingH_, DXGI_FORMAT_UNKNOWN, 0);
        createRenderTarget();
        pendingW_ = pendingH_ = 0;
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

bool Renderer::endFrame(bool vsync) {
    ImGui::Render();
    const float clear[4] = { kColBackground[0], kColBackground[1], kColBackground[2], 1.0f };
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Present is the only thing pacing the visible loop, so its return value
    // is load-bearing: while occluded it returns immediately instead of
    // waiting for vsync.
    const HRESULT hr = swapChain_->Present(vsync ? 1 : 0, 0);
    return hr == DXGI_STATUS_OCCLUDED;
}

bool Renderer::stillOccluded() {
    if (!swapChain_) return false;
    return swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED;
}

} // namespace audiomon::ui
