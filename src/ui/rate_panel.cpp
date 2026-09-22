#include "ui/rate_panel.h"

#include "ui/theme.h"

#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <utility>

namespace ui {

bool RatePanel::owns(HWND h) const {
    return std::find(buttons_.begin(), buttons_.end(), h) != buttons_.end();
}

LRESULT RatePanel::custom_draw(const NMCUSTOMDRAW& cd, const Theme& theme) const {
    // Hell zeichnet das Theme die Beschriftung richtig -- dort nichts anfassen.
    if (!theme.dark() || cd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;

    // Selbst oeffnen: GetWindowTheme() liefert fuer diese Knoepfe gemessen null. Das Oeffnen
    // am Fenster uebernimmt dessen DarkMode_Explorer aus SetWindowTheme, der Kreis bleibt
    // also dunkel.
    HWND b = cd.hdr.hwndFrom;
    const UINT dpi = GetDpiForWindow(b);
    HTHEME th = OpenThemeDataForDpi(b, L"Button", dpi);
    if (!th) return CDRF_DODEFAULT;

    HDC dc = cd.hdc;
    RECT rc = cd.rc;
    FillRect(dc, &rc, theme.bg_brush());

    const bool disabled = (cd.uItemState & CDIS_DISABLED) != 0;
    int state = disabled                           ? RBS_UNCHECKEDDISABLED
              : (cd.uItemState & CDIS_SELECTED)    ? RBS_UNCHECKEDPRESSED
              : (cd.uItemState & CDIS_HOT)         ? RBS_UNCHECKEDHOT
                                                   : RBS_UNCHECKEDNORMAL;
    if (Button_GetCheck(b) == BST_CHECKED) state += RBS_CHECKEDNORMAL - RBS_UNCHECKEDNORMAL;

    // Den Kreis weiter vom Theme: der ist im Dark Mode richtig und passt zu den Checkboxen.
    SIZE glyph{};
    GetThemePartSize(th, dc, BP_RADIOBUTTON, state, nullptr, TS_DRAW, &glyph);
    RECT g{rc.left, rc.top + (rc.bottom - rc.top - glyph.cy) / 2, 0, 0};
    g.right = g.left + glyph.cx;
    g.bottom = g.top + glyph.cy;
    DrawThemeBackground(th, dc, BP_RADIOBUTTON, state, &g, nullptr);

    wchar_t label[32] = {};
    GetWindowTextW(b, label, 32);
    RECT text{g.right + MulDiv(4, static_cast<int>(dpi), 96), rc.top, rc.right, rc.bottom};

    HGDIOBJ old_font = nullptr;
    if (auto font = reinterpret_cast<HFONT>(SendMessageW(b, WM_GETFONT, 0, 0)))
        old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? theme.colors().text_dim : theme.colors().text);
    constexpr UINT kFmt = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX;
    DrawTextW(dc, label, -1, &text, kFmt);

    // Fokusrahmen wie beim Original nur, wenn Windows gerade Tastatur-Hinweise zeigt.
    const auto ui_state = static_cast<UINT>(SendMessageW(b, WM_QUERYUISTATE, 0, 0));
    if ((cd.uItemState & CDIS_FOCUS) && !(ui_state & UISF_HIDEFOCUS)) {
        RECT fr = text;
        DrawTextW(dc, label, -1, &fr, kFmt | DT_CALCRECT);
        const int dy = ((text.bottom - text.top) - (fr.bottom - fr.top)) / 2;
        OffsetRect(&fr, 0, dy);
        InflateRect(&fr, 1, 1);
        DrawFocusRect(dc, &fr);
    }

    if (old_font) SelectObject(dc, old_font);
    CloseThemeData(th);
    return CDRF_SKIPDEFAULT;
}

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
