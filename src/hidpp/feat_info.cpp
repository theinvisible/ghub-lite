#include "hidpp/feat_info.h"

#include <cwchar>

namespace hidpp {

std::wstring read_device_name(Device& dev) {
    Reply count = dev.call(kFeatDeviceName, 0);
    if (!count) return {};
    const uint8_t total = count.param(0);

    std::wstring name;
    // Pro Aufruf kommen bis zu 16 Zeichen; der Offset zaehlt in Zeichen, nicht in Reports.
    for (uint8_t off = 0; off < total && name.size() < 64;) {
        Reply chunk = dev.call(kFeatDeviceName, 1, {off});
        if (!chunk) break;
        const size_t before = name.size();
        for (size_t i = 0; i < chunk.param_len() && name.size() < total; ++i) {
            const uint8_t c = chunk.param(i);
            if (c == 0) break;
            name += static_cast<wchar_t>(c);
        }
        if (name.size() == before) break;   // kein Fortschritt: Endlosschleife vermeiden
        off = static_cast<uint8_t>(name.size());
    }
    return name;
}

bool read_device_info(Device& dev, DeviceInfo* out) {
    *out = DeviceInfo{};
    out->name = read_device_name(dev);

    // 0x0003 fn0: entityCnt(1), unitId(4), transport(2), modelId(6), ...
    // Gemessen G502 X PLUS: 0C C4 F5 15 BE 00 0C ... -> unitId C4F515BE
    if (Reply r = dev.call(kFeatDeviceInfo, 0)) {
        out->unit_id = (static_cast<uint32_t>(r.param(1)) << 24) |
                       (static_cast<uint32_t>(r.param(2)) << 16) |
                       (static_cast<uint32_t>(r.param(3)) << 8) |
                        static_cast<uint32_t>(r.param(4));
    }

    // 0x1004 fn1: stateOfCharge(1), batteryLevel(1), chargingStatus(1), externalPower(1)
    if (dev.has(kFeatUnifiedBattery)) {
        if (Reply r = dev.call(kFeatUnifiedBattery, 1)) {
            out->has_battery = true;
            out->battery_percent = r.param(0);
            const uint8_t cs = r.param(2);
            out->charge = cs <= static_cast<uint8_t>(ChargeState::Error)
                              ? static_cast<ChargeState>(cs) : ChargeState::Discharging;
        }
    } else if (dev.has(kFeatBatteryStatus)) {
        // 0x1000 fn0: batteryDischargeLevel(1), batteryDischargeNextLevel(1), status(1)
        if (Reply r = dev.call(kFeatBatteryStatus, 0)) {
            out->has_battery = true;
            out->battery_percent = r.param(0);
            out->charge = r.param(2) == 0 ? ChargeState::Discharging : ChargeState::Charging;
        }
    }

    // 0x8100 fn2 getOnboardMode -- ausschliesslich lesend. fn1 waere setOnboardMode und
    // bleibt bewusst unangetastet, solange das Profil-Schreiben nicht implementiert ist.
    if (dev.has(kFeatOnboardProfiles)) {
        if (Reply r = dev.call(kFeatOnboardProfiles, 2)) {
            const uint8_t m = r.param(0);
            out->onboard = (m == 1 || m == 2) ? static_cast<OnboardMode>(m) : OnboardMode::Unknown;
        }
    }

    return !out->name.empty() || out->unit_id != 0;
}

OnboardMode read_onboard_mode(Device& dev) {
    if (!dev.has(kFeatOnboardProfiles)) return OnboardMode::Unknown;
    Reply r = dev.call(kFeatOnboardProfiles, 2);
    if (!r) return OnboardMode::Unknown;
    const uint8_t m = r.param(0);
    return (m == 1 || m == 2) ? static_cast<OnboardMode>(m) : OnboardMode::Unknown;
}

bool set_onboard_mode(Device& dev, OnboardMode mode, std::wstring* error_out) {
    if (!dev.has(kFeatOnboardProfiles)) {
        if (error_out) *error_out = L"Gerät kennt keine Onboard-Profile";
        return false;
    }
    if (mode != OnboardMode::Onboard && mode != OnboardMode::Host) {
        if (error_out) *error_out = L"ungültiger Modus";
        return false;
    }

    Reply r = dev.call(kFeatOnboardProfiles, 1, {static_cast<uint8_t>(mode)});
    if (!r) {
        if (error_out) *error_out = describe(r);
        return false;
    }

    // Zurücklesen statt der Antwort zu glauben: fn1 quittiert auch dann, wenn das Geraet
    // den Wunsch anschliessend ignoriert.
    if (read_onboard_mode(dev) != mode) {
        if (error_out) *error_out = L"Gerät hat den Modus nicht übernommen";
        return false;
    }
    return true;
}

std::wstring DeviceInfo::unit_key() const {
    wchar_t buf[16];
    swprintf(buf, 16, L"%08X", unit_id);
    return buf;
}

const wchar_t* onboard_mode_text(OnboardMode m) {
    switch (m) {
        case OnboardMode::Onboard: return L"Onboard-Modus";
        case OnboardMode::Host:    return L"Host-Modus";
        default:                   return L"Modus unbekannt";
    }
}

const wchar_t* charge_state_text(ChargeState c) {
    switch (c) {
        case ChargeState::Charging:
        case ChargeState::ChargingSlow: return L"lädt";
        case ChargeState::Complete:     return L"voll";
        case ChargeState::Error:        return L"Ladefehler";
        default:                        return L"Akku";
    }
}

} // namespace hidpp
