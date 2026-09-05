// Manual visual-QA harness for the real D3D11/ImGui dashboard. The native
// window is never shown or activated, configuration is entirely synthetic,
// and AudioEngine workers are never started.
#include "ui/MixerWindow.h"
#include "ui/Renderer.h"
#include "ui/Theme.h"

#include <windows.h>
#include <shellapi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace audiomon;

namespace audiomon {

// Establish the topology seen by the read-only status helpers without opening
// devices or claiming that a worker session is running.
struct AudioEngineTestAccess {
    static void prepareIdleSession(AudioEngine& engine, const Config& config) {
        engine.config_ = config;
        engine.sourceCount_ = std::min(config.sources.size(), size_t(kMaxSources));
        engine.outputCount_ = std::min(config.outputCount(), size_t(kMaxOutputs));
        engine.running_.store(false, std::memory_order_release);
        engine.monitoringState_.store(0, std::memory_order_release);
    }
};

} // namespace audiomon

namespace audiomon::ui {

struct RendererTestAccess {
    static bool renderWithoutPresent(Renderer& renderer) {
        if (!renderer.context_ || !renderer.rtv_) return false;
        ImGui::Render();
        const ImVec4 background =
            ImGui::ColorConvertU32ToFloat4(themePalette().background);
        const float clear[] = {background.x, background.y, background.z, background.w};
        renderer.context_->OMSetRenderTargets(1, &renderer.rtv_, nullptr);
        renderer.context_->ClearRenderTargetView(renderer.rtv_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        return true;
    }

    static bool readBackRgba(Renderer& renderer, std::vector<uint8_t>& pixels,
                             uint32_t& width, uint32_t& height) {
        if (!renderer.device_ || !renderer.context_ || !renderer.swapChain_) return false;

        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(renderer.swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
            !backBuffer)
            return false;

        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.SampleDesc.Count != 1 ||
            desc.Width == 0 || desc.Height == 0) {
            backBuffer->Release();
            return false;
        }

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging = nullptr;
        const HRESULT createResult = renderer.device_->CreateTexture2D(
            &stagingDesc, nullptr, &staging);
        if (FAILED(createResult) || !staging) {
            backBuffer->Release();
            return false;
        }

        renderer.context_->CopyResource(staging, backBuffer);
        backBuffer->Release();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(renderer.context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            staging->Release();
            return false;
        }

        width = desc.Width;
        height = desc.Height;
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        pixels.resize(rowBytes * height);
        const auto* source = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * rowBytes,
                        source + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);

        renderer.context_->Unmap(staging, 0);
        staging->Release();
        return true;
    }
};

struct MixerWindowTestAccess {
    static void bindSynthetic(MixerWindow& mixer, AudioEngine& engine, Config& config) {
        mixer.engine_ = &engine;
        mixer.config_ = &config;
        // A null application HWND prevents metering startup and all shell/UI
        // actions while still allowing the dashboard itself to be rendered.
        mixer.window_ = nullptr;
        mixer.visible_ = false;
        mixer.playback_ = {
            {L"synthetic-render-main", L"Studio Monitor Output", true, false},
            {L"synthetic-render-stream", L"Broadcast Mix Output", true, false},
            {L"synthetic-render-headset", L"USB Headset Game", true, true},
        };
        mixer.microphones_ = {
            {L"synthetic-capture-mic", L"Broadcast Microphone", false, false},
        };
        mixer.apps_ = {
            {4100, L"C:\\Synthetic\\Game.exe", L"Synthetic Game",
             {L"synthetic-render-headset"}, true},
            {4200, L"C:\\Synthetic\\Chat.exe", L"Synthetic Chat",
             {L"synthetic-render-headset"}, true},
        };
    }

    static void openSettings(MixerWindow& mixer, int page) {
        mixer.settingsPage_ = page;
        mixer.openSettings_ = true;
    }

    static void openAddSource(MixerWindow& mixer) {
        mixer.editSource_ = -1;
        mixer.draft_ = ChannelConfig{};
        mixer.name_[0] = '\0';
        mixer.openSource_ = true;
    }

    static void openConfigureOutput(MixerWindow& mixer, const Config& config) {
        mixer.editOutput_ = 0;
        mixer.outputDraft_ = config.outputAt(0);
        std::snprintf(mixer.outputName_, sizeof(mixer.outputName_), "%s",
                      mixer.outputDraft_.label.c_str());
        mixer.openOutput_ = true;
    }
};

} // namespace audiomon::ui

namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type = 0x4D42;
    uint32_t size = 0;
    uint16_t reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t pixelOffset = 0;
};
#pragma pack(pop)

static_assert(sizeof(BmpFileHeader) == 14);

bool writeAll(HANDLE file, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const DWORD request = static_cast<DWORD>(std::min<size_t>(
            size - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, request, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return true;
}

bool writeBmp(const std::wstring& path, const std::vector<uint8_t>& rgba,
              uint32_t width, uint32_t height) {
    const uint64_t pixelBytes64 = uint64_t(width) * uint64_t(height) * 4;
    const uint64_t fileBytes64 = sizeof(BmpFileHeader) + sizeof(BITMAPINFOHEADER) + pixelBytes64;
    if (rgba.size() != pixelBytes64 || fileBytes64 > std::numeric_limits<uint32_t>::max())
        return false;

    BmpFileHeader fileHeader;
    fileHeader.pixelOffset = sizeof(BmpFileHeader) + sizeof(BITMAPINFOHEADER);
    fileHeader.size = static_cast<uint32_t>(fileBytes64);

    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    // A positive BMP height stores scanlines bottom-up.
    info.biHeight = static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixelBytes64);

    std::vector<uint8_t> bgra(static_cast<size_t>(pixelBytes64));
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = rgba.data() + static_cast<size_t>(y) * rowBytes;
        uint8_t* destination = bgra.data() + static_cast<size_t>(height - 1 - y) * rowBytes;
        for (uint32_t x = 0; x < width; ++x) {
            destination[x * 4] = source[x * 4 + 2];
            destination[x * 4 + 1] = source[x * 4 + 1];
            destination[x * 4 + 2] = source[x * 4];
            destination[x * 4 + 3] = 255;
        }
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool ok = writeAll(file, &fileHeader, sizeof(fileHeader)) &&
                    writeAll(file, &info, sizeof(info)) &&
                    writeAll(file, bgra.data(), bgra.size()) &&
                    FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    if (!ok || !closed) {
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

Config syntheticConfig(ColorTheme theme) {
    Config config;
    config.sources.clear();
    config.clearOutputs();
    config.colorTheme = theme;
    config.bufferMillis = 50;

    ChannelConfig game;
    game.label = "Game Audio";
    game.kind = SourceKind::Playback;
    game.deviceId = L"synthetic-render-headset";
    game.deviceNameMatch = L"USB Headset Game";
    game.icon = "headphones";
    game.volume = .78f;

    ChannelConfig chat;
    chat.label = "Voice Chat";
    chat.kind = SourceKind::Application;
    chat.processPath = L"C:\\Synthetic\\Chat.exe";
    chat.icon = "chat";
    chat.volume = .64f;

    ChannelConfig microphone;
    microphone.label = "Broadcast Microphone";
    microphone.kind = SourceKind::Microphone;
    microphone.deviceId = L"synthetic-capture-mic";
    microphone.deviceNameMatch = L"Broadcast Microphone";
    microphone.icon = "microphone";
    microphone.volume = .86f;

    ChannelConfig music;
    music.label = "Music and Alerts";
    music.kind = SourceKind::Application;
    music.processPath = L"C:\\Synthetic\\Music.exe";
    music.icon = "wave";
    music.volume = .55f;
    music.muted = true;
    config.sources = {game, chat, microphone, music};

    ChannelConfig monitor;
    monitor.label = "Studio Monitors";
    monitor.deviceId = L"synthetic-render-main";
    monitor.deviceNameMatch = L"Studio Monitor Output";
    monitor.icon = "speaker";
    monitor.volume = .82f;
    config.addOutput(monitor);

    ChannelConfig stream;
    stream.label = "Broadcast Mix";
    stream.deviceId = L"synthetic-render-stream";
    stream.deviceNameMatch = L"Broadcast Mix Output";
    stream.icon = "screen";
    stream.volume = .9f;
    config.addOutput(stream);
    return config;
}

enum class Scene { Dashboard, AudioSettings, GeneralSettings, About, AddSource, ConfigureOutput };

struct Shot {
    const wchar_t* fileName;
    ColorTheme theme;
    Scene scene;
    uint32_t width;
    uint32_t height;
};

bool captureShot(HWND window, const std::filesystem::path& directory, const Shot& shot) {
    if (!SetWindowPos(window, nullptr, 0, 0, static_cast<int>(shot.width),
                      static_cast<int>(shot.height),
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
        return false;

    ui::Renderer renderer;
    if (!renderer.ensureCreated(window)) return false;
    ui::applyTheme(shot.theme);

    AudioEngine engine;
    Config config = syntheticConfig(shot.theme);
    AudioEngineTestAccess::prepareIdleSession(engine, config);
    ui::MixerWindow mixer;
    ui::MixerWindowTestAccess::bindSynthetic(mixer, engine, config);

    switch (shot.scene) {
        case Scene::Dashboard: break;
        case Scene::AudioSettings: ui::MixerWindowTestAccess::openSettings(mixer, 0); break;
        case Scene::GeneralSettings: ui::MixerWindowTestAccess::openSettings(mixer, 1); break;
        case Scene::About: ui::MixerWindowTestAccess::openSettings(mixer, 4); break;
        case Scene::AddSource: ui::MixerWindowTestAccess::openAddSource(mixer); break;
        case Scene::ConfigureOutput:
            ui::MixerWindowTestAccess::openConfigureOutput(mixer, config);
            break;
    }

    bool rendered = false;
    for (int frame = 0; frame < 2; ++frame) {
        for (size_t source = 0; source < config.sources.size(); ++source) {
            const float level = .18f + .13f * static_cast<float>(source);
            engine.channelPeak(static_cast<int>(source)).l.publish(level);
            engine.channelPeak(static_cast<int>(source)).r.publish(level * .86f);
        }
        if (!renderer.beginFrame()) {
            renderer.destroy();
            return false;
        }
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        mixer.draw(ImGui::GetIO().DeltaTime, static_cast<int>(shot.width),
                   static_cast<int>(shot.height));
        rendered = ui::RendererTestAccess::renderWithoutPresent(renderer);
        if (!rendered) break;
    }

    std::vector<uint8_t> rgba;
    uint32_t capturedWidth = 0;
    uint32_t capturedHeight = 0;
    const bool readBack = rendered && ui::RendererTestAccess::readBackRgba(
                                      renderer, rgba, capturedWidth, capturedHeight);
    const std::filesystem::path output = directory / shot.fileName;
    const bool saved = readBack && capturedWidth == shot.width && capturedHeight == shot.height &&
                       writeBmp(output.wstring(), rgba, capturedWidth, capturedHeight);
    renderer.destroy();

    if (saved)
        std::wprintf(L"saved %ls\n", output.c_str());
    else
        std::fwprintf(stderr, L"failed %ls\n", output.c_str());
    return saved;
}

} // namespace

int main() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments || argumentCount != 2) {
        std::fprintf(stderr, "usage: ui-snapshot-test <output-directory>\n");
        if (arguments) LocalFree(arguments);
        return 2;
    }
    const std::filesystem::path outputDirectory(arguments[1]);
    LocalFree(arguments);

    std::error_code directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);
    if (directoryError || !std::filesystem::is_directory(outputDirectory, directoryError)) {
        std::fwprintf(stderr, L"could not create output directory: %ls\n",
                      outputDirectory.c_str());
        return 2;
    }

    constexpr wchar_t windowClass[] = L"AudioMonitorUiSnapshotHarness";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = windowClass;
    if (!RegisterClassW(&wc)) {
        std::fprintf(stderr, "could not register hidden snapshot window\n");
        return 2;
    }

    // WS_EX_NOACTIVATE plus never calling ShowWindow keeps this harness wholly
    // offscreen and prevents it from disturbing the user's foreground app.
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass,
                                  L"Audio Monitor UI Snapshot", WS_POPUP,
                                  0, 0, 1440, 890, nullptr, nullptr, instance, nullptr);
    if (!window) {
        UnregisterClassW(windowClass, instance);
        std::fprintf(stderr, "could not create hidden snapshot window\n");
        return 2;
    }

    const Shot shots[] = {
        {L"dashboard-dark-1440x890.bmp", ColorTheme::Dark, Scene::Dashboard, 1440, 890},
        {L"dashboard-light-1440x890.bmp", ColorTheme::Light, Scene::Dashboard, 1440, 890},
        {L"settings-audio-dark-1440x890.bmp", ColorTheme::Dark, Scene::AudioSettings, 1440, 890},
        {L"settings-audio-light-1440x890.bmp", ColorTheme::Light, Scene::AudioSettings, 1440, 890},
        {L"settings-general-dark-1440x890.bmp", ColorTheme::Dark, Scene::GeneralSettings, 1440, 890},
        {L"settings-about-dark-1440x890.bmp", ColorTheme::Dark, Scene::About, 1440, 890},
        {L"add-source-dark-1440x890.bmp", ColorTheme::Dark, Scene::AddSource, 1440, 890},
        {L"add-source-light-1440x890.bmp", ColorTheme::Light, Scene::AddSource, 1440, 890},
        {L"configure-output-dark-1440x890.bmp", ColorTheme::Dark, Scene::ConfigureOutput, 1440, 890},
        {L"dashboard-light-960x600.bmp", ColorTheme::Light, Scene::Dashboard, 960, 600},
        {L"settings-about-light-960x600.bmp", ColorTheme::Light, Scene::About, 960, 600},
    };

    int failures = 0;
    for (const Shot& shot : shots)
        if (!captureShot(window, outputDirectory, shot)) ++failures;

    DestroyWindow(window);
    UnregisterClassW(windowClass, instance);
    std::printf("UI snapshots: %zu written, %d failures\n",
                std::size(shots) - static_cast<size_t>(failures), failures);
    return failures == 0 ? 0 : 1;
}
