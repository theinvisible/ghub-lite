// Tastenkombination: erfassen, speichern, senden.
//
// In der INI steht die Kombination sprachneutral ("Ctrl+Shift+F13"), angezeigt wird sie
// deutsch ("Strg+Umschalt+F13"). Die INI soll auf einem anderen System mit anderer
// Sprache lesbar bleiben.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace app {

struct Keystroke {
    uint16_t vk    = 0;   // virtueller Tastencode der Haupttaste, 0 = nichts belegt
    bool     ctrl  = false;
    bool     alt   = false;
    bool     shift = false;
    bool     win   = false;

    bool empty() const { return vk == 0; }
    bool operator==(const Keystroke& o) const {
        return vk == o.vk && ctrl == o.ctrl && alt == o.alt && shift == o.shift && win == o.win;
    }
};

// Loest die Kombination aus: Modifier druecken, Taste druecken, alles in umgekehrter
// Reihenfolge wieder loesen. Nichts darf gedrueckt haengen bleiben.
bool send(const Keystroke& k);

// "Ctrl+Shift+F13" -- fuer die INI.
std::wstring to_storage(const Keystroke& k);
bool from_storage(const std::wstring& text, Keystroke* out);

// "Strg+Umschalt+F13" -- fuer die Anzeige.
std::wstring to_display(const Keystroke& k);

// Name einer einzelnen Taste, so wie Windows sie nennt (nutzt das Tastaturlayout).
std::wstring key_name(uint16_t vk);

// true fuer Strg/Alt/Umschalt/Windows in allen Varianten -- solche Tasten taugen nicht als
// Haupttaste einer Kombination.
bool is_modifier(uint16_t vk);

} // namespace app
