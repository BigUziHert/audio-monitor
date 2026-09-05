#pragma once
//
// Notification-area icon and its context menu.
//
#include <windows.h>
#include <shellapi.h>   // WIN32_LEAN_AND_MEAN excludes this

namespace audiomon::ui {

// Chosen in the WM_APP range so it cannot collide with a system message.
inline constexpr UINT kTrayCallbackMessage = WM_APP + 1;

enum TrayCommand : UINT {
    kTrayCmdRestore = 40001,
    kTrayCmdExit    = 40002,
};

class TrayIcon {
public:
    TrayIcon() = default;
    // Without this an abnormal exit leaves a dead icon in the notification
    // area until the user hovers over it.
    ~TrayIcon() { remove(); }

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool add(HWND owner, HICON icon, const wchar_t* tooltip);
    void remove();
    void setTooltip(const wchar_t* tooltip);

    // Explorer restarting destroys the icon; re-add it when TaskbarCreated
    // arrives. Returns the registered message to compare against.
    static UINT taskbarCreatedMessage();

    // Shows the context menu at the shell-provided position. Returns the chosen
    // command, or 0.
    UINT showMenu(HWND owner, POINT position);

private:
    NOTIFYICONDATAW data_{};
    bool            added_ = false;
};

} // namespace audiomon::ui
