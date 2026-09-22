// Einstellungen in %APPDATA%\ghub-lite\settings.ini.
//
// Bewusst eine INI statt JSON: die Win32-Profil-API erledigt das ohne eine einzige
// zusaetzliche Abhaengigkeit, und die Datei bleibt von Hand editierbar.
//
// Der Schluessel pro Geraet ist die Unit-ID aus HID++ 0x0003, nicht der Name -- so bleiben
// mehrere baugleiche Maeuse unterscheidbar.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

// Mehr G-Tasten hat kein Geraet, das hier in Frage kommt (G815: 5).
inline constexpr size_t kMaxGKeys = 8;

struct DeviceSettings {
    uint16_t dpi     = 0;   // 0 = nichts gespeichert
    uint16_t rate_hz = 0;

    // Belegung der G-Tasten, Index 0 = G1. Leerer Eintrag = nicht belegt. Abgelegt wird
    // die sprachneutrale Schreibweise aus app::to_storage().
    std::vector<std::wstring> gkeys;
    bool gkeys_active = false;

    // Host-Modus (0x8100) beim Start wieder herstellen -- ohne ihn verweigert das Geraet
    // die Signalrate. Wird nur gesetzt, wenn ghub-lite ihn selbst gebraucht hat.
    bool host_mode = false;

    std::wstring gkey(size_t index) const {
        return index < gkeys.size() ? gkeys[index] : std::wstring();
    }
};

class Settings {
public:
    void load();

    const std::wstring& path() const { return path_; }

    bool autostart() const { return autostart_; }
    void set_autostart(bool on);

    bool minimize_to_tray() const { return minimize_to_tray_; }
    void set_minimize_to_tray(bool on);

    const std::vector<uint16_t>& presets() const { return presets_; }

    DeviceSettings device(const std::wstring& key) const;
    void set_device(const std::wstring& key, DeviceSettings s);

private:
    void write_int(const wchar_t* section, const wchar_t* name, int value) const;

    std::wstring path_;
    bool autostart_ = false;
    bool minimize_to_tray_ = true;
    std::vector<uint16_t> presets_{800, 1600, 3200, 6400};
};

// Autostart laeuft ueber HKCU\...\CurrentVersion\Run -- geschrieben wird nur auf
// ausdruecklichen Wunsch, nie beiläufig beim Start.
bool read_autostart();

// exe: welches Programm beim Anmelden startet. Der Installer uebergibt den installierten Pfad
// statt seines eigenen; Name und Format des Eintrags bleiben so an einer Stelle.
std::wstring exe_path();
bool write_autostart(bool on, const std::wstring& exe = exe_path());

} // namespace app
