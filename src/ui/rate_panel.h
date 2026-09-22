// Signalraten-Auswahl: eine Reihe Radioknoepfe, deren Beschriftung aus den Faehigkeiten
// des Geraets kommt. Standard-Controls mit dem Theme DarkMode_Explorer -- bis auf die
// Beschriftung: die zeichnet das Theme bei *aktivierten* Radioknoepfen in seiner eigenen,
// fast schwarzen Farbe und ignoriert WM_CTLCOLOR*. Deaktiviert nimmt es Grau, weshalb das
// nur bei verbundenem Geraet auffiel. Die Checkboxen sind davon nicht betroffen. Im Dark
// Mode zeichnet custom_draw() deshalb die Beschriftung selbst; den Kreis malt weiter das
// Theme.
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <vector>

namespace ui {

class Theme;

class RatePanel {
public:
    // Legt die maximal moegliche Anzahl Knoepfe an; nicht benutzte bleiben versteckt.
    void create(HWND parent, HINSTANCE inst, int first_id);

    // Uebernimmt die vom Geraet gemeldeten Raten und markiert die aktive.
    void set_rates(const std::vector<uint16_t>& rates_hz, uint16_t current_hz);

    // Ordnet die sichtbaren Knoepfe in einer Reihe an.
    void layout(int x, int y, int button_w, int button_h, int gap);

    void set_font(HFONT font);
    void enable(bool on);

    // 0, wenn die ID zu keinem sichtbaren Knopf gehoert.
    uint16_t rate_for_id(int id) const;

    // NM_CUSTOMDRAW eines Knopfes dieses Panels; Rueckgabe geht unveraendert an Windows.
    bool owns(HWND h) const;
    LRESULT custom_draw(const NMCUSTOMDRAW& cd, const Theme& theme) const;

    int first_id() const { return first_id_; }
    int last_id() const { return first_id_ + static_cast<int>(buttons_.size()) - 1; }

private:
    static constexpr size_t kMaxButtons = 8;
    std::vector<HWND> buttons_;
    std::vector<uint16_t> rates_;
    int first_id_ = 0;
};

} // namespace ui
