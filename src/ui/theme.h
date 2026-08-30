// Dark-Mode-Unterstuetzung fuer Win32.
//
// Drei Bausteine sind noetig, weil Windows keinen einzelnen Schalter dafuer hat:
//   1. Titelleiste  -> DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)
//   2. Systemmenues -> undokumentierte uxtheme-Ordinals (seit 1809 stabil, aber ungestuetzt;
//                      jeder Export wird geprueft, fehlt einer, bleibt die UI hell)
//   3. Controls     -> SetWindowTheme(DarkMode_Explorer / DarkMode_CFD) plus WM_CTLCOLOR*
#pragma once

#include <windows.h>

namespace ui {

struct Palette {
    COLORREF bg;          // Fensterhintergrund
    COLORREF panel;       // abgesetzte Flaeche
    COLORREF text;
    COLORREF text_dim;
    COLORREF accent;
    COLORREF border;
    COLORREF track;       // Slider-Rinne
    COLORREF warning;     // Fehlermeldung in der Statuszeile
    COLORREF disabled;
};

class Theme {
public:
    // Einmalig beim Programmstart: uxtheme-Ordinals laden und den App-Modus anmelden.
    static void init_process();

    // Liest den Systemzustand neu ein. true, wenn sich etwas geaendert hat.
    bool refresh();

    bool dark() const { return dark_; }
    const Palette& colors() const { return pal_; }

    HBRUSH bg_brush() const { return bg_brush_; }
    HBRUSH panel_brush() const { return panel_brush_; }

    // Titelleiste des Fensters umfaerben.
    void apply_frame(HWND hwnd) const;
    // Alle Kindfenster passend thematisieren (rekursiv).
    void apply_controls(HWND parent) const;

    ~Theme();

private:
    void rebuild_brushes();

    bool dark_ = false;
    Palette pal_{};
    HBRUSH bg_brush_ = nullptr;
    HBRUSH panel_brush_ = nullptr;
};

// true, wenn Windows gerade auf dunkle App-Darstellung steht.
bool system_prefers_dark();

// Hilfsfunktion fuer WM_SETTINGCHANGE.
bool is_color_scheme_change(LPARAM lparam);

} // namespace ui
