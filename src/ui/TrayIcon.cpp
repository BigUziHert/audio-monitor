#include "ui/TrayIcon.h"
#include <cwchar>

namespace audiomon::ui {

UINT TrayIcon::taskbarCreatedMessage() {
    // Registered once per process; the same value comes back on repeat calls.
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

bool TrayIcon::add(HWND owner, HICON icon, const wchar_t* tooltip) {
    data_ = {};
    data_.cbSize           = sizeof(NOTIFYICONDATAW);
    data_.hWnd             = owner;
    data_.uID              = 1;
    data_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = kTrayCallbackMessage;
    data_.hIcon            = icon;
    std::wcsncpy(data_.szTip, tooltip, (sizeof(data_.szTip) / sizeof(wchar_t)) - 1);

    if (!Shell_NotifyIconW(NIM_ADD, &data_)) return false;

    // VERSION_4 gives richer callback packing and correct tooltip behaviour on
    // modern shells.
    data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    added_ = true;
    return true;
}

void TrayIcon::remove() {
    if (!added_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
}

void TrayIcon::setTooltip(const wchar_t* tooltip) {
    if (!added_) return;
    data_.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wcsncpy(data_.szTip, tooltip, (sizeof(data_.szTip) / sizeof(wchar_t)) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
}

UINT TrayIcon::showMenu(HWND owner) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;

    AppendMenuW(menu, MF_STRING, kTrayCmdRestore, L"&Open mixer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdExit, L"E&xit");
    SetMenuDefaultItem(menu, kTrayCmdRestore, FALSE);

    POINT pt;
    GetCursorPos(&pt);

    // Without this the menu will not dismiss when the user clicks elsewhere:
    // TrackPopupMenu only closes on an outside click if its owner window is in
    // the foreground. The trailing PostMessage is the documented companion
    // that lets the menu tear down cleanly afterwards.
    SetForegroundWindow(owner);

    const UINT cmd = TrackPopupMenu(menu,
                                    TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    return cmd;
}

} // namespace audiomon::ui
