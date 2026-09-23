#include "hidpp/feat_buttons.h"

#include <algorithm>
#include <utility>

namespace hidpp {
namespace {

constexpr uint8_t kHidBack    = 4;
constexpr uint8_t kHidForward = 5;

// Nur vermessene Modelle. Jede Zeile stammt aus hidpp_dump --spy am echten Geraet.
constexpr ButtonLayout kLayouts[] = {
    {0x4099, 4, 6},   // G502 X PLUS, LIGHTSPEED
    {0xC095, 4, 6},   // G502 X PLUS, Kabel
};

// Legt code auf pos und gibt dem bisherigen Traeger von code den Code, der auf pos stand.
void place(std::vector<uint8_t>& map, uint8_t pos, uint8_t code) {
    if (pos == 0 || pos > map.size()) return;
    const auto it = std::find(map.begin(), map.end(), code);
    if (it != map.end()) std::swap(*it, map[pos - 1u]);
    else map[pos - 1u] = code;
}

} // namespace

const ButtonLayout* find_button_layout(const DeviceInfo& info) {
    for (const auto& l : kLayouts)
        for (uint16_t id : info.model_ids)
            if (id != 0 && id == l.model_id) return &l;
    return nullptr;
}

bool read_button_mapping(Device& dev, std::vector<uint8_t>* out) {
    Reply count = dev.call(kFeatMouseButtonSpy, 0);
    if (!count) return false;
    Reply map = dev.call(kFeatMouseButtonSpy, 3);
    if (!map) return false;

    const size_t n = std::min<size_t>(count.param(0), map.param_len());
    out->assign(map.params(), map.params() + n);
    return !out->empty();
}

bool write_button_mapping(Device& dev, const std::vector<uint8_t>& mapping, std::wstring* error_out) {
    // Ein leeres oder zu kurzes Mapping wuerde die restlichen Positionen mit 0 fuellen --
    // also Tasten abschalten. Nur vollstaendige Tabellen schreiben.
    if (mapping.empty() || mapping.size() > 16) {
        if (error_out) *error_out = L"ungültiges Tasten-Mapping";
        return false;
    }
    // Device::call nimmt nur eine initializer_list; die Laenge ist hier aber erst zur
    // Laufzeit bekannt -- deshalb direkt ueber den Kanal.
    auto fi = dev.feature(kFeatMouseButtonSpy);
    if (!fi || !dev.channel()) {
        if (error_out) *error_out = L"Gerät kennt 0x8110 nicht";
        return false;
    }
    Reply w = dev.channel()->call(dev.index(), fi->index, 4, mapping.data(), mapping.size());
    if (!w) {
        if (error_out) *error_out = describe(w);
        return false;
    }
    return true;
}

std::vector<uint8_t> host_button_mapping(std::vector<uint8_t> current, const ButtonLayout& layout) {
    place(current, layout.back, kHidBack);
    place(current, layout.forward, kHidForward);
    return current;
}

} // namespace hidpp
