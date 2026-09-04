#include "ui/Renderer.h"
#include <windows.h>
#include <cstdio>

namespace audiomon::ui {
struct RendererTestAccess {
    static ID3D11Texture2D* backBuffer(Renderer& renderer) {
        ID3D11Texture2D* texture = nullptr;
        if (renderer.swapChain_) renderer.swapChain_->GetBuffer(0, IID_PPV_ARGS(&texture));
        return texture;
    }
    static bool sizeIs(Renderer& renderer, UINT width, UINT height) {
        auto* texture = backBuffer(renderer);
        if (!texture) return false;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        texture->Release();
        return desc.Width == width && desc.Height == height;
    }
};
} // namespace audiomon::ui

int main() {
    using namespace audiomon::ui;
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("FAIL: %s\n", label); ++failed; }
        return ok;
    };
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"AudioMonitorRendererTest";
    if (!RegisterClassW(&wc)) return 1;
    // Hidden window: exercises D3D without changing focus or the running app.
    const auto window = CreateWindowW(wc.lpszClassName, L"Renderer test", WS_POPUP,
                                     0, 0, 640, 480, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    {
        Renderer renderer;
        auto draw = [&] {
            if (!check(renderer.beginFrame(), "begin a valid frame")) return false;
            renderer.endFrame(false);
            return true;
        };
        if (check(renderer.ensureCreated(window), "create renderer")) {
            draw(); // Leaves the render target bound to the D3D immediate context.
            SetWindowPos(window, nullptr, 0, 0, 900, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            renderer.onResize(900, 600);
            draw();
            check(RendererTestAccess::sizeIs(renderer, 900, 600), "resize releases the bound back buffer");

            // Force a real ResizeBuffers failure by keeping an extra reference.
            auto* held = RendererTestAccess::backBuffer(renderer);
            SetWindowPos(window, nullptr, 0, 0, 800, 500, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            renderer.onResize(800, 500);
            const bool started = renderer.beginFrame();
            check(!started, "failed resize skips rendering safely");
            if (started) renderer.endFrame(false);
            if (held) held->Release();
            draw();
            check(RendererTestAccess::sizeIs(renderer, 800, 500), "renderer recovers after failed resize");

            renderer.destroy();
            renderer.onResize(800, 500);
            if (check(renderer.ensureCreated(window), "recreate after tray teardown")) draw();
        }
    }
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, instance);
    std::printf("Renderer tests: %d failures\n", failed);
    return failed ? 1 : 0;
}
