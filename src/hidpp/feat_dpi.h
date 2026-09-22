// DPI: Feature 0x2201 (AdjustableDpi) und 0x2202 (ExtendedAdjustableDpi).
//
// Verifiziert auf der G502 X PLUS gegen 0x2201 v2. Der 0x2202-Pfad ist nach Protokoll
// implementiert, hier aber mangels Geraet nicht gegengeprueft -- siehe Kommentar in
// feat_dpi.cpp.
#pragma once

#include "hidpp/hidpp_root.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hidpp {

// Ein Abschnitt der DPI-Liste. step == 0 bedeutet einen einzelnen festen Wert (to == from).
struct DpiSegment {
    uint16_t from = 0;
    uint16_t to   = 0;
    uint16_t step = 0;
};

struct DpiCaps {
    bool     valid        = false;
    uint16_t via_feature  = 0;     // 0x2201 oder 0x2202
    uint8_t  sensor_count = 0;
    uint16_t current      = 0;
    uint16_t dflt         = 0;
    std::vector<DpiSegment> segments;

    uint16_t min() const;
    uint16_t max() const;
    // Naechster gueltiger Wert zu einem Wunschwert (Rasterung auf die Schrittweite).
    uint16_t snap(int wanted) const;
    // Aufzaehlung aller Stufen -- nur benutzen, wenn die Anzahl handhabbar ist.
    std::vector<uint16_t> enumerate(size_t limit = 4096) const;
    std::wstring describe() const;
};

bool read_dpi(Device& dev, DpiCaps* out);

// Nur den aktiven Wert neu lesen, ueber das Feature, das read_dpi() gefunden hat. Bei
// Misserfolg bleibt caps unveraendert -- ein Timeout darf die bekannten Faehigkeiten nicht
// wegwischen.
bool read_dpi_current(Device& dev, DpiCaps* caps);
bool write_dpi(Device& dev, const DpiCaps& caps, uint16_t dpi, std::wstring* error_out);

} // namespace hidpp
