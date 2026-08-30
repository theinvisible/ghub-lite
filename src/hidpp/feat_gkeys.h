// G-Tasten: Feature 0x8010 (GKEYS).
//
// Wichtig zum Verstaendnis: 0x8010 belegt nichts um. Es schaltet die G-Tasten in den
// Software-Modus, wodurch ihre Druckereignisse als HID++-Meldungen hereinkommen statt die
// Onboard-Belegung auszuloesen. Was dann passiert, entscheidet ghub-lite.
//
// Gemessen an der G815 (HID++ 4.2, GKeys v0 auf Feature-Index 10):
//   fn0 getCount              -> 5
//   fn1 getPhysicalLayout     -> 00 05
//   fn2 enableSoftwareControl -> Echo des gesetzten Wertes
//   Meldung: 11 FF <featIdx> 00 <Maske...>   (Event 0, swId 0)
#pragma once

#include "hidpp/hidpp_root.h"

#include <cstdint>
#include <string>

namespace hidpp {

struct GKeyCaps {
    bool    valid = false;
    uint8_t count = 0;          // G1..Gn
    uint8_t feature_index = 0;  // zum Erkennen der Meldungen im Strom
};

bool read_gkeys(Device& dev, GKeyCaps* out);

// Software-Modus schalten. Aus heisst: die Tastatur macht wieder ihr Onboard-Ding.
bool set_gkey_software_mode(Device& dev, bool on, std::wstring* error_out);

// Eine eingetroffene Meldung als G-Tasten-Zustand deuten.
// Rueckgabe false, wenn die Meldung nicht von diesem Feature stammt.
//
// Die Maske ist eine Bitmaske der gerade gedrueckten Tasten, Bit 0 = G1. Flanken bildet
// der Aufrufer durch Vergleich mit der vorigen Maske.
bool decode_gkey_event(const uint8_t* data, size_t len, uint8_t dev_index,
                       uint8_t feature_index, uint16_t* mask_out);

} // namespace hidpp
