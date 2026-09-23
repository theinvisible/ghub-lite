// Tastenzuordnung im Host-Modus: Feature 0x8110 (MouseButtonSpy).
//
// Im Onboard-Modus ordnet das Profil im Geraet jeder physischen Taste ihre Funktion zu. Im
// Host-Modus gilt stattdessen das Mapping von 0x8110, und das ist nach dem Aufwachen die
// Identitaet: physische Position n geht als HID-Taste n hinaus. Windows wertet bei Maeusen
// nur die HID-Tasten 1..5 aus -- sitzt eine Daumentaste auf Position 6 oder hoeher, ist sie
// im Host-Modus tot. Gemessen an der G502 X PLUS: Vorwaerts auf Position 6.
//
// Belegung laut cvuchener/hidpp (IMouseButtonSpy), an der G502 X PLUS bestaetigt:
//   fn0 getMouseButtonCount   -> Anzahl (G502 X PLUS: 11)
//   fn1 startMouseButtonSpy / fn2 stopMouseButtonSpy
//   fn3 getMouseButtonMapping -> ein Byte je Position, 0 = fuer HID abgeschaltet, 1..16
//   fn4 setMouseButtonMapping -- wird laut invisible-ptt nur im Host-Modus beachtet und
//                                geht beim Einschlafen verloren
//   Meldung (Event 0): 11 <idx> <feat> 00 <Zustand BE16>, Bit n = Position n+1
#pragma once

#include "hidpp/feat_info.h"
#include "hidpp/hidpp_root.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hidpp {

// Wo die Daumentasten eines Modells physisch sitzen (1-basierte Positionen in 0x8110).
struct ButtonLayout {
    uint16_t model_id;    // eine der modelIds aus 0x0003
    uint8_t  back;        // soll HID-Taste 4 werden
    uint8_t  forward;     // soll HID-Taste 5 werden
};

// nullptr, wenn fuer dieses Modell nichts vermessen ist -- dann bleibt das Mapping unberuehrt.
const ButtonLayout* find_button_layout(const DeviceInfo& info);

// fn0 + fn3. Liefert genau so viele Eintraege, wie das Geraet Tasten meldet.
bool read_button_mapping(Device& dev, std::vector<uint8_t>* out);
bool write_button_mapping(Device& dev, const std::vector<uint8_t>& mapping, std::wstring* error_out);

// Das Mapping, das im Host-Modus gelten soll: ausgehend vom aktuellen werden Zurueck und
// Vorwaerts auf HID 4 und 5 gelegt. Wer diese Codes vorher hatte, bekommt im Tausch den
// frei werdenden -- so geht keine Taste verloren, sie landet hoechstens auf einer HID-Nummer,
// die Windows ohnehin ignoriert.
std::vector<uint8_t> host_button_mapping(std::vector<uint8_t> current, const ButtonLayout& layout);

} // namespace hidpp
