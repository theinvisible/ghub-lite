#include "ui/theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <cstring>

namespace ui {
namespace {

// Ab Win10 1903 ist das Attribut 20; davor lag dasselbe auf 19. Beides zu setzen kostet
// nichts -- ein unbekanntes Attribut quittiert DWM einfach mit einem Fehlercode.
constexpr DWORD kDwmDarkMode      = 20;
constexpr DWORD kDwmDarkModeOld   = 19;

enum PreferredAppMode { AppModeDefault = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };

using FnSetPreferredAppMode = PreferredAppMode (WINAPI*)(PreferredAppMode);
using FnAllowDarkModeForWindow = BOOL (WINAPI*)(HWND, BOOL);
using FnRefreshImmersiveColorPolicyState = void (WINAPI*)();
using FnFlushMenuThemes = void (WINAPI*)();

FnSetPreferredAppMode g_set_preferred_app_mode = nullptr;
FnAllowDarkModeForWindow g_allow_dark_for_window = nullptr;
FnRefreshImmersiveColorPolicyState g_refresh_policy = nullptr;
FnFlushMenuThemes g_flush_menu_themes = nullptr;

const Palette kDark{
    /*bg*/       RGB(32, 32, 32),
    /*panel*/    RGB(43, 43, 43),
    /*text*/     RGB(240, 240, 240),
    /*text_dim*/ RGB(155, 155, 155),
    /*accent*/   RGB(76, 154, 255),
    /*border*/   RGB(62, 62, 62),
    /*track*/    RGB(72, 72, 72),
    /*warning*/  RGB(255, 170, 60),
    /*disabled*/ RGB(110, 110, 110),
};

const Palette kLight{
    /*bg*/       RGB(243, 243, 243),
    /*panel*/    RGB(255, 255, 255),
    /*text*/     RGB(24, 24, 24),
    /*text_dim*/ RGB(100, 100, 100),
    /*accent*/   RGB(0, 95, 184),
    /*border*/   RGB(208, 208, 208),
    /*track*/    RGB(200, 200, 200),
    /*warning*/  RGB(176, 92, 0),
    /*disabled*/ RGB(150, 150, 150),
};

void theme_children(HWND parent, bool dark) {
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[64] = {};
        GetClassNameW(child, cls, 64);

        const wchar_t* sub = nullptr;
        if (_wcsicmp(cls, L"Button") == 0)              sub = dark ? L"DarkMode_Explorer" : nullptr;
        else if (_wcsicmp(cls, L"ComboBox") == 0)       sub = dark ? L"DarkMode_CFD" : nullptr;
        else if (_wcsicmp(cls, L"Edit") == 0)           sub = dark ? L"DarkMode_CFD" : nullptr;
        else if (_wcsicmp(cls, L"ListBox") == 0)        sub = dark ? L"DarkMode_Explorer" : nullptr;
        else if (_wcsicmp(cls, L"ComboLBox") == 0)      sub = dark ? L"DarkMode_CFD" : nullptr;

        if (g_allow_dark_for_window) g_allow_dark_for_window(child, dark ? TRUE : FALSE);
        SetWindowTheme(child, sub, nullptr);

        theme_children(child, dark);
        InvalidateRect(child, nullptr, TRUE);
    }
}

} // namespace

bool system_prefers_dark() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1, size = sizeof(value), type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                        reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_DWORD) return false;
    return value == 0;
}

bool is_color_scheme_change(LPARAM lparam) {
    const auto* s = reinterpret_cast<const wchar_t*>(lparam);
    return s != nullptr && _wcsicmp(s, L"ImmersiveColorSet") == 0;
}

void Theme::init_process() {
    // Die Ordinals sind undokumentiert. Fehlt einer, laeuft alles weiter -- dann bleiben
    // nur Menues und Scrollbalken hell.
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;

    g_refresh_policy = reinterpret_cast<FnRefreshImmersiveColorPolicyState>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(104))));
    g_allow_dark_for_window = reinterpret_cast<FnAllowDarkModeForWindow>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(133))));
    g_set_preferred_app_mode = reinterpret_cast<FnSetPreferredAppMode>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))));
    g_flush_menu_themes = reinterpret_cast<FnFlushMenuThemes>(
        reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))));

    if (g_set_preferred_app_mode) g_set_preferred_app_mode(AllowDark);
    if (g_refresh_policy) g_refresh_policy();
}

Theme::~Theme() {
    if (bg_brush_) DeleteObject(bg_brush_);
    if (panel_brush_) DeleteObject(panel_brush_);
}

void Theme::rebuild_brushes() {
    if (bg_brush_) DeleteObject(bg_brush_);
    if (panel_brush_) DeleteObject(panel_brush_);
    bg_brush_ = CreateSolidBrush(pal_.bg);
    panel_brush_ = CreateSolidBrush(pal_.panel);
}

bool Theme::refresh() {
    const bool want_dark = system_prefers_dark();
    if (bg_brush_ && want_dark == dark_) return false;
    dark_ = want_dark;
    pal_ = dark_ ? kDark : kLight;
    rebuild_brushes();
    if (g_refresh_policy) g_refresh_policy();
    if (g_flush_menu_themes) g_flush_menu_themes();
    return true;
}

void Theme::apply_frame(HWND hwnd) const {
    const BOOL on = dark_ ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmDarkMode, &on, sizeof(on))))
        DwmSetWindowAttribute(hwnd, kDwmDarkModeOld, &on, sizeof(on));
    if (g_allow_dark_for_window) g_allow_dark_for_window(hwnd, on);
}

void Theme::apply_controls(HWND parent) const {
    theme_children(parent, dark_);
    InvalidateRect(parent, nullptr, TRUE);
}

} // namespace ui
