// Signalraten-Auswahl: eine Reihe Radioknoepfe, deren Beschriftung aus den Faehigkeiten
// des Geraets kommt. Hier reichen Standard-Controls -- BS_AUTORADIOBUTTON mit dem Theme
// DarkMode_Explorer sieht dunkel ordentlich aus, anders als die Trackbar.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace ui {

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

    int first_id() const { return first_id_; }
    int last_id() const { return first_id_ + static_cast<int>(buttons_.size()) - 1; }

private:
    static constexpr size_t kMaxButtons = 8;
    std::vector<HWND> buttons_;
    std::vector<uint16_t> rates_;
    int first_id_ = 0;
};

} // namespace ui
