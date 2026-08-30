#include "hidpp/device_manager.h"

#include "hidpp/hid_enum.h"
#include "hidpp/hidpp_device.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace hidpp {

const DeviceState* Snapshot::find(const std::wstring& k) const {
    for (const auto& m : devices) if (m.key == k) return &m;
    return nullptr;
}

bool ghub_processes_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            // lghub.exe, lghub_agent.exe, lghub_system_tray.exe, lghub_updater.exe
            if (_wcsnicmp(pe.szExeFile, L"lghub", 5) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

namespace {

// Wie lange auf ein Ping gewartet wird. Ein verbundenes Geraet antwortet in wenigen
// Millisekunden; der Wert begrenzt vor allem, wie lange ein leerer Steckplatz kostet.
constexpr unsigned kPingTimeoutMs = 400;
constexpr unsigned kTickMs = 4000;

struct Command {
    enum class Kind { Refresh, FullScan, SetDpi, SetRate, SetGKeys, SetDesired, Quit };
    Kind kind = Kind::Refresh;
    std::wstring key;
    uint16_t value = 0;
    Desired desired{};
};

// Was der Verteiler-Thread braucht, um eine G-Tasten-Meldung zu deuten. Bewusst eine
// eigene, kleine Struktur: der Verteiler laeuft nicht auf dem Worker-Thread und darf
// deshalb die Geraeteliste nicht anfassen.
struct GKeyWatch {
    uint8_t  dev_index = 0;
    uint8_t  feature_index = 0;
    uint32_t unit_id = 0;
    uint16_t last_mask = 0;
};

// Ein geoeffneter Empfaenger bzw. ein direkt angeschlossenes Geraet, samt der darauf
// gefundenen HID++-Geraete. Die Device-Objekte halten den Feature-Cache -- sie bleiben
// deshalb ueber Ticks hinweg am Leben.
struct EndpointCtx {
    HidppEndpoint ep;
    std::unique_ptr<Channel> channel;
    std::map<uint8_t, std::unique_ptr<Device>> slots;
};

} // namespace

struct Manager::Impl {
    HWND notify_hwnd = nullptr;
    UINT notify_msg  = 0;
    UINT gkey_msg    = 0;

    // Wird vom Verteiler-Thread gelesen und geschrieben, vom Worker gepflegt.
    std::mutex gkey_mtx;
    std::map<std::wstring, std::vector<GKeyWatch>> gkey_watches;   // nach Endpunkt

    // Laeuft auf dem Verteiler-Thread des Kanals, nicht auf dem Worker.
    void on_notification(const std::wstring& endpoint_key, const uint8_t* data, size_t len) {
        std::vector<std::pair<uint8_t, bool>> edges;   // (Taste 1..n, gedrueckt)
        uint32_t unit_id = 0;
        {
            std::lock_guard<std::mutex> lock(gkey_mtx);
            auto it = gkey_watches.find(endpoint_key);
            if (it == gkey_watches.end()) return;

            for (auto& w : it->second) {
                uint16_t mask = 0;
                if (!decode_gkey_event(data, len, w.dev_index, w.feature_index, &mask)) continue;

                // Die Tastatur schickt bei jeder Aenderung den vollstaendigen Zustand.
                // Flanken entstehen erst im Vergleich mit der vorigen Maske.
                const uint16_t changed = static_cast<uint16_t>(mask ^ w.last_mask);
                w.last_mask = mask;
                unit_id = w.unit_id;
                for (int bit = 0; bit < 16; ++bit) {
                    if (!(changed & (1u << bit))) continue;
                    edges.emplace_back(static_cast<uint8_t>(bit + 1), (mask & (1u << bit)) != 0);
                }
                break;
            }
        }

        if (!notify_hwnd || !gkey_msg) return;
        for (const auto& [gkey, pressed] : edges) {
            PostMessageW(notify_hwnd, gkey_msg,
                         MAKEWPARAM(gkey, pressed ? 1 : 0), static_cast<LPARAM>(unit_id));
        }
    }

    std::thread worker;
    std::atomic<bool> running{false};

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Command> queue;

    mutable std::mutex snap_mtx;
    std::shared_ptr<const Snapshot> snap = std::make_shared<Snapshot>();

    std::map<std::wstring, EndpointCtx> endpoints;   // nach parent_id
    std::map<std::wstring, Desired> desired;         // nach Unit-Key
    std::vector<DeviceState> devices;

    void push(Command c) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            queue.push_back(std::move(c));
        }
        cv.notify_one();
    }

    std::wstring last_status;
    bool last_status_error = false;

    // Ein leerer Text bedeutet "nichts Neues zu melden" -- dann bleibt die letzte Meldung
    // stehen, statt vom naechsten Leerlauf-Tick weggewischt zu werden.
    void publish(std::wstring status, bool is_error, bool busy) {
        if (status.empty()) {
            status = last_status;
            is_error = last_status_error;
        } else {
            last_status = status;
            last_status_error = is_error;
        }

        // Den erzwungenen Host-Modus mitgeben, damit die Oberflaeche ihn speichern kann.
        for (auto& d : devices) {
            auto it = desired.find(d.key);
            d.host_mode_forced = it != desired.end() && it->second.host_mode;
        }

        auto s = std::make_shared<Snapshot>();
        s->devices = devices;
        s->status = std::move(status);
        s->status_is_error = is_error;
        s->busy = busy;
        s->ghub_running = ghub_running_cached();
        {
            std::lock_guard<std::mutex> lock(snap_mtx);
            snap = s;
        }
        if (notify_hwnd) PostMessageW(notify_hwnd, notify_msg, 0, 0);
    }

    // ghub_processes_running() legt einen Schnappschuss *aller* Systemprozesse an. Das bei
    // jedem Publish zu tun ist der teuerste Einzelposten im Leerlauf -- und G HUB startet
    // und stoppt nicht im Sekundentakt.
    ULONGLONG ghub_checked_at = 0;
    bool ghub_running_flag = false;

    bool ghub_running_cached() {
        const ULONGLONG now = GetTickCount64();
        if (ghub_checked_at == 0 || now - ghub_checked_at >= 30000) {
            ghub_running_flag = ghub_processes_running();
            ghub_checked_at = now;
        }
        return ghub_running_flag;
    }

    // Warum wurde nichts gefunden? Wird in die Statuszeile durchgereicht, damit man den
    // Unterschied zwischen "kein Empfaenger da" und "Kanal laesst sich nicht oeffnen" sieht.
    size_t last_endpoint_count = 0;
    std::wstring last_open_error;

    // Endpunkte mit der Wirklichkeit abgleichen: neue oeffnen, verschwundene wegwerfen.
    void sync_endpoints() {
        auto found = find_endpoints(kVendorLogitech);
        last_endpoint_count = found.size();
        last_open_error.clear();

        std::map<std::wstring, HidppEndpoint> by_key;
        for (auto& ep : found) by_key[ep.long_col.parent_id] = ep;

        for (auto it = endpoints.begin(); it != endpoints.end();) {
            if (by_key.find(it->first) == by_key.end()) it = endpoints.erase(it);
            else ++it;
        }

        for (auto& [key, ep] : by_key) {
            if (endpoints.count(key)) continue;
            EndpointCtx ctx;
            ctx.ep = ep;
            ctx.channel = std::make_unique<Channel>();
            std::wstring err;
            if (!ctx.channel->open(ep, &err)) {   // z.B. exklusiv belegt
                last_open_error = err;
                continue;
            }
            const std::wstring ep_key = key;
            ctx.channel->set_notification_handler(
                [this, ep_key](const uint8_t* d, size_t n) { on_notification(ep_key, d, n); });
            endpoints.emplace(key, std::move(ctx));
        }
    }

    std::wstring scan_failure_reason() const {
        if (last_endpoint_count == 0)
            return L"Kein Logitech-HID++-Endpunkt gefunden (Empfänger angesteckt?).";
        if (endpoints.empty())
            return L"HID++-Kanal ließ sich nicht öffnen: " +
                   (last_open_error.empty() ? std::wstring(L"unbekannter Grund") : last_open_error);
        return L"Empfänger erreichbar, aber kein gepaartes Gerät mit DPI oder Signalrate.";
    }

    // Volles Abklopfen aller Steckplaetze. Teuer, deshalb nur beim Start, bei
    // WM_DEVICECHANGE und auf ausdrueckliche Anforderung.
    void full_scan() {
        sync_endpoints();
        devices.clear();

        for (auto& [key, ctx] : endpoints) {
            // Erst alle Steckplaetze abklopfen. Ein leerer Platz wird vom Empfaenger prompt
            // mit 0x08 quittiert -- eine Zeitueberschreitung heisst deshalb nicht "leer",
            // sondern "da ist etwas, das gerade nicht funkt". Das gilt aber nur, solange der
            // Empfaenger ueberhaupt antwortet; sonst entstuenden lauter Phantomgeraete.
            std::map<uint8_t, Device::SlotState> states;
            bool receiver_talks = false;
            for (int slot = 0; slot <= kMaxPairedSlot; ++slot) {
                const uint8_t index = (slot == 0) ? kIndexDirect : static_cast<uint8_t>(slot);
                auto& slot_dev = ctx.slots[index];
                if (!slot_dev) slot_dev = std::make_unique<Device>(ctx.channel.get(), index);
                const auto state = slot_dev->probe(kPingTimeoutMs);
                states[index] = state;
                if (state != Device::SlotState::NoAnswer) receiver_talks = true;
            }

            for (const auto& [index, state] : states) {
                if (state == Device::SlotState::Empty) continue;
                if (state == Device::SlotState::NoAnswer && !receiver_talks) continue;

                DeviceState st;
                st.index = index;
                st.wireless = (index != kIndexDirect);
                st.endpoint_key = key;

                if (state != Device::SlotState::Awake) {
                    // Gepaart, aber gerade nicht erreichbar: als bekanntes, getrenntes Geraet
                    // fuehren statt es zu verschweigen. Sobald es aufwacht, fuellt tick() die
                    // Daten nach und wendet die Wunschwerte an. Bis dahin steht ein
                    // vorlaeufiger Schluessel drin -- die Unit-ID gibt nur das wache Geraet her.
                    st.connected = false;
                    st.info.name = L"Gerät (schläft)";
                    st.key = L"?" + std::to_wstring(index);
                    devices.push_back(std::move(st));
                    continue;
                }

                st.connected = true;
                if (!load_details(*ctx.slots[index], st)) continue;
                devices.push_back(std::move(st));
            }
        }
        rebuild_gkey_watches();
    }

    // Alles vom wachen Geraet einlesen -- Name, Bereiche, Faehigkeiten. Nur beim Verbinden
    // aufrufen: das kostet rund ein Dutzend HID++-Runden, und nichts davon aendert sich
    // im laufenden Betrieb. false, wenn das Geraet keine der Faehigkeiten hat, um die es
    // uns geht (der Empfaenger auf 0xFF etwa).
    bool load_details(Device& dev, DeviceState& st) {
        read_device_info(dev, &st.info);
        st.key = st.info.unit_key();
        read_dpi(dev, &st.dpi);
        read_rate(dev, st.wireless, &st.rate);
        read_gkeys(dev, &st.gkeys);

        // Der Geraetetyp ergibt sich aus dem, was das Geraet kann, nicht aus dem Namen.
        if (st.dpi.valid)        st.kind = DeviceKind::Mouse;
        else if (st.gkeys.valid) st.kind = DeviceKind::Keyboard;

        // Hat alles geklappt, was das Geraet laut Feature-Tabelle koennen muesste? Nur dann
        // darf der Tick sich das erneute Lesen sparen. dev.has() geht ueber den
        // Feature-Cache und kostet nach dem ersten Mal nichts.
        // Name plus mindestens eine Faehigkeit: reisst beim Aufwachen eine Abfrage ab,
        // fehlt in aller Regel beides, und der naechste Tick holt es nach.
        bool complete = !st.info.name.empty() &&
                        (st.dpi.valid || st.rate.valid || st.gkeys.valid);
        if (dev.has(kFeatAdjustableDpi) || dev.has(kFeatExtAdjustableDpi))
            complete = complete && st.dpi.valid;
        if (dev.has(kFeatReportRate) || dev.has(kFeatExtReportRate))
            complete = complete && st.rate.valid;
        if (dev.has(kFeatGKeys))
            complete = complete && st.gkeys.valid;
        st.details_complete = complete;

        return st.dpi.valid || st.rate.valid || st.gkeys.valid;
    }

    // Der guenstige Teil: nur was sich tatsaechlich aendern kann. DPI und Rate muessen
    // bleiben, weil die DPI-Taste an der Maus und G HUB sie von aussen verstellen.
    // Akku und Onboard-Modus aendern sich langsam -- die kommen nur alle paar Ticks dran.
    void refresh_dynamic(Device& dev, DeviceState& st, bool slow_turn) {
        if (st.dpi.valid) {
            if (Reply r = dev.call(kFeatAdjustableDpi, 2, {0})) st.dpi.current = r.param_u16(1);
        }
        if (st.rate.valid && st.rate.via_feature == kFeatReportRate) {
            if (Reply r = dev.call(kFeatReportRate, 1); r && r.param(0) > 0)
                st.rate.current_hz = static_cast<uint16_t>(1000 / r.param(0));
        }
        if (!slow_turn) return;

        if (st.info.has_battery && dev.has(kFeatUnifiedBattery)) {
            if (Reply r = dev.call(kFeatUnifiedBattery, 1)) {
                st.info.battery_percent = r.param(0);
                const uint8_t cs = r.param(2);
                st.info.charge = cs <= static_cast<uint8_t>(ChargeState::Error)
                                     ? static_cast<ChargeState>(cs) : ChargeState::Discharging;
            }
        }
        if (dev.has(kFeatOnboardProfiles)) {
            if (Reply r = dev.call(kFeatOnboardProfiles, 2)) {
                const uint8_t m = r.param(0);
                st.info.onboard = (m == 1 || m == 2) ? static_cast<OnboardMode>(m)
                                                     : OnboardMode::Unknown;
            }
        }
    }

    // Billige Signatur ueber alles, was die Oberflaeche zeigt. Aendert sie sich nicht,
    // braucht niemand einen neuen Schnappschuss.
    uint64_t view_signature() const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        for (const auto& d : devices) {
            mix(d.connected ? 1 : 0);
            mix(d.dpi.current);
            mix(d.rate.current_hz);
            mix(d.info.battery_percent);
            mix(static_cast<uint64_t>(d.info.onboard));
            mix(static_cast<uint64_t>(d.info.charge));
            mix(std::hash<std::wstring>{}(d.key));
            // Auch die Faehigkeiten selbst: fuellt sich eine nachgeholte Ratenliste,
            // muss die Oberflaeche das mitbekommen.
            mix(d.rate.rates_hz.size());
            mix(d.dpi.segments.size());
            mix(d.gkeys.count);
        }
        mix(std::hash<std::wstring>{}(last_status));
        return h;
    }

    // Die Beobachtungsliste des Verteiler-Threads neu aufbauen. Nur verbundene Geraete mit
    // G-Tasten stehen drin.
    void rebuild_gkey_watches() {
        std::map<std::wstring, std::vector<GKeyWatch>> fresh;
        for (const auto& st : devices) {
            if (!st.connected || !st.gkeys.valid) continue;
            GKeyWatch w;
            w.dev_index = st.index;
            w.feature_index = st.gkeys.feature_index;
            w.unit_id = st.info.unit_id;
            fresh[st.endpoint_key].push_back(w);
        }
        std::lock_guard<std::mutex> lock(gkey_mtx);
        gkey_watches.swap(fresh);
    }

    // Sammelurteil ueber den aktuellen Bestand, fuer die Statuszeile.
    std::wstring current_status(bool* is_error) const {
        if (devices.empty()) { *is_error = true; return scan_failure_reason(); }
        const bool any_awake = std::any_of(devices.begin(), devices.end(),
                                           [](const DeviceState& m) { return m.connected; });
        *is_error = !any_awake;
        return any_awake ? L"Bereit."
                         : L"Gerät gefunden, schläft gerade — kurz bewegen oder drücken.";
    }

    // Guenstiger Durchlauf: nur bekannte Geraete anpingen und die veraenderlichen Werte
    // auffrischen. Gibt true zurueck, wenn sich der Verbindungszustand geaendert hat.
    unsigned tick_count = 0;
    uint64_t last_signature = 0;

    bool tick() {
        bool changed = false;
        // Akku und Onboard-Modus nur etwa einmal pro Minute -- ein Prozentwert, der im
        // Vier-Sekunden-Takt zappelt, ist ohnehin Augenwischerei.
        const bool slow_turn = (tick_count++ % 15) == 0;
        for (auto& st : devices) {
            auto ep_it = endpoints.find(st.endpoint_key);
            if (ep_it == endpoints.end()) { st.connected = false; continue; }

            auto& slot_dev = ep_it->second.slots[st.index];
            if (!slot_dev) slot_dev = std::make_unique<Device>(ep_it->second.channel.get(), st.index);

            const bool was = st.connected;
            st.connected = slot_dev->probe(kPingTimeoutMs) == Device::SlotState::Awake;
            if (was != st.connected) changed = true;
            if (!st.connected) continue;

            if (!was) {
                // Frisch verbunden: einmal alles lesen, dann die Wunschwerte anwenden.
                // Das ist der Ersatz fuer das Schreiben ins Onboard-Profil.
                load_details(*slot_dev, st);
                apply_desired(*slot_dev, st);
            } else if (!st.details_complete) {
                // Beim Verbinden ist etwas durchgerutscht -- nachholen, bis es sitzt.
                load_details(*slot_dev, st);
                if (st.details_complete) changed = true;
            } else {
                refresh_dynamic(*slot_dev, st, slow_turn);
            }
        }
        if (changed) rebuild_gkey_watches();
        return changed;
    }

    void apply_desired(Device& dev, DeviceState& st) {
        auto it = desired.find(st.key);
        if (it == desired.end()) return;

        // Zuerst der Modus: im Onboard-Modus lehnt das Geraet setReportRate ab, die
        // Reihenfolge ist also nicht beliebig.
        if (it->second.host_mode && st.info.onboard != OnboardMode::Host) {
            if (set_onboard_mode(dev, OnboardMode::Host, nullptr))
                st.info.onboard = OnboardMode::Host;
        }

        if (it->second.dpi && st.dpi.valid && st.dpi.current != it->second.dpi) {
            if (write_dpi(dev, st.dpi, it->second.dpi, nullptr))
                read_dpi(dev, &st.dpi);
        }
        if (it->second.rate_hz && st.rate.valid && st.rate.current_hz != it->second.rate_hz) {
            if (write_rate(dev, st.rate, it->second.rate_hz, nullptr))
                read_rate(dev, st.wireless, &st.rate);
        }
        // Der Software-Modus ueberlebt kein Aus/Ein der Tastatur -- nach jeder Wiederkehr
        // neu setzen, sonst gehen die G-Tasten still auf ihre Onboard-Belegung zurueck.
        if (it->second.gkeys_active && st.gkeys.valid)
            set_gkey_software_mode(dev, true, nullptr);
    }

    Device* device_for(const DeviceState& st) {
        auto ep_it = endpoints.find(st.endpoint_key);
        if (ep_it == endpoints.end()) return nullptr;
        auto& slot_dev = ep_it->second.slots[st.index];
        if (!slot_dev) slot_dev = std::make_unique<Device>(ep_it->second.channel.get(), st.index);
        return slot_dev.get();
    }

    DeviceState* device_by_key(const std::wstring& key) {
        for (auto& m : devices) if (m.key == key) return &m;
        return nullptr;
    }

    void handle(const Command& c) {
        switch (c.kind) {
            case Command::Kind::FullScan: {
                publish(L"Suche Geräte …", false, true);
                full_scan();
                for (auto& st : devices) {
                    if (!st.connected) continue;
                    if (Device* d = device_for(st)) apply_desired(*d, st);
                }

                bool err = false;
                const std::wstring msg = current_status(&err);
                publish(msg, err, false);
                break;
            }

            case Command::Kind::Refresh: {
                // Nur wenn sich die Erreichbarkeit geaendert hat, eine neue Meldung setzen --
                // sonst wuerde ein Tick die letzte Rueckmeldung ("... gesetzt") wegwischen.
                const bool changed = tick();
                bool err = false;
                publish(changed ? current_status(&err) : std::wstring(), err, false);
                break;
            }

            case Command::Kind::SetDesired: {
                desired[c.key] = c.desired;
                // Nicht nur merken, sondern gleich herstellen: sonst wirkte der
                // Wunschzustand erst nach dem naechsten Reconnect, und ein Neustart des
                // Programms haette die gespeicherten Werte stillschweigend ignoriert.
                DeviceState* st = device_by_key(c.key);
                if (!st || !st->connected) break;
                if (Device* dev = device_for(*st)) {
                    apply_desired(*dev, *st);
                    rebuild_gkey_watches();
                    publish(L"", false, false);
                }
                break;
            }

            case Command::Kind::SetDpi: {
                DeviceState* st = device_by_key(c.key);
                Device* dev = st ? device_for(*st) : nullptr;
                if (!st || !dev) { publish(L"Gerät nicht erreichbar.", true, false); break; }

                std::wstring err;
                if (!write_dpi(*dev, st->dpi, c.value, &err)) {
                    publish(L"DPI setzen fehlgeschlagen: " + err, true, false);
                    break;
                }
                read_dpi(*dev, &st->dpi);
                desired[c.key].dpi = st->dpi.current;
                wchar_t msg[80];
                swprintf(msg, 80, L"Auflösung auf %u DPI gesetzt.", st->dpi.current);
                publish(msg, false, false);
                break;
            }

            case Command::Kind::SetRate: {
                DeviceState* st = device_by_key(c.key);
                Device* dev = st ? device_for(*st) : nullptr;
                if (!st || !dev) { publish(L"Gerät nicht erreichbar.", true, false); break; }

                std::wstring err;
                bool ok = write_rate(*dev, st->rate, c.value, &err);
                bool switched = false;

                // Gemessen an G502 X PLUS und G815: im Onboard-Modus gehoert die Signalrate
                // dem Profil im Geraet und setReportRate wird mit 0x02 abgelehnt. Im
                // Host-Modus geht sie. Also einmal umschalten und erneut versuchen -- das
                // ist ein reiner Moduswechsel, kein Schreiben in den Profilspeicher.
                if (!ok && st->info.onboard == OnboardMode::Onboard) {
                    std::wstring mode_err;
                    if (set_onboard_mode(*dev, OnboardMode::Host, &mode_err)) {
                        st->info.onboard = OnboardMode::Host;
                        switched = true;
                        err.clear();
                        ok = write_rate(*dev, st->rate, c.value, &err);
                        if (!ok) {
                            // Umsonst umgeschaltet -- dann auch wieder zurueck.
                            set_onboard_mode(*dev, OnboardMode::Onboard, nullptr);
                            st->info.onboard = OnboardMode::Onboard;
                            switched = false;
                        }
                    } else {
                        err += L" — Umschalten in den Host-Modus scheiterte: " + mode_err;
                    }
                }

                if (!ok) {
                    publish(L"Signalrate setzen fehlgeschlagen: " + err, true, false);
                    break;
                }
                if (switched) desired[c.key].host_mode = true;
                read_rate(*dev, st->wireless, &st->rate);
                desired[c.key].rate_hz = st->rate.current_hz;

                wchar_t msg[200];
                if (switched)
                    swprintf(msg, 200, L"Signalrate auf %u Hz gesetzt — dafür in den "
                                       L"Host-Modus geschaltet, beim Beenden zurück.",
                             st->rate.current_hz);
                else
                    swprintf(msg, 200, L"Signalrate auf %u Hz gesetzt.", st->rate.current_hz);
                publish(msg, false, false);
                break;
            }

            case Command::Kind::SetGKeys: {
                DeviceState* st = device_by_key(c.key);
                Device* dev = st ? device_for(*st) : nullptr;
                if (!st || !dev || !st->gkeys.valid) {
                    publish(L"Tastatur nicht erreichbar.", true, false);
                    break;
                }

                const bool on = c.value != 0;
                std::wstring err;
                if (!set_gkey_software_mode(*dev, on, &err)) {
                    publish(L"G-Tasten umschalten fehlgeschlagen: " + err, true, false);
                    break;
                }
                desired[c.key].gkeys_active = on;
                rebuild_gkey_watches();
                publish(on ? L"G-Tasten werden von ghub-lite ausgewertet."
                           : L"G-Tasten wieder auf Onboard-Belegung.", false, false);
                break;
            }

            case Command::Kind::Quit:
                for (auto& st : devices) {
                    if (!st.connected) continue;
                    Device* d = device_for(st);
                    if (!d) continue;

                    // Software-Modus aus, sonst blieben die G-Tasten tot zurueck.
                    if (st.gkeys.valid) set_gkey_software_mode(*d, false, nullptr);

                    // Host-Modus nur dort zuruecknehmen, wo wir ihn selbst gesetzt haben:
                    // ohne ghub-lite soll das Geraet wieder sein eigenes Profil benutzen.
                    auto it = desired.find(st.key);
                    if (it != desired.end() && it->second.host_mode)
                        set_onboard_mode(*d, OnboardMode::Onboard, nullptr);
                }
                break;
        }
    }

    void run() {
        push(Command{Command::Kind::FullScan});

        while (running) {
            std::deque<Command> batch;
            {
                std::unique_lock<std::mutex> lock(mtx);
                if (queue.empty())
                    cv.wait_for(lock, std::chrono::milliseconds(kTickMs));
                if (!running) break;
                batch.swap(queue);
            }

            if (batch.empty()) {
                // Leerlauf-Tick: Verbindungsstatus pflegen. Publiziert wird nur, wenn sich
                // an dem, was die Oberflaeche zeigt, wirklich etwas geaendert hat -- sonst
                // kostet jeder Tick eine Kopie der Geraeteliste, eine Nachricht und einen
                // kompletten Neuaufbau des Fensters, fuer nichts.
                const bool changed = tick();
                bool err = false;
                const std::wstring status = changed ? current_status(&err) : std::wstring();

                const uint64_t sig = view_signature();
                if (changed || sig != last_signature) {
                    last_signature = sig;
                    publish(status, err, false);
                }
                continue;
            }

            for (const auto& c : batch) {
                // Quit muss durch handle(): dort wird der Software-Modus der G-Tasten
                // abgeschaltet, solange die Kanaele noch offen sind.
                handle(c);
                if (c.kind == Command::Kind::Quit) { running = false; break; }
            }
        }
    }
};

Manager::Manager() : impl_(std::make_unique<Impl>()) {}

Manager::~Manager() { stop(); }

void Manager::start(void* notify_hwnd, unsigned notify_msg, unsigned gkey_msg) {
    if (impl_->running) return;
    impl_->notify_hwnd = static_cast<HWND>(notify_hwnd);
    impl_->notify_msg  = notify_msg;
    impl_->gkey_msg    = gkey_msg;
    impl_->running = true;
    impl_->worker = std::thread([this] { impl_->run(); });
}

void Manager::stop() {
    if (!impl_->running) return;
    // running erst vom Worker selbst zuruecksetzen lassen: er soll Quit noch abarbeiten
    // und dabei den Software-Modus der G-Tasten sauber abschalten.
    impl_->push(Command{Command::Kind::Quit});
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->running = false;
    impl_->endpoints.clear();
}

void Manager::refresh(bool full) {
    impl_->push(Command{full ? Command::Kind::FullScan : Command::Kind::Refresh});
}

void Manager::set_dpi(const std::wstring& key, uint16_t dpi) {
    Command c{Command::Kind::SetDpi};
    c.key = key;
    c.value = dpi;
    impl_->push(std::move(c));
}

void Manager::set_rate(const std::wstring& key, uint16_t hz) {
    Command c{Command::Kind::SetRate};
    c.key = key;
    c.value = hz;
    impl_->push(std::move(c));
}

void Manager::set_gkey_active(const std::wstring& key, bool on) {
    Command c{Command::Kind::SetGKeys};
    c.key = key;
    c.value = on ? 1 : 0;
    impl_->push(std::move(c));
}

void Manager::set_desired(const std::wstring& key, Desired d) {
    Command c{Command::Kind::SetDesired};
    c.key = key;
    c.desired = d;
    impl_->push(std::move(c));
}

std::shared_ptr<const Snapshot> Manager::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->snap_mtx);
    return impl_->snap;
}

} // namespace hidpp
