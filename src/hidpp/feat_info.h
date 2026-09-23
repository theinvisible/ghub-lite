// Geraeteinfos: Name (0x0005), Unit-ID (0x0003), Akku (0x1004/0x1000),
// Onboard-Modus (0x8100, nur lesend).
#pragma once

#include "hidpp/hidpp_root.h"

#include <cstdint>
#include <string>

namespace hidpp {

enum class OnboardMode : uint8_t {
    Unknown = 0,
    Onboard = 1,   // Geraet arbeitet aus seinem eigenen Profilspeicher
    Host    = 2,   // Software steuert; G HUB schaltet hierhin
};

enum class ChargeState : uint8_t {
    Discharging = 0,
    Charging    = 1,
    ChargingSlow = 2,
    Complete    = 3,
    Error       = 4,
};

struct DeviceInfo {
    std::wstring name;
    uint32_t unit_id = 0;        // stabiler Schluessel fuer die Einstellungsdatei
    // modelId aus 0x0003: bis zu drei Produkt-IDs je Verbindungsart (Funk, USB, ...).
    // Gemessen G502 X PLUS: 4099 (LIGHTSPEED), C095 (Kabel), 0000.
    uint16_t model_ids[3] = {};
    bool     has_battery = false;
    uint8_t  battery_percent = 0;
    ChargeState charge = ChargeState::Discharging;
    OnboardMode onboard = OnboardMode::Unknown;

    std::wstring unit_key() const;   // "C4F515BE"
};

// 0x0005: Name kommt in Haeppchen als ASCII.
std::wstring read_device_name(Device& dev);

bool read_device_info(Device& dev, DeviceInfo* out);

// 0x8100 fn1 setOnboardMode. Die einzige schreibende Stelle an 0x8100 -- und ein reiner
// Moduswechsel, kein Schreiben in den Profilspeicher.
//
// Hintergrund: im Onboard-Modus gehoert die Signalrate dem Profil im Geraet, und
// setReportRate wird mit HID++-Fehler 0x02 abgelehnt. Erst im Host-Modus laesst sie sich
// setzen. Liest den Modus danach zurueck und meldet nur Erfolg, wenn er wirklich steht.
bool set_onboard_mode(Device& dev, OnboardMode mode, std::wstring* error_out);

// 0x8100 fn2, einzeln -- fuer die Kontrolle nach einem Moduswechsel.
OnboardMode read_onboard_mode(Device& dev);

const wchar_t* onboard_mode_text(OnboardMode m);
const wchar_t* charge_state_text(ChargeState c);

} // namespace hidpp
