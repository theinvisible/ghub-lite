// Geraeteverwaltung auf einem eigenen Thread.
//
// Die UI besitzt keine Geraete-Handles und ruft nie blockierend: ein schlafendes Funkgeraet
// laeuft in den Timeout, und das darf das Fenster nicht einfrieren. Kommandos gehen ueber
// eine Queue hinein, Ergebnisse kommen als Schnappschuss zurueck, angekuendigt per
// PostMessage.
#pragma once

#include "hidpp/feat_buttons.h"
#include "hidpp/feat_dpi.h"
#include "hidpp/feat_gkeys.h"
#include "hidpp/feat_info.h"
#include "hidpp/feat_rate.h"
#include "hidpp/hidpp_root.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hidpp {

enum class DeviceKind : uint8_t { Unknown, Mouse, Keyboard };

// Ein gefundenes Geraet mit den Faehigkeiten, die es tatsaechlich meldet. Welche Bloecke
// gueltig sind, entscheidet das Geraet -- eine Maus hat kein gkeys, eine Tastatur kein dpi.
struct DeviceState {
    std::wstring key;              // Unit-ID als Hex, Schluessel fuer Einstellungen
    std::wstring endpoint_key;     // Instanz-ID des USB-Interfaces
    uint8_t  index     = 0;        // HID++-Geraeteindex
    bool     wireless  = false;    // haengt an einem Empfaenger
    bool     connected = false;
    DeviceKind kind    = DeviceKind::Unknown;
    DeviceInfo info;
    DpiCaps  dpi;
    RateCaps rate;
    GKeyCaps gkeys;

    // true, wenn ghub-lite selbst den Host-Modus gesetzt hat. Die Oberflaeche schreibt das
    // in die Einstellungen, damit es einen Neustart uebersteht.
    bool host_mode_forced = false;

    // false, wenn beim einmaligen Auslesen etwas nicht geklappt hat. Ein gerade
    // aufgewachtes Funkgeraet laesst schon mal eine Abfrage aus dem ersten Block ins
    // Leere laufen; dann fehlt dauerhaft eine Faehigkeit, weil der guenstige Tick sie
    // nicht noch einmal liest. Der Manager wiederholt das Lesen, solange das hier false
    // ist -- sonst bliebe z.B. die Ratenliste fuer immer leer.
    bool details_complete = false;
};

struct Snapshot {
    std::vector<DeviceState> devices;
    std::wstring status;           // letzte Meldung fuer die Statuszeile
    bool status_is_error = false;
    bool ghub_running = false;
    bool busy = false;

    const DeviceState* find(const std::wstring& key) const;
};

// Wunschzustand, der nach Reconnect oder Programmstart erneut hergestellt wird.
struct Desired {
    uint16_t dpi          = 0;      // 0 = nicht erzwingen
    uint16_t rate_hz      = 0;
    bool     gkeys_active = false;  // G-Tasten im Software-Modus halten

    // Host-Modus (0x8100) erzwingen. Wird nur gesetzt, wenn das Geraet eine Signalrate
    // im Onboard-Modus verweigert hat -- dort gehoert sie dem Geraeteprofil. Beim Beenden
    // schaltet der Manager zurueck auf Onboard, damit die Maus ohne ghub-lite wieder ihr
    // eigenes Profil benutzt.
    bool     host_mode    = false;
};

// Laufen gerade G-HUB-Prozesse? Die konkurrieren um denselben HID++-Kanal.
bool ghub_processes_running();

class Manager {
public:
    Manager();
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    // notify_msg wird nach jeder Aenderung an notify_hwnd gepostet (ohne Nutzdaten --
    // der Empfaenger holt sich snapshot()).
    //
    // gkey_msg meldet einen G-Tastendruck, und zwar sofort, nicht ueber den Schnappschuss:
    //     wParam = MAKEWPARAM(taste 1..n, gedrueckt 0/1)
    //     lParam = Unit-ID des Geraets
    // Ausgefuehrt wird die Aktion bewusst auf dem UI-Thread: dort liegt die Belegungs-
    // tabelle, und PostMessage kostet im Leerlauf weit unter einer Millisekunde.
    void start(void* notify_hwnd, unsigned notify_msg, unsigned gkey_msg);
    void stop();

    void refresh(bool full_scan);
    void set_dpi(const std::wstring& key, uint16_t dpi);
    void set_rate(const std::wstring& key, uint16_t hz);
    void set_gkey_active(const std::wstring& key, bool on);
    void set_desired(const std::wstring& key, Desired d);

    std::shared_ptr<const Snapshot> snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hidpp
