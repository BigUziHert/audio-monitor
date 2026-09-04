#pragma once
//
// Dear ImGui on a D3D11 swapchain.
//
// The device and swapchain are created LAZILY, on the first show, and torn
// down again when the window is hidden. This app spends essentially all of its
// life in the notification area on a machine that is busy running a game, so
// holding a D3D device, a swapchain and compiled shaders the whole time buys
// nothing and costs VRAM. Re-creating on show takes a few milliseconds.
//
#include <d3d11.h>
#include <cstdint>

namespace audiomon::ui {

class Renderer {
public:
    Renderer() = default;
    // Belt and braces: the normal shutdown path calls destroy() explicitly,
    // but an early return must not leak a D3D device and an ImGui context.
    ~Renderer() { destroy(); }

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool ensureCreated(void* hwnd);   // idempotent
    void destroy();
    bool alive() const { return device_ != nullptr; }

    void onResize(uint32_t width, uint32_t height);

    // False means no frame was started; the caller should throttle and retry.
    bool beginFrame();

    // Returns true if the swapchain is occluded (the window is covered, the
    // workstation is locked, or a fullscreen app owns the output). Present
    // stops blocking on vsync in that state, so the caller must throttle
    // itself or the render loop becomes a spin.
    bool endFrame(bool vsync);

    // Cheap poll used while occluded, to notice when the window is visible
    // again without drawing anything.
    bool stillOccluded();

private:
    friend struct RendererTestAccess;
    bool createDeviceObjects(void* hwnd);
    bool createRenderTarget();
    void releaseRenderTarget();

    ID3D11Device*           device_    = nullptr;
    ID3D11DeviceContext*    context_   = nullptr;
    IDXGISwapChain*         swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_       = nullptr;
    void*                   hwnd_      = nullptr;
    bool                    imguiInit_ = false;
    bool                    recoveryNeeded_ = false;
    uint32_t                pendingW_  = 0;
    uint32_t                pendingH_  = 0;
};

} // namespace audiomon::ui
