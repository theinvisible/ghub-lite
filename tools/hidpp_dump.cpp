// hidpp_dump -- Konsolen-Probe fuer den HID++-Unterbau.
//
// Zweck: die Referenzdaten des tatsaechlich angeschlossenen Geraets erheben, statt gegen
// Protokollgedaechtnis zu implementieren. Ausgabe gehoert nach docs/devlog/.
//
// Die Probe ruft ausschliesslich Funktionen auf, die nach Protokoll Getter sind. Setzende
// Funktionen laufen nur ueber die expliziten Schalter --set-dpi / --set-rate.

#include "hidpp/device_manager.h"
#include "hidpp/hid_enum.h"
#include "hidpp/hidpp_device.h"
#include "hidpp/hidpp_root.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

using namespace hidpp;

namespace {

struct Options {
    bool extended = false;    // auch die weniger sicher dokumentierten Getter abklopfen
    int  set_dpi  = 0;
    int  set_rate = 0;        // in Hz
    int  only_slot = -1;      // nur diesen Geraeteindex behandeln
};

void warn_about_ghub() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int count = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsnicmp(pe.szExeFile, L"lghub", 5) == 0) ++count;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (count > 0) {
        wprintf(L"WARNUNG: %d laufende lghub*-Prozesse gefunden.\n", count);
        wprintf(L"         G HUB pollt dieselben Geraete und faelscht das Bild. Fuer belastbare\n");
        wprintf(L"         Ergebnisse G HUB vorher komplett beenden (auch das Tray-Icon).\n\n");
    }
}

void print_reply(const wchar_t* label, const Reply& r) {
    if (r) {
        wprintf(L"    %-34s %s\n", label, hex_dump(r.raw.data(), r.raw_len).c_str());
    } else {
        wprintf(L"    %-34s -- %s\n", label, describe(r).c_str());
    }
}

void dump_features(Device& dev) {
    auto feats = dev.enumerate_features();
    if (feats.empty()) {
        wprintf(L"    (Feature-Tabelle nicht lesbar)\n");
        return;
    }
    wprintf(L"    Features (%zu):\n", feats.size());
    for (const auto& f : feats) {
        wprintf(L"      idx %3u  0x%04X  v%u  %-30s%s%s%s\n",
                f.index, f.id, f.version, feature_name(f.id),
                f.obsolete()    ? L" [obsolet]" : L"",
                f.hidden()      ? L" [versteckt]" : L"",
                f.engineering() ? L" [engineering]" : L"");
    }
}

void dump_dpi(Device& dev, const Options& opt) {
    if (dev.has(kFeatAdjustableDpi)) {
        wprintf(L"    -- 0x2201 AdjustableDpi --\n");
        print_reply(L"fn0 getSensorCount",      dev.call(kFeatAdjustableDpi, 0));
        print_reply(L"fn1 getSensorDpiList(0)", dev.call(kFeatAdjustableDpi, 1, {0}));
        print_reply(L"fn2 getSensorDpi(0)",     dev.call(kFeatAdjustableDpi, 2, {0}));
    }
    if (dev.has(kFeatExtAdjustableDpi)) {
        wprintf(L"    -- 0x2202 ExtendedAdjustableDpi --\n");
        print_reply(L"fn0 getSensorCount",           dev.call(kFeatExtAdjustableDpi, 0));
        print_reply(L"fn1 getSensorCapabilities(0)", dev.call(kFeatExtAdjustableDpi, 1, {0}));
        print_reply(L"fn2 getSensorDpiRanges(0,0)",  dev.call(kFeatExtAdjustableDpi, 2, {0, 0}));
        if (opt.extended) {
            print_reply(L"fn3 getSensorDpiList(0)",      dev.call(kFeatExtAdjustableDpi, 3, {0}));
            print_reply(L"fn4 getSensorLodList(0)",      dev.call(kFeatExtAdjustableDpi, 4, {0}));
            print_reply(L"fn5 getSensorDpiParameters(0)", dev.call(kFeatExtAdjustableDpi, 5, {0}));
        }
    }
    if (!dev.has(kFeatAdjustableDpi) && !dev.has(kFeatExtAdjustableDpi))
        wprintf(L"    (kein DPI-Feature)\n");
}

void dump_rate(Device& dev, const Options& opt) {
    if (dev.has(kFeatReportRate)) {
        wprintf(L"    -- 0x8060 ReportRate --\n");
        print_reply(L"fn0 getReportRateList", dev.call(kFeatReportRate, 0));
        print_reply(L"fn1 getReportRate",     dev.call(kFeatReportRate, 1));
    }
    if (dev.has(kFeatExtReportRate)) {
        wprintf(L"    -- 0x8061 ExtendedAdjustableReportRate --\n");
        print_reply(L"fn0 getDeviceCapabilities(wired=0)",    dev.call(kFeatExtReportRate, 0, {0}));
        print_reply(L"fn0 getDeviceCapabilities(wireless=1)", dev.call(kFeatExtReportRate, 0, {1}));
        if (opt.extended)
            print_reply(L"fn1 getActualReportRate", dev.call(kFeatExtReportRate, 1));
    }
    if (!dev.has(kFeatReportRate) && !dev.has(kFeatExtReportRate))
        wprintf(L"    (kein Report-Rate-Feature)\n");
}

void dump_gkeys(Device& dev) {
    if (!dev.has(kFeatGKeys)) return;
    wprintf(L"    -- 0x8010 GKeys --\n");
    // Nur Getter. fn2 waere enableSoftwareControl und laeuft ausschliesslich ueber --listen.
    print_reply(L"fn0 getCount",           dev.call(kFeatGKeys, 0));
    print_reply(L"fn1 getPhysicalLayout",  dev.call(kFeatGKeys, 1));
}

void dump_misc(Device& dev, const Options& opt) {
    if (dev.has(kFeatDeviceInfo))
        print_reply(L"0x0003 fn0 getDeviceInfo", dev.call(kFeatDeviceInfo, 0));
    if (dev.has(kFeatUnifiedBattery)) {
        print_reply(L"0x1004 fn0 getCapabilities", dev.call(kFeatUnifiedBattery, 0));
        print_reply(L"0x1004 fn1 getStatus",       dev.call(kFeatUnifiedBattery, 1));
    }
    if (dev.has(kFeatBatteryStatus))
        print_reply(L"0x1000 fn0 getBatteryLevelStatus", dev.call(kFeatBatteryStatus, 0));
    if (dev.has(kFeatOnboardProfiles)) {
        // Nur Getter: fn1 waere setOnboardMode, fn3 setCurrentProfile -- beide ausgelassen.
        print_reply(L"0x8100 fn0 getOnboardProfilesInfo", dev.call(kFeatOnboardProfiles, 0));
        print_reply(L"0x8100 fn2 getOnboardMode",         dev.call(kFeatOnboardProfiles, 2));
        if (opt.extended)
            print_reply(L"0x8100 fn4 getCurrentProfile", dev.call(kFeatOnboardProfiles, 4));
    }
    if (dev.has(kFeatMouseButtonSpy)) {
        // Nur Getter. Belegung laut cvuchener/hidpp (IMouseButtonSpy): fn0 Anzahl, fn3 Mapping
        // (ein Byte je Taste, 0 = fuer HID abgeschaltet, 1..16 = HID-Taste). fn1/fn2 starten
        // und stoppen die Meldungen, fn4 setzt das Mapping -- alle ausgelassen.
        print_reply(L"0x8110 fn0 getMouseButtonCount",   dev.call(kFeatMouseButtonSpy, 0));
        print_reply(L"0x8110 fn3 getMouseButtonMapping", dev.call(kFeatMouseButtonSpy, 3));
    }
}

void apply_sets(Device& dev, const Options& opt) {
    if (opt.set_dpi > 0) {
        const uint8_t hi = static_cast<uint8_t>((opt.set_dpi >> 8) & 0xFF);
        const uint8_t lo = static_cast<uint8_t>(opt.set_dpi & 0xFF);
        if (dev.has(kFeatExtAdjustableDpi)) {
            // 0x2202 fn6 setSensorDpiParameters(sensor, dpiX, dpiY, lod) -- dpiY 0 = wie X.
            print_reply(L"SET 0x2202 fn6 setSensorDpiParameters",
                        dev.call(kFeatExtAdjustableDpi, 6, {0, hi, lo, 0, 0, 0}));
        } else if (dev.has(kFeatAdjustableDpi)) {
            print_reply(L"SET 0x2201 fn3 setSensorDpi",
                        dev.call(kFeatAdjustableDpi, 3, {0, hi, lo}));
        } else {
            wprintf(L"    --set-dpi: Geraet bietet kein DPI-Feature\n");
        }
    }

    if (opt.set_rate > 0) {
        if (dev.has(kFeatReportRate)) {
            const int ms = 1000 / opt.set_rate;
            if (ms < 1 || ms > 8) {
                wprintf(L"    --set-rate: %d Hz laesst sich mit 0x8060 nicht ausdruecken\n", opt.set_rate);
            } else {
                print_reply(L"SET 0x8060 fn2 setReportRate",
                            dev.call(kFeatReportRate, 2, {static_cast<uint8_t>(ms)}));
            }
        } else if (dev.has(kFeatExtReportRate)) {
            // 0x8061: Ratenindex, 0=8ms ... 3=1ms(1000Hz), 4=500us, 5=250us, 6=125us.
            int idx = -1;
            switch (opt.set_rate) {
                case 125:  idx = 0; break;
                case 250:  idx = 1; break;
                case 500:  idx = 2; break;
                case 1000: idx = 3; break;
                case 2000: idx = 4; break;
                case 4000: idx = 5; break;
                case 8000: idx = 6; break;
                default: break;
            }
            if (idx < 0) wprintf(L"    --set-rate: %d Hz ist kein 0x8061-Ratenindex\n", opt.set_rate);
            else print_reply(L"SET 0x8061 fn2 setReportRate",
                             dev.call(kFeatExtReportRate, 2, {static_cast<uint8_t>(idx)}));
        } else {
            wprintf(L"    --set-rate: Geraet bietet kein Report-Rate-Feature\n");
        }
    }

    if (opt.set_dpi > 0 || opt.set_rate > 0) {
        wprintf(L"    -- Rueckgelesen --\n");
        dump_dpi(dev, opt);
        dump_rate(dev, opt);
    }
}

void probe_endpoint(const HidppEndpoint& ep, const Options& opt) {
    wprintf(L"\n============================================================\n");
    wprintf(L"Endpunkt VID %04X PID %04X\n", ep.long_col.vendor_id, ep.long_col.product_id);
    wprintf(L"  Elternknoten : %s\n", ep.long_col.parent_id.c_str());
    wprintf(L"  long      : UP=%04X U=%04X in=%u out=%u  %s\n",
            ep.long_col.usage_page, ep.long_col.usage,
            ep.long_col.input_len, ep.long_col.output_len, ep.long_col.path.c_str());
    if (ep.has_short())
        wprintf(L"  short     : UP=%04X U=%04X in=%u out=%u\n",
                ep.short_col.usage_page, ep.short_col.usage,
                ep.short_col.input_len, ep.short_col.output_len);
    else
        wprintf(L"  short     : (keine)\n");
    if (ep.has_very_long())
        wprintf(L"  very long : UP=%04X U=%04X in=%u out=%u\n",
                ep.very_long_col.usage_page, ep.very_long_col.usage,
                ep.very_long_col.input_len, ep.very_long_col.output_len);
    else
        wprintf(L"  very long : (keine)\n");

    Channel ch;
    std::wstring err;
    if (!ch.open(ep, &err)) {
        wprintf(L"  Kanal laesst sich nicht oeffnen: %s\n", err.c_str());
        return;
    }

    for (int slot = 0; slot <= kMaxPairedSlot; ++slot) {
        const uint8_t index = (slot == 0) ? kIndexDirect : static_cast<uint8_t>(slot);
        if (opt.only_slot >= 0 && index != static_cast<uint8_t>(opt.only_slot)) continue;

        Device dev(&ch, index);
        auto ping = dev.ping();
        if (!ping) continue;

        const std::wstring name = read_device_name(dev);
        wprintf(L"\n  --- Geraeteindex 0x%02X  HID++ %u.%u  \"%s\" ---\n",
                index, ping->major, ping->minor, name.empty() ? L"?" : name.c_str());

        dump_features(dev);
        dump_misc(dev, opt);
        dump_gkeys(dev);
        dump_dpi(dev, opt);
        dump_rate(dev, opt);
        apply_sets(dev, opt);
    }
}

// Selbsttest der Schicht, die auch die GUI benutzt: Manager starten, Schnappschuesse
// mitschreiben. Faengt Fehler, die in der reinen Protokollprobe nicht auffallen.
// G-Tasten in den Software-Modus schalten und mitschreiben, was hereinkommt.
// Der Software-Modus wird am Ende in jedem Fall wieder abgeschaltet, sonst waeren die
// G-Tasten nach Programmende tot.
int run_gkey_listen(int seconds) {
    auto eps = find_endpoints(kVendorLogitech);
    for (auto& ep : eps) {
        Channel ch;
        if (!ch.open(ep, nullptr)) continue;

        for (int slot = 0; slot <= kMaxPairedSlot; ++slot) {
            const uint8_t index = (slot == 0) ? kIndexDirect : static_cast<uint8_t>(slot);
            Device dev(&ch, index);
            if (dev.probe(300) != Device::SlotState::Awake) continue;
            if (!dev.has(kFeatGKeys)) continue;

            const uint8_t feat = dev.feature(kFeatGKeys)->index;
            Reply count = dev.call(kFeatGKeys, 0);
            wprintf(L"\nGeraet 0x%02X \"%s\": %u G-Tasten, GKeys auf Feature-Index %u\n",
                    index, read_device_name(dev).c_str(), count.param(0), feat);

            const DWORD t0 = GetTickCount();
            ch.set_notification_handler([t0](const uint8_t* d, size_t n) {
                wprintf(L"  %6lu ms  %s\n", GetTickCount() - t0, hex_dump(d, n).c_str());
                fflush(stdout);
            });

            print_reply(L"fn2 enableSoftwareControl(1)", dev.call(kFeatGKeys, 2, {1}));
            wprintf(L"  ... %d s lang G1..G5 druecken ...\n", seconds);
            fflush(stdout);
            Sleep(static_cast<DWORD>(seconds) * 1000);

            ch.set_notification_handler(nullptr);
            print_reply(L"fn2 enableSoftwareControl(0)", dev.call(kFeatGKeys, 2, {0}));
            return 0;
        }
    }
    wprintf(L"Kein Geraet mit Feature 0x8010 gefunden.\n");
    return 1;
}

// Schreibt mit, welche physische Maustaste welches Bit in 0x8110 ist. Noetig, weil die
// Positionen je Modell verschieden sind und nirgends dokumentiert stehen. Aendert das
// Mapping nicht; startSpy/stopSpy schalten nur die zusaetzlichen HID++-Meldungen.
int run_button_spy(int seconds, int only_slot) {
    auto eps = find_endpoints(kVendorLogitech);
    for (auto& ep : eps) {
        Channel ch;
        if (!ch.open(ep, nullptr)) continue;

        for (int slot = 0; slot <= kMaxPairedSlot; ++slot) {
            const uint8_t index = (slot == 0) ? kIndexDirect : static_cast<uint8_t>(slot);
            if (only_slot >= 0 && index != static_cast<uint8_t>(only_slot)) continue;
            Device dev(&ch, index);

            // Die Maus schlaeft staendig -- ein paar Sekunden Zeit zum Aufwecken geben.
            Device::SlotState state = dev.probe(400);
            for (int i = 0; i < 20 && state == Device::SlotState::Asleep; ++i) {
                if (i == 0) { wprintf(L"Maus schlaeft -- bitte kurz bewegen ...\n"); fflush(stdout); }
                Sleep(500);
                state = dev.probe(400);
            }
            if (state != Device::SlotState::Awake || !dev.has(kFeatMouseButtonSpy)) continue;

            const uint8_t feat = dev.feature(kFeatMouseButtonSpy)->index;
            wprintf(L"\nGeraet 0x%02X \"%s\": 0x8110 auf Feature-Index %u, Modus: %s\n",
                    index, read_device_name(dev).c_str(), feat,
                    onboard_mode_text(read_onboard_mode(dev)));
            print_reply(L"fn0 getMouseButtonCount",   dev.call(kFeatMouseButtonSpy, 0));
            print_reply(L"fn3 getMouseButtonMapping", dev.call(kFeatMouseButtonSpy, 3));

            uint16_t last = 0;
            const DWORD t0 = GetTickCount();
            ch.set_notification_handler([t0, index, feat, &last](const uint8_t* d, size_t n) {
                // Event 0, swId 0: 11 <idx> <feat> 00 <Zustand BE16>
                if (n < 6 || d[1] != index || d[2] != feat || d[3] != 0x00) return;
                const uint16_t mask = static_cast<uint16_t>((d[4] << 8) | d[5]);
                const uint16_t down = static_cast<uint16_t>(mask & ~last);
                last = mask;
                std::wstring pressed;
                for (int bit = 0; bit < 16; ++bit)
                    if (down & (1u << bit)) pressed += L" Position " + std::to_wstring(bit + 1)
                                                       + L" (Bit " + std::to_wstring(bit) + L")";
                wprintf(L"  %6lu ms  %s%s\n", GetTickCount() - t0, hex_dump(d, n).c_str(),
                        pressed.empty() ? L"" : (L"   gedrueckt:" + pressed).c_str());
                fflush(stdout);
            });

            print_reply(L"fn1 startMouseButtonSpy", dev.call(kFeatMouseButtonSpy, 1));
            wprintf(L"  ... %d s lang die Tasten einzeln und langsam druecken ...\n", seconds);
            fflush(stdout);
            Sleep(static_cast<DWORD>(seconds) * 1000);

            print_reply(L"fn2 stopMouseButtonSpy", dev.call(kFeatMouseButtonSpy, 2));
            ch.set_notification_handler(nullptr);
            return 0;
        }
    }
    wprintf(L"Kein waches Geraet mit Feature 0x8110 gefunden.\n");
    return 1;
}

// Belegt, dass 0x8100 fn1 wirklich setOnboardMode ist, und misst gleich, ob sich die
// Signalrate im Host-Modus setzen laesst. Stellt den Ausgangszustand wieder her.
int run_onboard_mode_test(int hz, int only_slot) {
    auto eps = find_endpoints(kVendorLogitech);
    for (auto& ep : eps) {
        Channel ch;
        if (!ch.open(ep, nullptr)) continue;
        for (int slot = 0; slot <= kMaxPairedSlot; ++slot) {
            const uint8_t index = (slot == 0) ? kIndexDirect : static_cast<uint8_t>(slot);
            if (only_slot >= 0 && index != static_cast<uint8_t>(only_slot)) continue;
            Device dev(&ch, index);
            if (dev.probe(400) != Device::SlotState::Awake) continue;
            if (!dev.has(kFeatOnboardProfiles) || !dev.has(kFeatReportRate)) continue;

            const OnboardMode before = read_onboard_mode(dev);
            wprintf(L"\n%s (0x%02X): Modus vorher = %s\n",
                    read_device_name(dev).c_str(), index, onboard_mode_text(before));

            std::wstring err;
            wprintf(L"  -> Host-Modus setzen: %s\n",
                    set_onboard_mode(dev, OnboardMode::Host, &err) ? L"OK" : err.c_str());

            RateCaps caps;
            read_rate(dev, index != kIndexDirect, &caps);
            err.clear();
            const bool rate_ok = write_rate(dev, caps, static_cast<uint16_t>(hz), &err);
            wprintf(L"  -> %d Hz setzen: %s\n", hz, rate_ok ? L"OK" : err.c_str());
            print_reply(L"  zurueckgelesen fn1 getReportRate", dev.call(kFeatReportRate, 1));

            // Ausgangszustand wiederherstellen -- Messung, kein Eingriff.
            if (caps.valid && caps.current_hz) write_rate(dev, caps, caps.current_hz, nullptr);
            err.clear();
            wprintf(L"  -> zurueck auf %s: %s\n", onboard_mode_text(before),
                    set_onboard_mode(dev, before, &err) ? L"OK" : err.c_str());
            return 0;
        }
    }
    wprintf(L"Kein passendes Geraet gefunden.\n");
    return 1;
}

// Betriebsmodus setzen -- zum Herstellen definierter Ausgangslagen beim Messen.
int run_set_mode(OnboardMode mode, int only_slot) {
    auto eps = find_endpoints(kVendorLogitech);
    for (auto& ep : eps) {
        Channel ch;
        if (!ch.open(ep, nullptr)) continue;
        for (int slot = 0; slot <= kMaxPairedSlot; ++slot) {
            const uint8_t index = (slot == 0) ? kIndexDirect : static_cast<uint8_t>(slot);
            if (only_slot >= 0 && index != static_cast<uint8_t>(only_slot)) continue;
            Device dev(&ch, index);
            if (dev.probe(400) != Device::SlotState::Awake) continue;
            if (!dev.has(kFeatOnboardProfiles)) continue;

            std::wstring err;
            const bool ok = set_onboard_mode(dev, mode, &err);
            wprintf(L"%s (0x%02X): %s -> %s\n", read_device_name(dev).c_str(), index,
                    onboard_mode_text(mode), ok ? L"OK" : err.c_str());
            return ok ? 0 : 1;
        }
    }
    wprintf(L"Kein passendes Geraet gefunden.\n");
    return 1;
}

int run_manager_selftest() {
    // Erst die Schritte einzeln, mit denselben Bausteinen wie der Manager -- so sieht man,
    // an welcher Stelle es klemmt, statt nur "nichts gefunden".
    wprintf(L"-- Schrittweise --\n");
    auto eps = find_endpoints(kVendorLogitech);
    wprintf(L"find_endpoints: %zu\n", eps.size());
    for (auto& ep : eps) {
        Channel ch;
        std::wstring err;
        wprintf(L"  open %s -> %s\n", ep.long_col.parent_id.c_str(),
                ch.open(ep, &err) ? L"ok" : err.c_str());
        if (!ch.is_open()) continue;
        for (int slot = 0; slot <= kMaxPairedSlot; ++slot) {
            const uint8_t index = (slot == 0) ? kIndexDirect : static_cast<uint8_t>(slot);
            const DWORD t0 = GetTickCount();
            Device dev(&ch, index);
            const uint8_t ping_params[3] = {0, 0, 0x5A};
            Reply raw = ch.call(index, kRootIndex, 1, ping_params, 3, 300);
            auto p = raw && raw.param(2) == 0x5A ? std::optional<Device::Ping>{{raw.param(0), raw.param(1)}}
                                                 : std::nullopt;
            const DWORD dt = GetTickCount() - t0;
            if (!p) {
                wprintf(L"    idx 0x%02X ping fehlgeschlagen (%lu ms): %s | roh: %s\n",
                        index, dt, describe(raw).c_str(),
                        raw.raw_len ? hex_dump(raw.raw.data(), raw.raw_len).c_str() : L"(nichts)");
                continue;
            }
            DpiCaps dpi;
            RateCaps rate;
            const bool dok = read_dpi(dev, &dpi);
            const bool rok = read_rate(dev, index != kIndexDirect, &rate);
            wprintf(L"    idx 0x%02X ping ok (%lu ms) dpi=%d[%s] rate=%d[%s]\n",
                    index, dt, dok ? 1 : 0, dpi.describe().c_str(),
                    rok ? 1 : 0, rate.describe().c_str());
        }
    }

    wprintf(L"\n-- Manager --\n");
    hidpp::Manager manager;
    manager.start(nullptr, 0, 0);

    for (int i = 0; i < 12; ++i) {
        Sleep(500);
        auto snap = manager.snapshot();
        if (!snap) { wprintf(L"[%2d] kein Schnappschuss\n", i); continue; }

        wprintf(L"[%2d] busy=%d ghub=%d geräte=%zu status=\"%s\"\n",
                i, snap->busy ? 1 : 0, snap->ghub_running ? 1 : 0,
                snap->devices.size(), snap->status.c_str());
        for (const auto& m : snap->devices) {
            wprintf(L"      %s key=%s idx=0x%02X %s art=%d dpi=%u rate=%u Hz gkeys=%d/%u featIdx=%u\n",
                    m.info.name.c_str(), m.key.c_str(), m.index,
                    m.connected ? L"verbunden" : L"getrennt",
                    static_cast<int>(m.kind), m.dpi.current, m.rate.current_hz,
                    m.gkeys.valid ? 1 : 0, m.gkeys.count, m.gkeys.feature_index);
        }
        if (!snap->devices.empty() && i >= 4) break;
    }

    manager.stop();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    SetConsoleOutputCP(CP_UTF8);

    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--manager") { warn_about_ghub(); return run_manager_selftest(); }
        else if (a == L"--listen") {
            warn_about_ghub();
            const int secs = (i + 1 < argc && argv[i + 1][0] != L'-') ? _wtoi(argv[++i]) : 15;
            return run_gkey_listen(secs > 0 ? secs : 15);
        }
        else if (a == L"--set-mode" && i + 1 < argc) {
            // --set-mode onboard|host [Geraeteindex]
            const std::wstring want = argv[++i];
            const OnboardMode mode = (_wcsicmp(want.c_str(), L"host") == 0)
                                         ? OnboardMode::Host : OnboardMode::Onboard;
            const int slot = (i + 1 < argc && argv[i + 1][0] != L'-') ? _wtoi(argv[++i]) : -1;
            return run_set_mode(mode, slot);
        }
        else if (a == L"--spy") {
            // --spy [Sekunden] [Geraeteindex]
            const int secs = (i + 1 < argc && argv[i + 1][0] != L'-') ? _wtoi(argv[++i]) : 20;
            const int slot = (i + 1 < argc && argv[i + 1][0] != L'-') ? _wtoi(argv[++i]) : -1;
            return run_button_spy(secs > 0 ? secs : 20, slot);
        }
        else if (a == L"--host-test") {
            // --host-test [Hz] [Geraeteindex]
            warn_about_ghub();
            const int hz   = (i + 1 < argc && argv[i + 1][0] != L'-') ? _wtoi(argv[++i]) : 500;
            const int slot = (i + 1 < argc && argv[i + 1][0] != L'-') ? _wtoi(argv[++i]) : -1;
            return run_onboard_mode_test(hz, slot);
        }
        else if (a == L"--extended" || a == L"-x") opt.extended = true;
        else if (a == L"--set-dpi"  && i + 1 < argc) opt.set_dpi  = _wtoi(argv[++i]);
        else if (a == L"--set-rate" && i + 1 < argc) opt.set_rate = _wtoi(argv[++i]);
        else if (a == L"--slot"     && i + 1 < argc) opt.only_slot = _wtoi(argv[++i]);
        else {
            wprintf(L"hidpp_dump [--extended] [--slot N] [--set-dpi N] [--set-rate HZ]\n");
            return a == L"--help" || a == L"-h" ? 0 : 2;
        }
    }

    warn_about_ghub();

    wprintf(L"=== Alle Logitech-HID-Interfaces ===\n");
    for (const auto& hi : enumerate(kVendorLogitech)) {
        wprintf(L"  PID %04X  UP=%04X U=%04X  in=%-3u out=%-3u  %s\n",
                hi.product_id, hi.usage_page, hi.usage, hi.input_len, hi.output_len,
                hi.product.empty() ? L"" : hi.product.c_str());
        wprintf(L"            %s\n", hi.path.c_str());
    }

    const auto endpoints = find_endpoints(kVendorLogitech);
    wprintf(L"\n=== %zu HID++-Endpunkt(e) ===\n", endpoints.size());
    if (endpoints.empty()) {
        wprintf(L"Keine 0xFF00-Collections gefunden -- ohne die geht nichts.\n");
        return 1;
    }
    for (const auto& ep : endpoints) probe_endpoint(ep, opt);

    wprintf(L"\nFertig.\n");
    return 0;
}
