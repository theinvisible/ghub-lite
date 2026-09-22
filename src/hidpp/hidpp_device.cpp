#include "hidpp/hidpp_device.h"

#include <windows.h>

extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <utility>
#include <vector>

namespace hidpp {
namespace {

std::wstring last_error_text() {
    const DWORD code = GetLastError();
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg = (n && buf) ? std::wstring(buf, n) : std::wstring(L"unbekannter Fehler");
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) msg.pop_back();
    wchar_t prefix[32];
    swprintf(prefix, 32, L"[%lu] ", code);
    return prefix + msg;
}

// Ein Lesekanal mit ueberlappendem ReadFile. Der HID-Klassentreiber puffert eingehende
// Reports pro Handle, es geht also nichts verloren, solange das Handle offen ist -- der
// Puffer wird in der Warteschleife unten geleert.
class Reader {
public:
    bool init(HANDLE h, uint16_t report_len) {
        h_ = h;
        buf_.resize(report_len ? report_len : 20);
        ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return ev_ != nullptr;
    }

    ~Reader() {
        cancel();
        if (ev_) CloseHandle(ev_);
    }

    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    HANDLE event() const { return ev_; }
    bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE && ev_ != nullptr; }

    // Stellt sicher, dass ein Lesevorgang laeuft. false = Handle unbrauchbar.
    bool arm() {
        if (!valid()) return false;
        if (pending_) return true;
        ResetEvent(ev_);
        ZeroMemory(&ov_, sizeof(ov_));
        ov_.hEvent = ev_;
        DWORD read = 0;
        if (ReadFile(h_, buf_.data(), static_cast<DWORD>(buf_.size()), &read, &ov_)) {
            pending_ = true;   // synchron fertig, das Event ist trotzdem gesetzt
            return true;
        }
        if (GetLastError() == ERROR_IO_PENDING) {
            pending_ = true;
            return true;
        }
        return false;
    }

    // Holt das Ergebnis eines abgeschlossenen Lesevorgangs ab.
    bool take(const uint8_t** data, size_t* len) {
        if (!pending_) return false;
        DWORD read = 0;
        const BOOL ok = GetOverlappedResult(h_, &ov_, &read, FALSE);
        if (!ok) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) return false;
            pending_ = false;
            return false;
        }
        pending_ = false;
        *data = buf_.data();
        *len = read;
        return read > 0;
    }

    void cancel() {
        if (!pending_ || !valid()) { pending_ = false; return; }
        CancelIoEx(h_, &ov_);
        DWORD read = 0;
        GetOverlappedResult(h_, &ov_, &read, TRUE);   // abwarten, sonst dangelt ov_
        pending_ = false;
    }

private:
    HANDLE h_ = nullptr;
    HANDLE ev_ = nullptr;
    OVERLAPPED ov_{};
    std::vector<uint8_t> buf_;
    bool pending_ = false;
};

HANDLE open_collection(const std::wstring& path, std::wstring* error_out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error_out) *error_out = last_error_text();
        return nullptr;
    }
    // Grosszuegiger Eingangspuffer: unaufgeforderte Meldungen sollen eine Antwort nicht
    // aus dem Ring draengen, waehrend wir noch auf sie warten.
    HidD_SetNumInputBuffers(h, 64);
    return h;
}

} // namespace

const wchar_t* status_text(Status s) {
    switch (s) {
        case Status::Ok:          return L"OK";
        case Status::Timeout:     return L"keine Antwort (Zeitueberschreitung)";
        case Status::IoError:     return L"E/A-Fehler";
        case Status::DeviceError: return L"Geraetefehler";
        case Status::NotOpen:     return L"Kanal nicht geoeffnet";
    }
    return L"?";
}

const wchar_t* error_text(uint8_t code, bool legacy) {
    if (legacy) {
        switch (code) {
            case 0x01: return L"unbekannte Unterfunktion";
            case 0x02: return L"ungueltige Adresse";
            case 0x03: return L"ungueltiger Wert";
            case 0x04: return L"Verbindung fehlgeschlagen";
            case 0x05: return L"zu viele Geraete";
            case 0x06: return L"bereits vorhanden";
            case 0x07: return L"belegt";
            case 0x08: return L"unbekanntes Geraet";
            case 0x09: return L"Ressourcenfehler";
            case 0x0A: return L"Anfrage nicht erlaubt";
            case 0x0B: return L"ungueltiger Parameter";
            case 0x0C: return L"falsche PIN";
            default:   return L"unbekannter 1.0-Fehler";
        }
    }
    switch (code) {
        case 0x00: return L"kein Fehler";
        case 0x01: return L"unbekannt";
        case 0x02: return L"ungueltiges Argument";
        case 0x03: return L"Wert ausserhalb des Bereichs";
        case 0x04: return L"Hardwarefehler";
        case 0x05: return L"Logitech-intern";
        case 0x06: return L"ungueltiger Feature-Index";
        case 0x07: return L"ungueltige Funktions-ID";
        case 0x08: return L"belegt";
        case 0x09: return L"nicht erlaubt";
        case 0x0A: return L"nicht unterstuetzt";
        default:   return L"unbekannter 2.0-Fehler";
    }
}

std::wstring describe(const Reply& r) {
    if (r.status == Status::DeviceError) {
        wchar_t buf[160];
        swprintf(buf, 160, L"%s 0x%02X (%s)",
                 r.legacy_error ? L"HID++1.0-Fehler" : L"HID++2.0-Fehler",
                 r.error_code, error_text(r.error_code, r.legacy_error));
        return buf;
    }
    return status_text(r.status);
}

std::wstring hex_dump(const uint8_t* data, size_t len) {
    std::wstring out;
    out.reserve(len * 3);
    wchar_t buf[4];
    for (size_t i = 0; i < len; ++i) {
        swprintf(buf, 4, L"%02X ", data[i]);
        out += buf;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

// Eine wartende Anfrage. Der Verteiler-Thread traegt die Antwort ein und weckt den
// Aufrufer, der in call() auf der Bedingungsvariablen haengt.
struct Channel::Waiter {
    uint8_t dev_index = 0;
    uint8_t feat_index = 0;
    uint8_t address = 0;
    Reply reply;
    bool done = false;
};

Channel::~Channel() { close(); }

bool Channel::open(const HidppEndpoint& ep, std::wstring* error_out) {
    close();
    ep_ = ep;

    long_h_ = open_collection(ep.long_col.path, error_out);
    if (!long_h_) return false;

    // Short und Very Long sind optional -- die G502 am Empfaenger hat short+long, die
    // G815 long+very long. Gelauscht wird auf allem, was sich oeffnen laesst, damit keine
    // Meldung auf einer nicht beobachteten Collection verpufft.
    if (ep.has_short())     short_h_     = open_collection(ep.short_col.path, nullptr);
    if (ep.has_very_long()) very_long_h_ = open_collection(ep.very_long_col.path, nullptr);

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    alive_ = true;
    dispatcher_ = std::thread([this] { dispatch_loop(); });
    return true;
}

void Channel::close() {
    if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
    if (dispatcher_.joinable()) dispatcher_.join();
    alive_ = false;
    if (stop_event_) { CloseHandle(static_cast<HANDLE>(stop_event_)); stop_event_ = nullptr; }

    // Erst nach dem Thread-Ende schliessen, sonst liest der Verteiler auf toten Handles.
    if (long_h_)      { CloseHandle(static_cast<HANDLE>(long_h_));      long_h_ = nullptr; }
    if (short_h_)     { CloseHandle(static_cast<HANDLE>(short_h_));     short_h_ = nullptr; }
    if (very_long_h_) { CloseHandle(static_cast<HANDLE>(very_long_h_)); very_long_h_ = nullptr; }

    // Wer noch wartet, bekommt einen Abbruch statt eines haengenden Timeouts.
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (Waiter* w : waiters_) {
            w->reply.status = Status::NotOpen;
            w->done = true;
        }
        waiters_.clear();
    }
    cv_.notify_all();
}

void Channel::set_notification_handler(NotificationHandler fn) {
    std::lock_guard<std::mutex> lock(mtx_);
    on_notification_ = std::move(fn);
}

// Ordnet eine eingetroffene Meldung einem Wartenden zu. Rueckgabe false = unaufgefordert.
bool Channel::deliver(const uint8_t* data, size_t len) {
    if (len < 4) return false;

    std::unique_lock<std::mutex> lock(mtx_);
    for (Waiter* w : waiters_) {
        if (w->done || data[1] != w->dev_index) continue;

        const bool match = data[2] == w->feat_index && data[3] == w->address;
        const bool err20 = data[2] == 0xFF;
        const bool err10 = data[2] == 0x8F;
        const bool match_err = (err20 || err10) && len >= 6 &&
                               data[3] == w->feat_index && data[4] == w->address;
        if (!match && !match_err) continue;

        if (match_err) {
            w->reply.status = Status::DeviceError;
            w->reply.error_code = data[5];
            w->reply.legacy_error = err10;
        } else {
            w->reply.status = Status::Ok;
        }
        w->reply.raw_len = len < w->reply.raw.size() ? len : w->reply.raw.size();
        for (size_t i = 0; i < w->reply.raw_len; ++i) w->reply.raw[i] = data[i];
        w->done = true;
        lock.unlock();
        cv_.notify_all();
        return true;
    }

    NotificationHandler handler = on_notification_;
    lock.unlock();
    if (handler) handler(data, len);
    return false;
}

void Channel::dispatch_loop() {
    Reader readers[3];
    HANDLE handles[3] = {static_cast<HANDLE>(long_h_), static_cast<HANDLE>(short_h_),
                         static_cast<HANDLE>(very_long_h_)};
    const uint16_t lens[3] = {ep_.long_col.input_len, ep_.short_col.input_len,
                              ep_.very_long_col.input_len};

    int active = 0;
    Reader* live[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (!handles[i]) continue;
        if (!readers[i].init(handles[i], lens[i])) continue;
        if (!readers[i].arm()) continue;
        live[active++] = &readers[i];
    }

    // Ohne die Long-Collection ist der Kanal nutzlos -- dann gleich als tot melden, statt
    // Anfragen in den Timeout laufen zu lassen.
    if (active == 0 || live[0] != &readers[0]) {
        alive_ = false;
        return;
    }

    for (;;) {
        HANDLE waits[4];
        DWORD n = 0;
        waits[n++] = static_cast<HANDLE>(stop_event_);
        for (int i = 0; i < active; ++i) waits[n++] = live[i]->event();

        const DWORD w = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;                       // close() hat gestoppt
        if (w < WAIT_OBJECT_0 || w >= WAIT_OBJECT_0 + n) break;

        const DWORD slot = w - WAIT_OBJECT_0 - 1;
        Reader* r = live[slot];
        const uint8_t* data = nullptr;
        size_t len = 0;
        if (r->take(&data, &len)) deliver(data, len);
        if (r->arm()) continue;

        // Handle unbrauchbar. Faellt nur eine Neben-Collection aus, weiter auf den uebrigen
        // lauschen -- sonst wuerde ein einzelner kaputter Nebenkanal den ganzen Kanal fuer
        // tot erklaeren und bei jedem Scan neu geoeffnet.
        if (r == &readers[0]) break;
        for (int i = static_cast<int>(slot); i + 1 < active; ++i) live[i] = live[i + 1];
        --active;
    }

    // Nicht nur beim Stoppen hier: bricht der Long-Leser ab (Geraet abgezogen), meldet das
    // healthy(), damit der Manager den Kanal neu oeffnet.
    alive_ = false;
}

Reply Channel::call(uint8_t dev_index, uint8_t feat_index, uint8_t func_id,
                    const uint8_t* params, size_t nparams, unsigned timeout_ms) {
    Reply reply;
    if (!is_open()) return reply;
    if (!alive_) { reply.status = Status::IoError; return reply; }   // niemand liest mehr

    // Erst anmelden, dann senden: der Verteiler-Thread liest schon, die Antwort kann
    // eintreffen, bevor WriteFile zurueckkehrt. Die swId wird im selben Zug vergeben.
    // Eine Verwechslung ist erst wieder moeglich, wenn eine Antwort mehr als 13 Anfragen
    // zu spaet kommt.
    Waiter waiter;
    waiter.dev_index = dev_index;
    waiter.feat_index = feat_index;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const uint8_t sw = next_swid_;
        next_swid_ = (sw >= kSwIdLast) ? kSwIdFirst : static_cast<uint8_t>(sw + 1);
        waiter.address = static_cast<uint8_t>((func_id << 4) | sw);
        waiters_.push_back(&waiter);
    }

    const size_t out_len = ep_.long_col.output_len ? ep_.long_col.output_len : 20;
    std::vector<uint8_t> out(out_len, 0);
    out[0] = kReportLong;
    out[1] = dev_index;
    out[2] = feat_index;
    out[3] = waiter.address;
    for (size_t i = 0; i < nparams && 4 + i < out.size(); ++i) out[4 + i] = params[i];

    // Beim Verlassen in jedem Fall wieder abmelden -- sonst zeigt der Verteiler auf einen
    // Wartenden, den es nicht mehr gibt.
    struct Unregister {
        Channel* ch;
        Waiter* w;
        ~Unregister() {
            std::lock_guard<std::mutex> lock(ch->mtx_);
            ch->waiters_.erase(std::remove(ch->waiters_.begin(), ch->waiters_.end(), w),
                               ch->waiters_.end());
        }
    } unreg{this, &waiter};

    OVERLAPPED wov{};
    wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    BOOL wok = WriteFile(static_cast<HANDLE>(long_h_), out.data(),
                         static_cast<DWORD>(out.size()), &written, &wov);
    if (!wok && GetLastError() == ERROR_IO_PENDING) {
        wok = GetOverlappedResult(static_cast<HANDLE>(long_h_), &wov, &written, TRUE);
    }
    if (!wok) {
        // Manche Geraete nehmen den Interrupt-OUT nicht an; SET_REPORT ueber den
        // Control-Endpunkt ist der uebliche Ausweg.
        wok = HidD_SetOutputReport(static_cast<HANDLE>(long_h_), out.data(),
                                   static_cast<ULONG>(out.size()));
    }
    if (wov.hEvent) CloseHandle(wov.hEvent);
    if (!wok) { reply.status = Status::IoError; return reply; }

    // Warten, bis der Verteiler die Antwort eingetragen hat.
    std::unique_lock<std::mutex> lock(mtx_);
    const bool got = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [&waiter] { return waiter.done; });
    reply = waiter.reply;
    if (!got && reply.status == Status::NotOpen) reply.status = Status::Timeout;
    return reply;
}

} // namespace hidpp
