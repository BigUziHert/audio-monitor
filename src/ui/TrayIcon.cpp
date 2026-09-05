#include "ui/TrayIcon.h"

namespace audiomon::ui {
namespace {

// Bounded copy that always null-terminates.
//
// wcsncpy would do here -- the existing use was correct -- but MSVC deprecates
// it, and silencing that with _CRT_SECURE_NO_WARNINGS would switch off a whole
// class of genuinely useful warnings across the project. Taking the array by
// reference means the capacity comes from the type and cannot be passed wrong.
template <size_t N>
void copyBounded(wchar_t (&dst)[N], const wchar_t* src) {
    size_t i = 0;
    if (src) { for (; i + 1 < N && src[i]; ++i) dst[i] = src[i]; }
    dst[i] = L'\0';
}

} // namespace

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
    copyBounded(data_.szTip, tooltip);

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
    copyBounded(data_.szTip, tooltip);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
}

UINT TrayIcon::showMenu(HWND owner, POINT position) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return 0;

    AppendMenuW(menu, MF_STRING, kTrayCmdRestore, L"&Open mixer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayCmdExit, L"E&xit");
    SetMenuDefaultItem(menu, kTrayCmdRestore, FALSE);

    if (position.x == -1 && position.y == -1) GetCursorPos(&position);

    // Without this the menu will not dismiss when the user clicks elsewhere:
    // TrackPopupMenu only closes on an outside click if its owner window is in
    // the foreground. The trailing PostMessage is the documented companion
    // that lets the menu tear down cleanly afterwards.
    SetForegroundWindow(owner);

    const UINT cmd = TrackPopupMenu(menu,
                                    TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                    position.x, position.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    return cmd;
}

} // namespace audiomon::ui
