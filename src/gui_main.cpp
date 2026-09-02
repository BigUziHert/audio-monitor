//
// Tray-resident mixer application.
//
// Minimize hides to the tray, releasing the renderer while audio continues.
// Close exits unless the user enables close-to-tray in Settings.
// The dashboard owns the title bar; Windows still handles sizing and snapping.
//
#include "audio/AudioEngine.h"
#include "config/Config.h"
#include "ui/MixerWindow.h"
#include "ui/Renderer.h"
#include "ui/TrayIcon.h"
#include "util/Log.h"
#include "util/Startup.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>

#include <chrono>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace audiomon;

namespace {

constexpr wchar_t kWindowClass[] = L"AudioMonitorWindow";
constexpr wchar_t kWindowTitle[] = L"Audio Monitor";
constexpr wchar_t kMutexName[]   = L"Local\\AudioMonitorSingleInstance";
constexpr int     kWindowWidth   = 1440;
constexpr int     kWindowHeight  = 890;

// Sent by a second instance to bring the running one to the front.
UINT g_showMessage = 0;

struct App {
    AudioEngine      engine;
    Config           config;
    ui::MixerWindow  mixer;
    ui::Renderer     renderer;
    ui::TrayIcon     tray;
    HWND             hwnd    = nullptr;
    bool             visible = false;
    bool             engineRunning = false;
    bool             occluded = false;
    bool             quitting = false;
    bool             configDirty = false;
    std::chrono::steady_clock::time_point lastFrame{};
    std::chrono::steady_clock::time_point lastSave{};
};

App* g_app = nullptr;

void showWindow(App& app) {
    if (!app.renderer.ensureCreated(app.hwnd)) {
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device for the mixer window.\n"
                             L"Audio mixing is unaffected and continues in the background.",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    app.visible = true;
    ShowWindow(app.hwnd, SW_SHOW);
    SetForegroundWindow(app.hwnd);
    app.lastFrame = std::chrono::steady_clock::now();
}

void saveConfigIfDirty(App& app, bool force);

void hideWindow(App& app) {
    // Every path into the tray flushes first. Putting it here rather than at
    // each call site means a future hide path cannot forget -- the tray-icon
    // toggle already had.
    saveConfigIfDirty(app, true);
    app.visible = false;
    ShowWindow(app.hwnd, SW_HIDE);
    // Give the GPU memory back: the machine is probably running a game.
    app.renderer.destroy();
}

void saveConfigIfDirty(App& app, bool force) {
    if (!app.configDirty) return;
    const auto now = std::chrono::steady_clock::now();
    // Coalesce: dragging a fader would otherwise write the file every frame.
    if (!force && std::chrono::duration_cast<std::chrono::milliseconds>(now - app.lastSave).count() < 1500) {
        return;
    }
    // Only fold runtime state back when the engine actually came up. If it
    // failed to start, its atomics still hold construction defaults, and
    // copying those over the loaded config would silently reset every fader
    // and mute the user had saved.
    if (app.engine.running()) app.engine.updateConfigFromRuntime(app.config);
    if (!app.config.save()) return;  // retain dirty state and retry a failed save
    app.configDirty = false;
    app.lastSave    = now;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = g_app;

    if (app && app->visible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    if (msg == ui::TrayIcon::taskbarCreatedMessage() && app) {
        // Explorer restarted and took the icon with it.
        app->tray.add(hwnd, reinterpret_cast<HICON>(
                          GetClassLongPtrW(hwnd, GCLP_HICONSM)), kWindowTitle);
        return 0;
    }
    if (g_showMessage && msg == g_showMessage && app) { showWindow(*app); return 0; }

    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                // Keep the client in the work area when maximized.
                if (IsZoomed(hwnd)) {
                    MONITORINFO info{sizeof(info)};
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info);
                    reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] = info.rcWork;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            if (IsZoomed(hwnd)) return HTCLIENT;
            POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(hwnd,&pt);
            RECT rc{}; GetClientRect(hwnd,&rc);
            const int edge=7;
            const bool left=pt.x<edge,right=pt.x>=rc.right-edge,top=pt.y<edge,bottom=pt.y>=rc.bottom-edge;
            if(top)return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
            if(bottom)return left?HTBOTTOMLEFT:right?HTBOTTOMRIGHT:HTBOTTOM;
            if(left)return HTLEFT; if(right)return HTRIGHT;
            return HTCLIENT;
        }
        case WM_DPICHANGED: {
            const auto* rect=reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);
            return 0;
        }
        case ui::kTrayCallbackMessage: {
            // Under NOTIFYICON_VERSION_4 the event is in the low word of lParam.
            switch (LOWORD(lp)) {
                // A click SHOWS the window; it never toggles. This used to
                // toggle, which flashed the window open and shut: under
                // NOTIFYICON_VERSION_4 a single left click delivers BOTH
                // WM_LBUTTONUP and NIN_SELECT, so a toggle flips twice, and a
                // double-click adds WM_LBUTTONDBLCLK and another WM_LBUTTONUP
                // on top. showWindow is idempotent, so however many of these
                // arrive the result is one visible, foregrounded window.
                // Hiding is the close button's job.
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONDBLCLK:
                    if (app) showWindow(*app);
                    return 0;
                case WM_LBUTTONUP:
                    // Redundant with the NIN_SELECT that follows it under
                    // VERSION_4; acting on both is exactly what caused the flash.
                    return 0;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP: {
                    if (!app) return 0;
                    const UINT cmd = app->tray.showMenu(hwnd);
                    if (cmd == ui::kTrayCmdRestore) showWindow(*app);
                    else if (cmd == ui::kTrayCmdExit) { app->quitting = true; DestroyWindow(hwnd); }
                    return 0;
                }
                default: return 0;
            }
        }

        case WM_SYSCOMMAND:
            // Minimise means "go to the tray", not "sit in the taskbar".
            if ((wp & 0xFFF0) == SC_MINIMIZE) {
                if (app) hideWindow(*app);
                return 0;
            }
            break;

        case WM_CLOSE:
            if (app && app->config.closeToTray) hideWindow(*app);
            else DestroyWindow(hwnd);
            return 0;

        case WM_SIZE:
            if (app && wp != SIZE_MINIMIZED) {
                app->renderer.onResize(LOWORD(lp), HIWORD(lp));
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = 960;
            mmi->ptMinTrackSize.y = 600;
            return 0;
        }

        case WM_DESTROY:
            // Delete the icon while the owning HWND is still alive; a
            // NIM_DELETE against a dead window leaves a ghost icon in the
            // notification area until the user hovers over it.
            if (app) app->tray.remove();
            PostQuitMessage(0);
            return 0;

        default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool commandLineHas(const wchar_t* flag) {
    const wchar_t* cmd = GetCommandLineW();
    return cmd && wcsstr(cmd, flag) != nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    g_showMessage = RegisterWindowMessageW(L"AudioMonitorShowWindow");

    // Single instance: a second launch just raises the first one's window.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, g_showMessage, 0, 0);
        return 0;
    }

    // MTA, deliberately, and it matters.
    //
    // AudioEngine::start runs on this thread and creates the
    // IMMDeviceEnumerator here, but that enumerator is then used from the
    // three capture threads, the render thread and the supervisor thread --
    // all of which are MTA. An object created in an STA and called directly
    // from another apartment is an illegal cross-apartment call: it needs
    // marshalling through a proxy, and without one it may appear to work and
    // then fail unpredictably. Putting every thread in the MTA makes sharing
    // the raw pointer correct rather than lucky.
    //
    // Nothing in this UI needs an STA: Shell_NotifyIcon and TrackPopupMenu are
    // plain Win32, SHGetKnownFolderPath is apartment-agnostic, and neither
    // ImGui nor D3D11 touches COM.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) return 1;

    log::init(Config::appDataDir());
    LOG_INFO("audio-monitor starting");

    App app;
    g_app = &app;
    app.config = Config::load();
    // Keep the checkbox honest if the user removed the Run entry by hand.
    app.config.startWithWindows = startup::isEnabled();

    HICON icon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    wc.hIcon         = icon;
    wc.hIconSm       = icon;
    RegisterClassExW(&wc);

    app.hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle,
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth, kWindowHeight,
                               nullptr, nullptr, hInstance, nullptr);
    if (!app.hwnd) { CoUninitialize(); return 1; }

    SetWindowPos(app.hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
    const DWORD cornerPreference=2; // DWMWCP_ROUND, harmless on older Windows.
    DwmSetWindowAttribute(app.hwnd,33,&cornerPreference,sizeof(cornerPreference));
    app.tray.add(app.hwnd, icon, kWindowTitle);
    app.mixer.init(&app.engine, &app.config, app.hwnd);

    app.engineRunning = app.engine.start(app.config);
    if (!app.engineRunning) {
        MessageBoxW(app.hwnd, L"Could not start the audio engine. See audio-monitor.log in "
                              L"%APPDATA%\\audio-monitor for details.",
                    kWindowTitle, MB_OK | MB_ICONERROR);
    }

    const bool startHidden = app.config.startMinimized || commandLineHas(L"--tray");
    if (!startHidden) showWindow(app);

    app.lastFrame = std::chrono::steady_clock::now();
    app.lastSave  = app.lastFrame;

    MSG msg{};
    bool running = true;
    while (running) {
        if (app.visible) {
            // Visible: drain messages without blocking, then draw a frame.
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = false; break; }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running) break;
            if (!app.visible || !app.renderer.alive()) continue;

            // Present does not wait for vsync while the window is occluded --
            // covered by a fullscreen game, or the workstation locked -- so
            // without this the loop would spin a core flat out on a machine
            // that is busy doing something the user actually cares about.
            if (app.occluded) {
                if (app.renderer.stillOccluded()) { Sleep(16); continue; }
                app.occluded = false;
            }

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - app.lastFrame).count();
            app.lastFrame = now;

            RECT rc{};
            GetClientRect(app.hwnd, &rc);

            app.renderer.beginFrame();
            if (app.mixer.draw(dt, rc.right - rc.left, rc.bottom - rc.top)) app.configDirty = true;
            app.occluded = app.renderer.endFrame(true);   // vsync paces the loop

            saveConfigIfDirty(app, false);

            if (app.mixer.exitRequested()) { app.quitting = true; DestroyWindow(app.hwnd); }
        } else {
            // Hidden: block. The process should be doing nothing at all here
            // beyond the audio threads.
            if (!GetMessageW(&msg, nullptr, 0, 0)) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    LOG_INFO("audio-monitor shutting down");
    saveConfigIfDirty(app, true);
    app.engine.stop();
    app.renderer.destroy();
    app.tray.remove();
    log::shutdown();

    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    CoUninitialize();
    return 0;
}
