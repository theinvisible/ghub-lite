#include "hidpp/feat_gkeys.h"

#include "hidpp/hidpp_device.h"

namespace hidpp {

bool read_gkeys(Device& dev, GKeyCaps* out) {
    *out = GKeyCaps{};

    auto fi = dev.feature(kFeatGKeys);
    if (!fi) return false;

    Reply count = dev.call(kFeatGKeys, 0);
    if (!count) return false;

    out->feature_index = fi->index;
    out->count = count.param(0);
    out->valid = out->count > 0;
    return out->valid;
}

bool set_gkey_software_mode(Device& dev, bool on, std::wstring* error_out) {
    Reply r = dev.call(kFeatGKeys, 2, {static_cast<uint8_t>(on ? 1 : 0)});
    if (!r) {
        if (error_out) *error_out = describe(r);
        return false;
    }
    return true;
}

bool decode_gkey_event(const uint8_t* data, size_t len, uint8_t dev_index,
                       uint8_t feature_index, uint16_t* mask_out) {
    // 11 | devIdx | featIdx | eventId<<4 | swId | Nutzdaten...
    // Unaufgeforderte Meldungen tragen swId 0; nur so lassen sie sich sicher von den
    // Antworten auf eigene Anfragen (swId 0x0A) unterscheiden.
    if (len < 6) return false;
    if (data[1] != dev_index) return false;
    if (data[2] != feature_index) return false;
    if (data[3] != 0x00) return false;   // Event 0, swId 0

    // Bitmaske der gerade gedrueckten Tasten, Bit 0 = G1. Die Tastatur schickt bei jeder
    // Aenderung den vollstaendigen Zustand, nicht einzelne Ereignisse -- Loslassen ist
    // also die Meldung mit geloeschtem Bit.
    *mask_out = static_cast<uint16_t>(data[4] | (data[5] << 8));
    return true;
}

} // namespace hidpp
