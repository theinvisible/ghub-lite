// Selbst gezeichneter Schieberegler.
//
// Die comctl32-Trackbar laesst sich im Dark Mode nicht ueberzeugend umfaerben (Rinne und
// Griff bleiben hell), deshalb hier ein eigenes Control. Es ist bewusst schlank: eine
// Rinne, ein Griff, Tastaturbedienung, Rasterung auf eine Schrittweite.
#pragma once

#include <windows.h>

namespace ui {

class Theme;

inline constexpr wchar_t kSliderClass[] = L"GhubSlider";

// An das Elternfenster: wParam = MAKEWPARAM(ctrlId, final), lParam = Wert.
// final == 0 waehrend des Ziehens, 1 beim Loslassen bzw. bei Tastenbedienung.
inline constexpr UINT WM_SLIDER_CHANGED = WM_APP + 10;

enum SliderMessage : UINT {
    SLM_SETRANGE = WM_USER + 1,   // wParam = min, lParam = max
    SLM_SETSTEP  = WM_USER + 2,   // wParam = Schrittweite
    SLM_SETVALUE = WM_USER + 3,   // wParam = Wert
    SLM_GETVALUE = WM_USER + 4,
    SLM_SETTHEME = WM_USER + 5,   // lParam = const Theme*
};

bool register_slider_class(HINSTANCE inst);

} // namespace ui
