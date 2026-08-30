#include "app/settings.h"

#include <windows.h>
#include <shlobj.h>

#include <cwchar>

namespace app {
namespace {

constexpr wchar_t kRunKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ghub-lite";
constexpr wchar_t kSectionApp[] = L"app";

std::wstring settings_directory() {
    wchar_t appdata[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, appdata)))
        return {};
    std::wstring dir = std::wstring(appdata) + L"\\ghub-lite";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring device_section(const std::wstring& key) {
    return L"device." + key;
}

std::vector<uint16_t> parse_presets(const std::wstring& text) {
    std::vector<uint16_t> out;
    size_t pos = 0;
    while (pos < text.size() && out.size() < 8) {
        const size_t comma = text.find(L',', pos);
        const std::wstring part = text.substr(pos, comma == std::wstring::npos ? comma : comma - pos);
        const int v = _wtoi(part.c_str());
        if (v > 0) out.push_back(static_cast<uint16_t>(v));
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return out;
}

} // namespace

std::wstring exe_path() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool read_autostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    const LSTATUS st = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool write_autostart(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;

    LSTATUS st;
    if (on) {
        // Mit --tray starten: beim Anmelden soll nur das Infobereich-Symbol erscheinen.
        const std::wstring cmd = L"\"" + exe_path() + L"\" --tray";
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

void Settings::load() {
    const std::wstring dir = settings_directory();
    if (dir.empty()) return;
    path_ = dir + L"\\settings.ini";

    minimize_to_tray_ = GetPrivateProfileIntW(kSectionApp, L"minimize_to_tray", 1, path_.c_str()) != 0;

    wchar_t buf[128] = {};
    GetPrivateProfileStringW(kSectionApp, L"presets", L"800,1600,3200,6400",
                             buf, 128, path_.c_str());
    if (auto p = parse_presets(buf); !p.empty()) presets_ = p;

    // Die Wahrheit ueber den Autostart steht in der Registry, nicht in der INI --
    // der Benutzer koennte den Eintrag ausserhalb entfernt haben.
    autostart_ = read_autostart();
}

void Settings::write_int(const wchar_t* section, const wchar_t* name, int value) const {
    if (path_.empty()) return;
    wchar_t buf[24];
    swprintf(buf, 24, L"%d", value);
    WritePrivateProfileStringW(section, name, buf, path_.c_str());
}

void Settings::set_autostart(bool on) {
    if (!write_autostart(on)) return;
    autostart_ = on;
    write_int(kSectionApp, L"autostart", on ? 1 : 0);
}

void Settings::set_minimize_to_tray(bool on) {
    minimize_to_tray_ = on;
    write_int(kSectionApp, L"minimize_to_tray", on ? 1 : 0);
}

DeviceSettings Settings::device(const std::wstring& key) const {
    DeviceSettings s;
    if (path_.empty() || key.empty()) return s;
    const std::wstring section = device_section(key);
    s.dpi     = static_cast<uint16_t>(GetPrivateProfileIntW(section.c_str(), L"dpi", 0, path_.c_str()));
    s.rate_hz = static_cast<uint16_t>(GetPrivateProfileIntW(section.c_str(), L"rate", 0, path_.c_str()));
    s.gkeys_active = GetPrivateProfileIntW(section.c_str(), L"gkeys_active", 0, path_.c_str()) != 0;
    s.host_mode    = GetPrivateProfileIntW(section.c_str(), L"host_mode", 0, path_.c_str()) != 0;

    s.gkeys.resize(kMaxGKeys);
    for (size_t i = 0; i < kMaxGKeys; ++i) {
        wchar_t name[16];
        swprintf(name, 16, L"gkey%zu", i + 1);
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(section.c_str(), name, L"", buf, 64, path_.c_str());
        s.gkeys[i] = buf;
    }
    return s;
}

void Settings::set_device(const std::wstring& key, DeviceSettings s) {
    if (path_.empty() || key.empty()) return;
    const std::wstring section = device_section(key);
    if (s.dpi)     write_int(section.c_str(), L"dpi", s.dpi);
    if (s.rate_hz) write_int(section.c_str(), L"rate", s.rate_hz);
    write_int(section.c_str(), L"gkeys_active", s.gkeys_active ? 1 : 0);
    write_int(section.c_str(), L"host_mode", s.host_mode ? 1 : 0);

    for (size_t i = 0; i < s.gkeys.size() && i < kMaxGKeys; ++i) {
        wchar_t name[16];
        swprintf(name, 16, L"gkey%zu", i + 1);
        // Leere Belegung als leeren Wert schreiben, nicht weglassen -- sonst bliebe eine
        // frueher gesetzte Kombination beim naechsten Laden stehen.
        WritePrivateProfileStringW(section.c_str(), name,
                                   s.gkeys[i].empty() ? nullptr : s.gkeys[i].c_str(),
                                   path_.c_str());
    }
}

} // namespace app
