#include "ui/tray.h"

#include <shellapi.h>

#include <cstring>

namespace ui {
namespace {

void copy_bounded(wchar_t* dst, size_t cap, const std::wstring& src) {
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    wmemcpy(dst, src.c_str(), n);
    dst[n] = L'\0';
}

} // namespace

Tray::~Tray() { remove(); }

bool Tray::add(HWND owner, UINT callback_msg, HICON icon, const std::wstring& tip) {
    if (added_) return true;

    nid_ = NOTIFYICONDATAW{};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = owner;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = callback_msg;
    nid_.hIcon = icon;
    copy_bounded(nid_.szTip, sizeof(nid_.szTip) / sizeof(wchar_t), tip);
    return restore();
}

bool Tray::restore() {
    if (!nid_.hWnd) return false;
    // set_tip() und notify() haben uFlags auf ihren Teil verengt -- fuers Anlegen alles.
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (Shell_NotifyIconW(NIM_ADD, &nid_)) {
        nid_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid_);
        added_ = true;
    } else {
        // NIM_ADD scheitert auch, wenn das Symbol noch da ist (TaskbarCreated kommt nicht
        // nur nach einem Absturz). Dann gilt es weiter als angelegt.
        added_ = Shell_NotifyIconW(NIM_MODIFY, &nid_) != FALSE;
    }
    return added_;
}

void Tray::set_tip(const std::wstring& tip) {
    if (!added_) return;
    nid_.uFlags = NIF_TIP;
    copy_bounded(nid_.szTip, sizeof(nid_.szTip) / sizeof(wchar_t), tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::notify(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    nid_.uFlags = NIF_INFO;
    nid_.dwInfoFlags = NIIF_NONE;
    copy_bounded(nid_.szInfoTitle, sizeof(nid_.szInfoTitle) / sizeof(wchar_t), title);
    copy_bounded(nid_.szInfo, sizeof(nid_.szInfo) / sizeof(wchar_t), text);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::remove() {
    if (!added_) return;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    added_ = false;
}

} // namespace ui
