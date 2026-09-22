#include "hidpp/feat_rate.h"

#include <algorithm>
#include <cwchar>

namespace hidpp {
namespace {

// 0x8061 kennt keine Millisekunden, sondern Indizes in eine feste Ratentabelle.
constexpr uint16_t kExtRates[] = {125, 250, 500, 1000, 2000, 4000, 8000};
constexpr size_t   kExtRateCount = sizeof(kExtRates) / sizeof(kExtRates[0]);

bool read_rate_8060(Device& dev, RateCaps* out) {
    Reply list = dev.call(kFeatReportRate, 0);
    if (!list) return false;

    // Bitmap: Bit i gesetzt -> Intervall (i+1) ms wird unterstuetzt.
    const uint8_t bitmap = list.param(0);
    for (int i = 0; i < 8; ++i) {
        if (!(bitmap & (1 << i))) continue;
        out->rates_hz.push_back(static_cast<uint16_t>(1000 / (i + 1)));
    }

    out->via_feature = kFeatReportRate;
    read_rate_current(dev, out);
    out->valid = !out->rates_hz.empty();
    return out->valid;
}

bool read_rate_8061(Device& dev, bool wireless, RateCaps* out) {
    const uint8_t conn = wireless ? 1 : 0;
    Reply caps = dev.call(kFeatExtReportRate, 0, {conn});
    if (!caps) return false;

    // Bitfeld ueber die Ratentabelle oben; Bit n -> kExtRates[n].
    const uint16_t bits = caps.param_u16(0);
    for (size_t i = 0; i < kExtRateCount; ++i)
        if (bits & (1u << i)) out->rates_hz.push_back(kExtRates[i]);

    out->via_feature = kFeatExtReportRate;
    read_rate_current(dev, out);
    out->valid = !out->rates_hz.empty();
    return out->valid;
}

} // namespace

bool RateCaps::supports(uint16_t hz) const {
    return std::find(rates_hz.begin(), rates_hz.end(), hz) != rates_hz.end();
}

std::wstring RateCaps::describe() const {
    if (!valid) return L"keine einstellbare Signalrate";
    std::wstring s;
    wchar_t buf[24];
    for (size_t i = 0; i < rates_hz.size(); ++i) {
        swprintf(buf, 24, L"%u", rates_hz[i]);
        if (i) s += L", ";
        s += buf;
    }
    return s + L" Hz";
}

bool read_rate(Device& dev, bool wireless, RateCaps* out) {
    *out = RateCaps{};
    // 0x8061 hat Vorrang: Geraete, die beides melden, fuehren die feineren Raten dort.
    if (dev.has(kFeatExtReportRate) && read_rate_8061(dev, wireless, out)) return true;
    *out = RateCaps{};
    if (dev.has(kFeatReportRate)) return read_rate_8060(dev, out);
    return false;
}

bool read_rate_current(Device& dev, RateCaps* caps) {
    if (caps->via_feature == kFeatExtReportRate) {
        Reply cur = dev.call(kFeatExtReportRate, 1);
        if (!cur || cur.param(0) >= kExtRateCount) return false;
        caps->current_hz = kExtRates[cur.param(0)];
        return true;
    }
    if (caps->via_feature == kFeatReportRate) {
        // Antwort ist das Intervall in Millisekunden, nicht die Frequenz.
        Reply cur = dev.call(kFeatReportRate, 1);
        if (!cur || cur.param(0) == 0) return false;
        caps->current_hz = static_cast<uint16_t>(1000 / cur.param(0));
        return true;
    }
    return false;
}

bool write_rate(Device& dev, const RateCaps& caps, uint16_t hz, std::wstring* error_out) {
    if (!caps.valid) {
        if (error_out) *error_out = L"Geraet bietet keine einstellbare Signalrate";
        return false;
    }
    if (!caps.supports(hz)) {
        if (error_out) *error_out = L"Signalrate wird von diesem Geraet nicht angeboten";
        return false;
    }

    Reply r;
    if (caps.via_feature == kFeatExtReportRate) {
        size_t idx = kExtRateCount;
        for (size_t i = 0; i < kExtRateCount; ++i) if (kExtRates[i] == hz) idx = i;
        if (idx == kExtRateCount) {
            if (error_out) *error_out = L"Signalrate ist kein gueltiger 0x8061-Index";
            return false;
        }
        r = dev.call(kFeatExtReportRate, 2, {static_cast<uint8_t>(idx)});
    } else {
        const int ms = 1000 / hz;
        if (ms < 1 || ms > 8) {
            if (error_out) *error_out = L"Signalrate laesst sich mit 0x8060 nicht ausdruecken";
            return false;
        }
        r = dev.call(kFeatReportRate, 2, {static_cast<uint8_t>(ms)});
    }
    if (!r) {
        if (error_out) *error_out = describe(r);
        return false;
    }
    return true;
}

} // namespace hidpp
