// Signalrate: Feature 0x8060 (ReportRate) und 0x8061 (ExtendedAdjustableReportRate).
//
// Verifiziert auf der G502 X PLUS gegen 0x8060 v0: Bitmap 0x8B = Bits 0,1,3,7
// -> 1/2/4/8 ms -> 1000/500/250/125 Hz. Der 0x8061-Pfad (2k/4k/8k-Geraete) ist nach
// Protokoll umgesetzt, hier aber nicht gegenpruefbar.
#pragma once

#include "hidpp/hidpp_root.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hidpp {

struct RateCaps {
    bool     valid       = false;
    uint16_t via_feature = 0;      // 0x8060 oder 0x8061
    uint16_t current_hz  = 0;
    std::vector<uint16_t> rates_hz;   // aufsteigend sortiert

    bool supports(uint16_t hz) const;
    std::wstring describe() const;
};

// wireless == true, wenn das Geraet ueber einen Empfaenger haengt (relevant fuer 0x8061,
// das seine Ratenliste pro Verbindungsart fuehrt).
bool read_rate(Device& dev, bool wireless, RateCaps* out);

// Nur die aktive Rate neu lesen, ueber das Feature, das read_rate() gefunden hat. Bei
// Misserfolg bleibt caps unveraendert -- ein Timeout darf die Ratenliste nicht wegwischen.
bool read_rate_current(Device& dev, RateCaps* caps);
bool write_rate(Device& dev, const RateCaps& caps, uint16_t hz, std::wstring* error_out);

} // namespace hidpp
