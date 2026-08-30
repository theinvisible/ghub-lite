// Hauptfenster.
//
// Aufbau: Geraeteauswahl, DPI (selbst gezeichneter Slider + Zahlenfeld + Schnellwahl),
// Signalrate (Radioknoepfe), zwei Optionen, Statuszeile.
//
// Zur DPI-Darstellung: HID++ 0x2201 kennt genau einen aktiven Aufloesungswert. Die fuenf
// Stufen, die G HUB zeigt, liegen im Onboard-Profil des Geraets, und das schreibt
// ghub-lite bewusst nicht. Die Schnellwahlknoepfe hier sind deshalb unsere eigenen Werte
// aus settings.ini, kein Geraetezustand.
#pragma once

#include "app/settings.h"
#include "hidpp/device_manager.h"
#include "ui/gkey_panel.h"
#include "ui/rate_panel.h"
#include "ui/theme.h"
#include "ui/tray.h"

#include <windows.h>

#include <memory>
#include <set>
#include <string>

namespace ui {

class MainWindow {
public:
    // start_hidden: Aufruf mit --tray beim Anmelden.
    bool create(HINSTANCE inst, bool start_hidden);
    HWND hwnd() const { return hwnd_; }

    static bool register_class(HINSTANCE inst);

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    void build_controls();
    void layout();
    void resize_to_design(const RECT* suggested);
    void rebuild_fonts();
    void apply_fonts();
    void apply_theme();

    void on_snapshot();
    void prime_desired(const hidpp::Snapshot& snap);
    void sync_host_mode(const hidpp::Snapshot& snap);
    void update_device_list(const hidpp::Snapshot& snap);
    void update_device_view(const hidpp::Snapshot& snap);
    void set_status(const std::wstring& text, bool error);

    const hidpp::DeviceState* current() const;
    void commit_dpi(int value);
    void commit_rate(uint16_t hz);
    void set_dpi_fields(int value);
    int  read_dpi_field() const;

    // Entwurfshoehe des Fensters -- haengt davon ab, welches Panel gerade sichtbar ist.
    int  design_height() const;
    void show_mouse_panel(bool on);
    void show_keyboard_panel(bool on);
    void load_gkey_bindings();
    void on_gkey_pressed(uint8_t gkey, uint32_t unit_id);
    void on_gkey_captured(uint8_t gkey);

    void show_tray_menu();
    void toggle_window(bool show);
    void paint(HDC dc);

    int dip(int v) const { return MulDiv(v, dpi_, 96); }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;

    HFONT font_ = nullptr;
    HFONT font_head_ = nullptr;

    HWND combo_ = nullptr;
    HWND detail_ = nullptr;
    HWND head_dpi_ = nullptr;
    HWND slider_ = nullptr;
    HWND edit_ = nullptr;
    HWND edit_unit_ = nullptr;
    HWND range_ = nullptr;
    HWND head_rate_ = nullptr;
    HWND head_gkeys_ = nullptr;
    HWND gkey_hint_ = nullptr;
    HWND chk_gkeys_ = nullptr;
    HWND chk_autostart_ = nullptr;
    HWND chk_tray_ = nullptr;
    HWND status_ = nullptr;
    HWND preset_[8] = {};

    RatePanel rates_;
    GKeyPanel gkeys_;
    Theme theme_;
    Tray tray_;
    app::Settings settings_;
    hidpp::Manager manager_;

    std::shared_ptr<const hidpp::Snapshot> snap_;
    std::wstring current_key_;
    std::set<std::wstring> primed_;   // Geraete, deren gespeicherte Werte schon uebergeben sind
    std::wstring last_manager_status_;   // zuletzt uebernommene Meldung des Managers
    bool updating_ = false;     // unterdrueckt Rueckkopplung beim Setzen der Controls
    bool laying_out_ = false;   // verhindert Rekursion zwischen layout() und WM_SIZE
    bool status_error_ = false;
    bool quitting_ = false;
    HICON icon_ = nullptr;
};

} // namespace ui
