// Belegung der G-Tasten: pro Taste eine Zeile mit der aktuellen Kombination und zwei
// Knoepfen.
//
// Das Erfassen laeuft ueber einen WH_KEYBOARD_LL-Hook, der ausschliesslich waehrend der
// Aufnahme haengt und die Tasten dabei schluckt. Ohne Hook waeren Win-Kombinationen und
// Druck nicht erfassbar, und die aufgenommene Kombination wuerde nebenbei im System
// ausgeloest. Der Hook wird in jedem Ausstiegspfad wieder abgehaengt -- bleibt er haengen,
// haengt die Tastatur systemweit.
#pragma once

#include "app/keystroke.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// An das Elternfenster, wenn eine Aufnahme abgeschlossen ist:
//   wParam = Tastennummer 1..n, lParam = 0
constexpr UINT WM_GKEY_CAPTURED = WM_APP + 11;

class GKeyPanel {
public:
    static constexpr size_t kMaxKeys = 8;

    void create(HWND parent, HINSTANCE inst, int id_change, int id_clear);
    void set_key_count(uint8_t count);
    void set_binding(uint8_t gkey, const std::wstring& display);
    void set_enabled(bool on);
    void show(bool visible);

    void layout(int x, int y, int width, int row_h, int gap);
    int  height(int row_h, int gap) const;
    void set_font(HFONT font);

    // true, wenn die ID zu diesem Panel gehoert und behandelt wurde.
    bool on_command(int id);

    void cancel_capture();
    bool capturing() const { return capture_target_ != 0; }
    uint8_t capture_target() const { return capture_target_; }
    const app::Keystroke& captured() const { return captured_; }

    // Nur fuer den Hook.
    bool feed_key(uint16_t vk, bool down);

private:
    void begin_capture(uint8_t gkey);
    void refresh_capture_text();

    HWND parent_ = nullptr;
    int  id_change_ = 0;
    int  id_clear_ = 0;
    uint8_t count_ = 0;

    std::vector<HWND> label_;
    std::vector<HWND> value_;
    std::vector<HWND> change_;
    std::vector<HWND> clear_;
    std::vector<std::wstring> display_;

    uint8_t capture_target_ = 0;      // 0 = keine Aufnahme laeuft
    app::Keystroke pending_{};
    app::Keystroke captured_{};
};

} // namespace ui
