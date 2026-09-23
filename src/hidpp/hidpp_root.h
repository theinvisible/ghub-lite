// Feature-Aufloesung: IRoot (0x0000) und IFeatureSet (0x0001).
//
// Feature-Indizes sind geraeteabhaengig und duerfen niemals hartkodiert werden. Einzige
// Ausnahme ist IRoot selbst, das per Definition auf Index 0 liegt; ueber dessen
// getFeature() wird alles andere nachgeschlagen und danach gecacht.
#pragma once

#include "hidpp/hidpp_device.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace hidpp {

// Feature-IDs, die dieses Projekt benutzt.
enum FeatureId : uint16_t {
    kFeatRoot            = 0x0000,
    kFeatFeatureSet      = 0x0001,
    kFeatDeviceInfo      = 0x0003,
    kFeatDeviceName      = 0x0005,
    kFeatBatteryStatus   = 0x1000,
    kFeatBatteryVoltage  = 0x1001,
    kFeatUnifiedBattery  = 0x1004,
    kFeatAdjustableDpi   = 0x2201,
    kFeatExtAdjustableDpi = 0x2202,
    kFeatReportRate      = 0x8060,
    kFeatExtReportRate   = 0x8061,
    kFeatGKeys           = 0x8010,
    kFeatMKeys           = 0x8020,
    kFeatOnboardProfiles = 0x8100,
    kFeatMouseButtonSpy  = 0x8110,
};

const wchar_t* feature_name(uint16_t id);

struct FeatureInfo {
    uint16_t id      = 0;
    uint8_t  index   = 0;
    uint8_t  type    = 0;   // Bit 7 obsolet, Bit 6 hidden, Bit 5 engineering
    uint8_t  version = 0;

    bool obsolete()    const { return (type & 0x80) != 0; }
    bool hidden()      const { return (type & 0x40) != 0; }
    bool engineering() const { return (type & 0x20) != 0; }
};

// Ein adressierbares HID++-Geraet: Kanal plus Geraeteindex plus Feature-Cache.
class Device {
public:
    Device(Channel* channel, uint8_t index) : ch_(channel), index_(index) {}

    Channel* channel() const { return ch_; }
    uint8_t index() const { return index_; }

    // IRoot fn1: Ping. Liefert die Protokollversion, wenn das Geraet erreichbar ist.
    struct Ping { uint8_t major = 0; uint8_t minor = 0; };
    std::optional<Ping> ping(unsigned timeout_ms = 400);

    // Wie ping(), aber mit der Unterscheidung, die beim Empfaenger den Unterschied macht:
    // ein schlafendes Geraet ist etwas anderes als ein leerer Steckplatz.
    enum class SlotState {
        Awake,      // antwortet, HID++ 2.0 nutzbar
        Asleep,     // gepaart, aber gerade nicht erreichbar (Empfaenger meldet 0x09)
        Empty,      // nichts an diesem Index gepaart (0x08)
        NoAnswer,   // Zeitueberschreitung oder unerwartete Antwort
    };
    SlotState probe(unsigned timeout_ms = 300);

    // IRoot fn0: Feature-ID -> Index. Ergebnis wird gecacht, auch das negative.
    std::optional<FeatureInfo> feature(uint16_t feature_id);
    bool has(uint16_t feature_id) { return feature(feature_id).has_value(); }

    // IFeatureSet: vollstaendige Feature-Tabelle des Geraets, fuer die Probe.
    std::vector<FeatureInfo> enumerate_features();

    // Aufruf einer Feature-Funktion mit automatischer Index-Aufloesung.
    Reply call(uint16_t feature_id, uint8_t func_id,
               std::initializer_list<uint8_t> params = {}, unsigned timeout_ms = 600);

private:
    Channel* ch_ = nullptr;
    uint8_t index_ = 0;
    std::map<uint16_t, std::optional<FeatureInfo>> cache_;
};

} // namespace hidpp
