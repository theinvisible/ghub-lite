#include "hidpp/feat_dpi.h"

#include <algorithm>
#include <cwchar>

namespace hidpp {
namespace {

// Die DPI-Liste von 0x2201 fn1 ist eine Folge von u16, terminiert durch 0x0000.
// Ein Wert mit gesetzter Maske 0xE000 ist kein DPI-Wert, sondern eine Schrittweite:
// er steht zwischen Bereichsanfang und Bereichsende.
//
// Gemessen auf der G502 X PLUS:
//   00 | 0064 | E032 | 6400 | 0000   ->  100..25600 in 50er-Schritten
constexpr uint16_t kStepMask = 0xE000;

std::vector<DpiSegment> parse_dpi_list(const uint8_t* p, size_t len) {
    std::vector<uint16_t> values;
    for (size_t i = 0; i + 1 < len; i += 2) {
        const uint16_t v = static_cast<uint16_t>((p[i] << 8) | p[i + 1]);
        if (v == 0) break;
        values.push_back(v);
    }

    std::vector<DpiSegment> segs;
    for (size_t i = 0; i < values.size();) {
        if (i + 2 < values.size() && (values[i + 1] & kStepMask) == kStepMask) {
            DpiSegment s;
            s.from = values[i];
            s.step = static_cast<uint16_t>(values[i + 1] & ~kStepMask);
            s.to   = values[i + 2];
            if (s.step == 0) s.step = 1;
            if (s.to < s.from) std::swap(s.from, s.to);
            segs.push_back(s);
            i += 3;
        } else {
            segs.push_back(DpiSegment{values[i], values[i], 0});
            i += 1;
        }
    }
    return segs;
}

bool read_dpi_2201(Device& dev, DpiCaps* out) {
    Reply count = dev.call(kFeatAdjustableDpi, 0);
    if (!count) return false;
    out->sensor_count = count.param(0) ? count.param(0) : 1;

    Reply list = dev.call(kFeatAdjustableDpi, 1, {0});
    if (!list) return false;
    // param[0] spiegelt den Sensorindex, ab param[1] beginnt die u16-Folge.
    out->segments = parse_dpi_list(list.params() + 1, list.param_len() - 1);

    Reply cur = dev.call(kFeatAdjustableDpi, 2, {0});
    if (!cur) return false;
    out->current = cur.param_u16(1);
    out->dflt    = cur.param_u16(3);

    out->via_feature = kFeatAdjustableDpi;
    out->valid = !out->segments.empty();
    return out->valid;
}

// 0x2202: nach Protokoll umgesetzt, auf dieser Hardware nicht pruefbar (die G502 X PLUS
// meldet 0x2201). Liefert die Ranges ueber fn2 statt einer flachen Liste.
bool read_dpi_2202(Device& dev, DpiCaps* out) {
    Reply count = dev.call(kFeatExtAdjustableDpi, 0);
    if (!count) return false;
    out->sensor_count = count.param(0) ? count.param(0) : 1;

    // fn2 getSensorDpiRanges(sensorIdx, rangeIdx): mehrere Aufrufe, bis eine Antwort
    // keinen weiteren Bereich mehr liefert.
    for (uint8_t range_idx = 0; range_idx < 8; ++range_idx) {
        Reply r = dev.call(kFeatExtAdjustableDpi, 2, {0, range_idx});
        if (!r) break;
        // param[0]=sensorIdx, param[1]=rangeIdx, ab param[2] dieselbe u16-Kodierung.
        auto segs = parse_dpi_list(r.params() + 2, r.param_len() - 2);
        if (segs.empty()) break;
        out->segments.insert(out->segments.end(), segs.begin(), segs.end());
    }

    Reply cur = dev.call(kFeatExtAdjustableDpi, 5, {0});
    if (cur) {
        // param[0]=sensorIdx, dann dpiX, defaultDpiX, dpiY, defaultDpiY
        out->current = cur.param_u16(1);
        out->dflt    = cur.param_u16(3);
    }

    out->via_feature = kFeatExtAdjustableDpi;
    out->valid = !out->segments.empty();
    return out->valid;
}

} // namespace

uint16_t DpiCaps::min() const {
    uint16_t m = 0;
    for (const auto& s : segments) if (m == 0 || s.from < m) m = s.from;
    return m;
}

uint16_t DpiCaps::max() const {
    uint16_t m = 0;
    for (const auto& s : segments) if (s.to > m) m = s.to;
    return m;
}

uint16_t DpiCaps::snap(int wanted) const {
    if (segments.empty()) return 0;
    if (wanted < min()) return min();
    if (wanted > max()) return max();

    uint16_t best = min();
    int best_delta = 0x7FFFFFFF;
    for (const auto& s : segments) {
        uint16_t candidate;
        if (s.step == 0) {
            candidate = s.from;
        } else {
            const int clamped = std::clamp(wanted, static_cast<int>(s.from), static_cast<int>(s.to));
            const int steps = (clamped - s.from + s.step / 2) / s.step;
            candidate = static_cast<uint16_t>(std::min<int>(s.from + steps * s.step, s.to));
        }
        const int delta = std::abs(static_cast<int>(candidate) - wanted);
        if (delta < best_delta) { best_delta = delta; best = candidate; }
    }
    return best;
}

std::vector<uint16_t> DpiCaps::enumerate(size_t limit) const {
    std::vector<uint16_t> out;
    for (const auto& s : segments) {
        if (s.step == 0) {
            out.push_back(s.from);
            continue;
        }
        for (int v = s.from; v <= s.to && out.size() < limit; v += s.step)
            out.push_back(static_cast<uint16_t>(v));
        if (out.size() >= limit) break;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::wstring DpiCaps::describe() const {
    if (!valid) return L"kein DPI-Feature";
    std::wstring s;
    wchar_t buf[96];
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& g = segments[i];
        if (g.step == 0) swprintf(buf, 96, L"%u", g.from);
        else             swprintf(buf, 96, L"%u–%u (Schritt %u)", g.from, g.to, g.step);
        if (i) s += L", ";
        s += buf;
    }
    return s;
}

bool read_dpi(Device& dev, DpiCaps* out) {
    *out = DpiCaps{};
    if (dev.has(kFeatExtAdjustableDpi)) return read_dpi_2202(dev, out);
    if (dev.has(kFeatAdjustableDpi))    return read_dpi_2201(dev, out);
    return false;
}

bool write_dpi(Device& dev, const DpiCaps& caps, uint16_t dpi, std::wstring* error_out) {
    if (!caps.valid) {
        if (error_out) *error_out = L"Geraet bietet keine einstellbare Aufloesung";
        return false;
    }
    const uint16_t value = caps.snap(dpi);
    const uint8_t hi = static_cast<uint8_t>(value >> 8);
    const uint8_t lo = static_cast<uint8_t>(value & 0xFF);

    Reply r;
    if (caps.via_feature == kFeatExtAdjustableDpi) {
        // fn6 setSensorDpiParameters(sensor, dpiX, dpiY, lod); dpiY = dpiX, lod unveraendert.
        r = dev.call(kFeatExtAdjustableDpi, 6, {0, hi, lo, hi, lo, 0});
    } else {
        r = dev.call(kFeatAdjustableDpi, 3, {0, hi, lo});
    }
    if (!r) {
        if (error_out) *error_out = describe(r);
        return false;
    }
    return true;
}

} // namespace hidpp
