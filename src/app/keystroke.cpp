#include "app/keystroke.h"

#include <cwchar>
#include <vector>

namespace app {
namespace {

// Tasten, die auf dem erweiterten Tastenblock liegen. Ohne KEYEVENTF_EXTENDEDKEY landet
// z.B. "Pos1" als Ziffernblock-7 statt als Pos1.
bool is_extended(uint16_t vk) {
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:
        case VK_LEFT:   case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
        case VK_LWIN:   case VK_RWIN:  case VK_APPS: case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

void fill(INPUT& in, uint16_t vk, bool up) {
    in = INPUT{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    // Scancode mitgeben: manche Spiele lesen per DirectInput und sehen nur den Scancode.
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
}

struct NamedKey {
    uint16_t vk;
    const wchar_t* storage;
    const wchar_t* display;
};

// Nur die Tasten, deren Name nicht aus dem Layout kommt oder deren Layoutname unbrauchbar
// waere. Alles andere loest key_name() ueber die Tastaturbelegung auf.
const NamedKey kNamed[] = {
    {VK_ESCAPE,  L"Esc",         L"Esc"},
    {VK_RETURN,  L"Enter",       L"Enter"},
    {VK_SPACE,   L"Space",       L"Leertaste"},
    {VK_TAB,     L"Tab",         L"Tab"},
    {VK_BACK,    L"Backspace",   L"Rücktaste"},
    {VK_INSERT,  L"Insert",      L"Einfg"},
    {VK_DELETE,  L"Delete",      L"Entf"},
    {VK_HOME,    L"Home",        L"Pos1"},
    {VK_END,     L"End",         L"Ende"},
    {VK_PRIOR,   L"PageUp",      L"Bild auf"},
    {VK_NEXT,    L"PageDown",    L"Bild ab"},
    {VK_LEFT,    L"Left",        L"Links"},
    {VK_RIGHT,   L"Right",       L"Rechts"},
    {VK_UP,      L"Up",          L"Hoch"},
    {VK_DOWN,    L"Down",        L"Runter"},
    {VK_SNAPSHOT,L"PrintScreen", L"Druck"},
    {VK_PAUSE,   L"Pause",       L"Pause"},
    {VK_APPS,    L"Menu",        L"Menü"},
};

const NamedKey* find_named(uint16_t vk) {
    for (const auto& n : kNamed) if (n.vk == vk) return &n;
    return nullptr;
}

std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool equals_ci(const std::wstring& a, const wchar_t* b) {
    return _wcsicmp(a.c_str(), b) == 0;
}

} // namespace

bool is_modifier(uint16_t vk) {
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU:    case VK_LMENU:    case VK_RMENU:
        case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
        case VK_LWIN:    case VK_RWIN:
            return true;
        default:
            return false;
    }
}

std::wstring key_name(uint16_t vk) {
    if (const NamedKey* n = find_named(vk)) return n->display;

    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8];
        swprintf(buf, 8, L"F%u", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        wchar_t buf[16];
        swprintf(buf, 16, L"Num %u", vk - VK_NUMPAD0);
        return buf;
    }

    // Buchstaben, Ziffern und die layoutabhaengigen Zeichen ueber die Tastaturbelegung:
    // auf einer deutschen Tastatur soll da nicht der US-Name stehen.
    const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (ch >= 0x20) {
        const wchar_t c = static_cast<wchar_t>(towupper(static_cast<wint_t>(ch)));
        return std::wstring(1, c);
    }

    wchar_t buf[16];
    swprintf(buf, 16, L"VK_%02X", vk);
    return buf;
}

bool send(const Keystroke& k) {
    if (k.empty()) return false;

    std::vector<INPUT> seq;
    seq.reserve(10);

    INPUT in{};
    if (k.ctrl)  { fill(in, VK_CONTROL, false); seq.push_back(in); }
    if (k.shift) { fill(in, VK_SHIFT,   false); seq.push_back(in); }
    if (k.alt)   { fill(in, VK_MENU,    false); seq.push_back(in); }
    if (k.win)   { fill(in, VK_LWIN,    false); seq.push_back(in); }

    fill(in, k.vk, false); seq.push_back(in);
    fill(in, k.vk, true);  seq.push_back(in);

    // Umgekehrte Reihenfolge beim Loslassen -- sonst bleibt im ungluecklichen Fall ein
    // Modifier gedrueckt haengen und das ganze System verhaelt sich falsch.
    if (k.win)   { fill(in, VK_LWIN,    true); seq.push_back(in); }
    if (k.alt)   { fill(in, VK_MENU,    true); seq.push_back(in); }
    if (k.shift) { fill(in, VK_SHIFT,   true); seq.push_back(in); }
    if (k.ctrl)  { fill(in, VK_CONTROL, true); seq.push_back(in); }

    const UINT sent = SendInput(static_cast<UINT>(seq.size()), seq.data(), sizeof(INPUT));
    return sent == seq.size();
}

std::wstring to_storage(const Keystroke& k) {
    if (k.empty()) return {};
    std::wstring s;
    if (k.ctrl)  s += L"Ctrl+";
    if (k.alt)   s += L"Alt+";
    if (k.shift) s += L"Shift+";
    if (k.win)   s += L"Win+";

    if (const NamedKey* n = find_named(k.vk)) { s += n->storage; return s; }
    if (k.vk >= VK_F1 && k.vk <= VK_F24) {
        wchar_t buf[8];
        swprintf(buf, 8, L"F%u", k.vk - VK_F1 + 1);
        s += buf;
        return s;
    }
    if (k.vk >= VK_NUMPAD0 && k.vk <= VK_NUMPAD9) {
        wchar_t buf[16];
        swprintf(buf, 16, L"Num%u", k.vk - VK_NUMPAD0);
        s += buf;
        return s;
    }
    if ((k.vk >= '0' && k.vk <= '9') || (k.vk >= 'A' && k.vk <= 'Z')) {
        s += static_cast<wchar_t>(k.vk);
        return s;
    }
    // Layoutabhaengige Zeichen als Code ablegen: der Name waere auf einem anderen
    // Tastaturlayout schlicht falsch.
    wchar_t buf[16];
    swprintf(buf, 16, L"VK_%02X", k.vk);
    s += buf;
    return s;
}

bool from_storage(const std::wstring& text, Keystroke* out) {
    *out = Keystroke{};
    if (trim(text).empty()) return false;

    std::wstring rest = trim(text);
    for (;;) {
        const size_t plus = rest.find(L'+');
        if (plus == std::wstring::npos) break;
        const std::wstring part = trim(rest.substr(0, plus));
        if      (equals_ci(part, L"Ctrl"))  out->ctrl = true;
        else if (equals_ci(part, L"Alt"))   out->alt = true;
        else if (equals_ci(part, L"Shift")) out->shift = true;
        else if (equals_ci(part, L"Win"))   out->win = true;
        else break;   // kein Modifier -- also schon die Haupttaste (etwa "+" selbst)
        rest = trim(rest.substr(plus + 1));
    }
    if (rest.empty()) return false;

    for (const auto& n : kNamed)
        if (equals_ci(rest, n.storage)) { out->vk = n.vk; return true; }

    if ((rest[0] == L'F' || rest[0] == L'f') && rest.size() >= 2 && iswdigit(rest[1])) {
        const int n = _wtoi(rest.c_str() + 1);
        if (n >= 1 && n <= 24) { out->vk = static_cast<uint16_t>(VK_F1 + n - 1); return true; }
    }
    if (rest.size() == 4 && _wcsnicmp(rest.c_str(), L"Num", 3) == 0 && iswdigit(rest[3])) {
        out->vk = static_cast<uint16_t>(VK_NUMPAD0 + (rest[3] - L'0'));
        return true;
    }
    if (rest.size() == 5 && _wcsnicmp(rest.c_str(), L"VK_", 3) == 0) {
        out->vk = static_cast<uint16_t>(wcstoul(rest.c_str() + 3, nullptr, 16));
        return out->vk != 0;
    }
    if (rest.size() == 1) {
        out->vk = static_cast<uint16_t>(towupper(rest[0]));
        return true;
    }
    return false;
}

std::wstring to_display(const Keystroke& k) {
    if (k.empty()) return L"—";
    std::wstring s;
    if (k.ctrl)  s += L"Strg+";
    if (k.alt)   s += L"Alt+";
    if (k.shift) s += L"Umschalt+";
    if (k.win)   s += L"Win+";
    s += key_name(k.vk);
    return s;
}

} // namespace app
