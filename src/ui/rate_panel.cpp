#include "ui/rate_panel.h"

#include <cwchar>
#include <utility>

namespace ui {

void RatePanel::create(HWND parent, HINSTANCE inst, int first_id) {
    first_id_ = first_id;
    buttons_.clear();
    for (size_t i = 0; i < kMaxButtons; ++i) {
        // WS_GROUP nur auf dem ersten Knopf: damit fasst Windows die Reihe als eine
        // Radiogruppe auf und die Pfeiltasten wandern sauber durch.
        const DWORD style = WS_CHILD | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP | WS_TABSTOP : 0);
        HWND b = CreateWindowExW(0, L"Button", L"", style, 0, 0, 10, 10, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(first_id + static_cast<int>(i))),
                                 inst, nullptr);
        buttons_.push_back(b);
    }
}

void RatePanel::set_rates(const std::vector<uint16_t>& rates_hz, uint16_t current_hz) {
    // Absteigend anzeigen: die schnellste Rate zuerst, so wie man sie auch sucht.
    rates_ = rates_hz;
    for (size_t i = 0; i + 1 < rates_.size(); ++i)
        for (size_t j = i + 1; j < rates_.size(); ++j)
            if (rates_[j] > rates_[i]) std::swap(rates_[i], rates_[j]);

    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (i < rates_.size()) {
            wchar_t label[24];
            swprintf(label, 24, L"%u Hz", rates_[i]);
            SetWindowTextW(buttons_[i], label);
            ShowWindow(buttons_[i], SW_SHOW);
            SendMessageW(buttons_[i], BM_SETCHECK,
                         rates_[i] == current_hz ? BST_CHECKED : BST_UNCHECKED, 0);
        } else {
            SendMessageW(buttons_[i], BM_SETCHECK, BST_UNCHECKED, 0);
            ShowWindow(buttons_[i], SW_HIDE);
        }
    }
}

void RatePanel::layout(int x, int y, int button_w, int button_h, int gap) {
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (i >= rates_.size()) continue;
        MoveWindow(buttons_[i], x + static_cast<int>(i) * (button_w + gap), y,
                   button_w, button_h, TRUE);
    }
}

void RatePanel::set_font(HFONT font) {
    for (HWND b : buttons_) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void RatePanel::enable(bool on) {
    for (HWND b : buttons_) EnableWindow(b, on ? TRUE : FALSE);
}

uint16_t RatePanel::rate_for_id(int id) const {
    const int i = id - first_id_;
    if (i < 0 || static_cast<size_t>(i) >= rates_.size()) return 0;
    return rates_[static_cast<size_t>(i)];
}

} // namespace ui
