// HID-Interface-Enumeration ueber SetupAPI.
//
// Fuer HID++ interessieren ausschliesslich die vendor-definierten Collections. Die
// eigentliche Maus-Collection (Usage-Page 0x01) traegt unter Windows die Kennung
// HID_DEVICE_SYSTEM_MOUSE und laesst sich nicht mit Schreibzugriff oeffnen -- die
// Vendor-Collections dagegen schon, ohne Adminrechte. Darauf beruht das ganze Projekt.
//
// Welche Vendor-Page benutzt wird, ist geraeteabhaengig (gemessen an dieser Hardware):
//   Maus G502 X PLUS am LIGHTSPEED-Empfaenger : 0xFF00, Usage 0x0001 / 0x0002
//   Tastatur G815                             : 0xFF43, Usage 0x0602 / 0x0604
// Die Usage-Nummern sind also nicht uebertragbar, die Reportlaengen schon. Deshalb wird
// nach Laenge klassifiziert statt nach Usage.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hidpp {

inline constexpr uint16_t kVendorLogitech = 0x046D;

// Vendor-definierte Usage-Pages beginnen bei 0xFF00.
inline constexpr uint16_t kUsagePageVendorFirst = 0xFF00;

// Reportlaengen inkl. fuehrendem Report-ID-Byte.
inline constexpr uint16_t kLenShort    = 7;    // Report-ID 0x10
inline constexpr uint16_t kLenLong     = 20;   // Report-ID 0x11
inline constexpr uint16_t kLenVeryLong = 64;   // Report-ID 0x12

struct HidInterface {
    std::wstring path;          // \?\hid#vid_046d&pid_c53a&mi_02&col02#...
    std::wstring parent_id;     // Instanz-ID des USB-Interface-Knotens (gruppiert Collections)
    std::wstring product;       // HidD_GetProductString, oft leer bei Vendor-Collections
    uint16_t vendor_id  = 0;
    uint16_t product_id = 0;
    uint16_t usage_page = 0;
    uint16_t usage      = 0;
    uint16_t input_len  = 0;    // inkl. fuehrendem Report-ID-Byte
    uint16_t output_len = 0;
};

// Listet alle vorhandenen HID-Interfaces. vid_filter == 0 bedeutet "alle".
std::vector<HidInterface> enumerate(uint16_t vid_filter = 0);

// Die Vendor-Collections eines USB-Interfaces. Nicht jedes Geraet hat alle drei:
// die G502 am Empfaenger bringt short+long, die G815 long+very long.
struct HidppEndpoint {
    HidInterface short_col;
    HidInterface long_col;
    HidInterface very_long_col;

    bool has_short() const { return !short_col.path.empty(); }
    bool has_very_long() const { return !very_long_col.path.empty(); }
};

// Gruppiert die Vendor-Collections nach parent_id und liefert je einen Endpunkt.
// Endpunkte ohne Long-Collection werden verworfen -- ohne die geht HID++ 2.0 nicht.
std::vector<HidppEndpoint> find_endpoints(uint16_t vid_filter = kVendorLogitech);

} // namespace hidpp
