#include "ui/gkey_panel.h"

#include <cwchar>

namespace ui {
namespace {

// Der Hook braucht globalen Zustand -- die Callback-Signatur laesst nichts anderes zu.
// Es kann immer nur eine Aufnahme gleichzeitig laufen, deshalb reicht ein Zeiger.
GKeyPanel* g_capturing = nullptr;
HHOOK      g_hook = nullptr;

LRESULT CALLBACK keyboard_hook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_capturing) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
        if (down || up) {
            g_capturing->feed_key(static_cast<uint16_t>(kb->vkCode), down);
            return 1;   // schlucken: die Kombination soll nichts ausloesen
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

void install_hook(GKeyPanel* panel) {
    if (g_hook) return;
    g_capturing = panel;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook, GetModuleHandleW(nullptr), 0);
    if (!g_hook) g_capturing = nullptr;
}

void remove_hook() {
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    g_capturing = nullptr;
}

HWND make_static(HWND parent, HINSTANCE inst, const wchar_t* text, DWORD extra = 0) {
    return CreateWindowExW(0, L"Static", text, WS_CHILD | extra,
                           0, 0, 10, 10, parent, nullptr, inst, nullptr);
}

} // namespace

void GKeyPanel::create(HWND parent, HINSTANCE inst, int id_change, int id_clear) {
    parent_ = parent;
    id_change_ = id_change;
    id_clear_ = id_clear;

    label_.clear(); value_.clear(); change_.clear(); clear_.clear();
    display_.assign(kMaxKeys, L"—");

    for (size_t i = 0; i < kMaxKeys; ++i) {
        wchar_t name[8];
        swprintf(name, 8, L"G%zu", i + 1);
        label_.push_back(make_static(parent, inst, name));
        value_.push_back(make_static(parent, inst, L"—", SS_PATHELLIPSIS));
        change_.push_back(CreateWindowExW(
            0, L"Button", L"Ändern", WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_change + static_cast<int>(i))),
            inst, nullptr));
        clear_.push_back(CreateWindowExW(
            0, L"Button", L"Löschen", WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id_clear + static_cast<int>(i))),
            inst, nullptr));
    }
}

void GKeyPanel::set_key_count(uint8_t count) {
    count_ = count < kMaxKeys ? count : static_cast<uint8_t>(kMaxKeys);
}

void GKeyPanel::set_binding(uint8_t gkey, const std::wstring& display) {
    if (gkey == 0 || gkey > kMaxKeys) return;
    display_[gkey - 1] = display.empty() ? L"—" : display;
    if (capture_target_ != gkey) SetWindowTextW(value_[gkey - 1], display_[gkey - 1].c_str());
}

void GKeyPanel::set_enabled(bool on) {
    for (size_t i = 0; i < kMaxKeys; ++i) {
        EnableWindow(change_[i], on ? TRUE : FALSE);
        EnableWindow(clear_[i], on ? TRUE : FALSE);
    }
}

void GKeyPanel::show(bool visible) {
    for (size_t i = 0; i < kMaxKeys; ++i) {
        const int cmd = (visible && i < count_) ? SW_SHOW : SW_HIDE;
        ShowWindow(label_[i], cmd);
        ShowWindow(value_[i], cmd);
        ShowWindow(change_[i], cmd);
        ShowWindow(clear_[i], cmd);
    }
    if (!visible) cancel_capture();
}

int GKeyPanel::height(int row_h, int gap) const {
    if (count_ == 0) return 0;
    return count_ * row_h + (count_ - 1) * gap;
}

void GKeyPanel::layout(int x, int y, int width, int row_h, int gap) {
    const int label_w  = MulDiv(width, 8, 100);
    const int btn_w    = MulDiv(width, 20, 100);
    const int spacing  = gap;
    const int value_w  = width - label_w - 2 * btn_w - 3 * spacing;

    for (size_t i = 0; i < count_; ++i) {
        const int row_y = y + static_cast<int>(i) * (row_h + gap);
        int cx = x;
        MoveWindow(label_[i], cx, row_y + row_h / 6, label_w, row_h, TRUE);
        cx += label_w + spacing;
        MoveWindow(value_[i], cx, row_y + row_h / 6, value_w, row_h, TRUE);
        cx += value_w + spacing;
        MoveWindow(change_[i], cx, row_y, btn_w, row_h, TRUE);
        cx += btn_w + spacing;
        MoveWindow(clear_[i], cx, row_y, btn_w, row_h, TRUE);
    }
}

void GKeyPanel::set_font(HFONT font) {
    for (size_t i = 0; i < kMaxKeys; ++i) {
        SendMessageW(label_[i],  WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(value_[i],  WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(change_[i], WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(clear_[i],  WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

bool GKeyPanel::on_command(int id) {
    if (id >= id_change_ && id < id_change_ + static_cast<int>(kMaxKeys)) {
        begin_capture(static_cast<uint8_t>(id - id_change_ + 1));
        return true;
    }
    if (id >= id_clear_ && id < id_clear_ + static_cast<int>(kMaxKeys)) {
        const uint8_t gkey = static_cast<uint8_t>(id - id_clear_ + 1);
        cancel_capture();
        captured_ = app::Keystroke{};
        set_binding(gkey, L"");
        PostMessageW(parent_, WM_GKEY_CAPTURED, gkey, 0);
        return true;
    }
    return false;
}

void GKeyPanel::begin_capture(uint8_t gkey) {
    cancel_capture();
    capture_target_ = gkey;
    pending_ = app::Keystroke{};
    SetWindowTextW(value_[gkey - 1], L"Tasten drücken … (Esc bricht ab)");
    install_hook(this);
    if (!g_hook) {   // Hook verweigert -- lieber ehrlich abbrechen als still nichts tun
        capture_target_ = 0;
        SetWindowTextW(value_[gkey - 1], display_[gkey - 1].c_str());
    }
}

void GKeyPanel::cancel_capture() {
    if (!capture_target_) return;
    const uint8_t gkey = capture_target_;
    capture_target_ = 0;
    remove_hook();
    SetWindowTextW(value_[gkey - 1], display_[gkey - 1].c_str());
}

void GKeyPanel::refresh_capture_text() {
    if (!capture_target_) return;
    std::wstring preview;
    if (pending_.ctrl)  preview += L"Strg+";
    if (pending_.alt)   preview += L"Alt+";
    if (pending_.shift) preview += L"Umschalt+";
    if (pending_.win)   preview += L"Win+";
    if (preview.empty()) preview = L"Tasten drücken … (Esc bricht ab)";
    SetWindowTextW(value_[capture_target_ - 1], preview.c_str());
}

bool GKeyPanel::feed_key(uint16_t vk, bool down) {
    if (!capture_target_) return false;

    if (down && vk == VK_ESCAPE) { cancel_capture(); return true; }

    // Modifier sammeln sich an, bis eine echte Taste kommt.
    if (app::is_modifier(vk)) {
        const bool on = down;
        switch (vk) {
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: pending_.ctrl  = on; break;
            case VK_MENU:    case VK_LMENU:    case VK_RMENU:    pending_.alt   = on; break;
            case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:   pending_.shift = on; break;
            case VK_LWIN:    case VK_RWIN:                       pending_.win   = on; break;
            default: break;
        }
        refresh_capture_text();
        return true;
    }

    if (!down) return true;

    const uint8_t gkey = capture_target_;
    captured_ = pending_;
    captured_.vk = vk;

    capture_target_ = 0;
    remove_hook();
    display_[gkey - 1] = app::to_display(captured_);
    SetWindowTextW(value_[gkey - 1], display_[gkey - 1].c_str());
    PostMessageW(parent_, WM_GKEY_CAPTURED, gkey, 0);
    return true;
}

} // namespace ui
