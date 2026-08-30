#include "hidpp/hidpp_root.h"

namespace hidpp {

const wchar_t* feature_name(uint16_t id) {
    switch (id) {
        case 0x0000: return L"IRoot";
        case 0x0001: return L"IFeatureSet";
        case 0x0002: return L"IFirmwareInfo";
        case 0x0003: return L"DeviceInformation";
        case 0x0005: return L"DeviceNameType";
        case 0x0007: return L"DeviceFriendlyName";
        case 0x0008: return L"KeepAlive";
        case 0x0020: return L"ConfigChange";
        case 0x0021: return L"Uniqueid32Bytes";
        case 0x00C2: return L"DfuControlSigned";
        case 0x1000: return L"BatteryStatus";
        case 0x1001: return L"BatteryVoltage";
        case 0x1004: return L"UnifiedBattery";
        case 0x1300: return L"Led/RgbControl";
        case 0x1802: return L"DeviceReset";
        case 0x1814: return L"ChangeHost";
        case 0x1815: return L"HostsInfo";
        case 0x1B04: return L"ReprogControlsV4";
        case 0x1E00: return L"EnableHiddenFeatures";
        case 0x1EB0: return L"TdeAccessToRawMemory";
        case 0x1F03: return L"EquadDjDebugInfo";
        case 0x2100: return L"VerticalScrolling";
        case 0x2110: return L"SmartShift";
        case 0x2111: return L"SmartShiftEnhanced";
        case 0x2121: return L"HiResWheel";
        case 0x2130: return L"RatchetWheel";
        case 0x2201: return L"AdjustableDpi";
        case 0x2202: return L"ExtendedAdjustableDpi";
        case 0x2205: return L"PointerMotionScaling";
        case 0x2250: return L"AnalysisMode";
        case 0x8010: return L"GKeys";
        case 0x8020: return L"MKeys";
        case 0x8030: return L"MR (Makroaufnahme)";
        case 0x8040: return L"BrightnessControl";
        case 0x8060: return L"ReportRate";
        case 0x8061: return L"ExtendedAdjustableReportRate";
        case 0x8070: return L"ColorLedEffects";
        case 0x8071: return L"RgbEffects";
        case 0x8080: return L"PerKeyLighting";
        case 0x8090: return L"ModeStatus";
        case 0x8100: return L"OnboardProfiles";
        case 0x8110: return L"MouseButtonSpy";
        case 0x8123: return L"ForceFeedback";
        default:     return L"(unbekannt)";
    }
}

std::optional<Device::Ping> Device::ping(unsigned timeout_ms) {
    if (!ch_) return std::nullopt;
    // IRoot fn1 (getProtocolVersion): die letzten Nutzdaten werden unveraendert
    // zurueckgespiegelt und dienen als zusaetzliche Plausibilitaetspruefung.
    constexpr uint8_t kEcho = 0x5A;
    const uint8_t params[3] = {0, 0, kEcho};
    Reply r = ch_->call(index_, kRootIndex, 1, params, 3, timeout_ms);
    if (!r) return std::nullopt;
    if (r.param(2) != kEcho) return std::nullopt;
    return Ping{r.param(0), r.param(1)};
}

Device::SlotState Device::probe(unsigned timeout_ms) {
    if (!ch_) return SlotState::NoAnswer;
    constexpr uint8_t kEcho = 0x5A;
    const uint8_t params[3] = {0, 0, kEcho};
    Reply r = ch_->call(index_, kRootIndex, 1, params, 3, timeout_ms);

    if (r && r.param(2) == kEcho) return SlotState::Awake;

    if (r.status == Status::DeviceError && r.legacy_error) {
        // Gemessen am LIGHTSPEED-Empfaenger:
        //   0x09 Ressourcenfehler -> Maus gepaart, aber schlafend
        //   0x08 unbekanntes Geraet -> Steckplatz leer
        //   0x01 unbekannte Unterfunktion -> der Empfaenger selbst auf Index 0xFF
        switch (r.error_code) {
            case 0x09: return SlotState::Asleep;
            case 0x08: return SlotState::Empty;
            case 0x01: return SlotState::Empty;
            default:   return SlotState::NoAnswer;
        }
    }
    if (r.status == Status::DeviceError) {
        // IRoot fn1 gibt es auf jedem echten HID++-2.0-Geraet. Eine 2.0-Fehlerantwort
        // heisst deshalb: an diesem Index sitzt keines. Die direkt per USB angeschlossene
        // G815 quittiert die Funk-Steckplaetze 1..6 genau so mit 0x0A.
        switch (r.error_code) {
            case 0x08: return SlotState::Asleep;    // belegt -- spaeter nochmal versuchen
            case 0x06:                              // ungueltiger Feature-Index
            case 0x07:                              // ungueltige Funktions-ID
            case 0x0A: return SlotState::Empty;     // nicht unterstuetzt
            default:   return SlotState::NoAnswer;
        }
    }
    return SlotState::NoAnswer;
}

std::optional<FeatureInfo> Device::feature(uint16_t feature_id) {
    if (auto it = cache_.find(feature_id); it != cache_.end()) return it->second;

    if (feature_id == kFeatRoot) {
        const FeatureInfo root{kFeatRoot, kRootIndex, 0, 0};
        cache_[feature_id] = root;
        return root;
    }
    if (!ch_) return std::nullopt;

    const uint8_t params[2] = {static_cast<uint8_t>(feature_id >> 8),
                               static_cast<uint8_t>(feature_id & 0xFF)};
    Reply r = ch_->call(index_, kRootIndex, 0, params, 2);

    // Nur eine *Antwort* darf ins Gedaechtnis. Eine Zeitueberschreitung oder ein
    // Geraetefehler heisst nicht "Feature gibt es nicht", sondern "gerade nicht zu
    // erfahren" -- ein gerade aufwachendes Funkgeraet laesst schon mal eine Abfrage ins
    // Leere laufen. Wer das als Absage merkt, haelt die Maus fuer den Rest der Laufzeit
    // faelschlich fuer ein Geraet ohne DPI.
    if (!r) return std::nullopt;

    // Index 0 als Antwort heisst "Feature nicht vorhanden" -- Index 0 ist IRoot selbst.
    std::optional<FeatureInfo> result;
    if (r.param(0) != 0)
        result = FeatureInfo{feature_id, r.param(0), r.param(1), r.param(2)};

    cache_[feature_id] = result;
    return result;
}

std::vector<FeatureInfo> Device::enumerate_features() {
    std::vector<FeatureInfo> out;
    auto fs = feature(kFeatFeatureSet);
    if (!fs || !ch_) return out;

    Reply count_reply = ch_->call(index_, fs->index, 0, nullptr, 0);
    if (!count_reply) return out;
    const uint8_t count = count_reply.param(0);   // ohne IRoot

    out.push_back(FeatureInfo{kFeatRoot, kRootIndex, 0, 0});
    for (uint8_t i = 1; i <= count; ++i) {
        const uint8_t params[1] = {i};
        Reply r = ch_->call(index_, fs->index, 1, params, 1);
        if (!r) continue;
        FeatureInfo fi;
        fi.id      = r.param_u16(0);
        fi.index   = i;
        fi.type    = r.param(2);
        fi.version = r.param(3);
        out.push_back(fi);
        cache_[fi.id] = fi;   // spart spaetere getFeature-Roundtrips
    }
    return out;
}

Reply Device::call(uint16_t feature_id, uint8_t func_id,
                   std::initializer_list<uint8_t> params, unsigned timeout_ms) {
    Reply r;
    auto fi = feature(feature_id);
    if (!fi || !ch_) {
        r.status = Status::DeviceError;
        r.error_code = 0x06;   // ungueltiger Feature-Index
        return r;
    }
    return ch_->call(index_, fi->index, func_id, params.begin(), params.size(), timeout_ms);
}

} // namespace hidpp
