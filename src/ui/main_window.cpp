#include "ui/main_window.h"

#include "app/ipc.h"
#include "app/keystroke.h"
#include "ui/dpi_panel.h"

#include <windowsx.h>
#include <dbt.h>
#include <shellapi.h>

extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <cwchar>

namespace ui {
namespace {

constexpr const wchar_t* kClassName = app::kWindowClass;
constexpr wchar_t kWindowTitle[] = L"ghub-lite";

// Nachrichten
constexpr UINT WM_APP_SNAPSHOT = WM_APP + 1;
constexpr UINT WM_APP_TRAY     = WM_APP + 2;
constexpr UINT WM_APP_GKEY     = WM_APP + 3;

// Control-IDs
enum : int {
    IDC_COMBO      = 100,
    IDC_SLIDER     = 101,
    IDC_EDIT       = 102,
    IDC_AUTOSTART  = 103,
    IDC_TRAY       = 104,
    IDC_GKEYS_ON   = 105,
    IDC_PRESET     = 110,   // .. 117
    IDC_RATE       = 130,   // .. 137
    IDC_GKEY_SET   = 150,   // .. 157
    IDC_GKEY_CLR   = 160,   // .. 167
};

// Menuebefehle im Infobereich
enum : int {
    IDM_SHOW = app::kCmdShow,   // auch von aussen gepostet, siehe app/ipc.h
    IDM_EXIT = app::kCmdExit,
    IDM_PRESET = 210,   // .. 217
    IDM_RATE   = 230,   // .. 237
};

// Fenstergroesse in DIP. Die Hoehe haengt davon ab, welches Panel sichtbar ist --
// siehe design_height().
constexpr int kWinW = 470;
constexpr int kWinHMouse = 382;
constexpr int kWinHBase  = 300;   // alles ausser den G-Tasten-Zeilen
constexpr int kGKeyRowH  = 26;
constexpr int kGKeyGap   = 6;
constexpr int kPad       = 16;

constexpr UINT_PTR kDeviceChangeTimer = 1;

HWND make_static(HWND parent, HINSTANCE inst, const wchar_t* text, DWORD extra = 0) {
    return CreateWindowExW(0, L"Static", text, WS_CHILD | WS_VISIBLE | extra,
                           0, 0, 10, 10, parent, nullptr, inst, nullptr);
}

// Texte nur bei echter Aenderung setzen: die Schnappschuesse kommen alle paar Sekunden,
// und ein SetWindowText zeichnet das Control auch dann neu, wenn sich nichts geaendert hat.
void set_text(HWND h, const std::wstring& text) {
    wchar_t current[256] = {};
    GetWindowTextW(h, current, 256);
    if (text != current) SetWindowTextW(h, text.c_str());
}

} // namespace

bool MainWindow::register_class(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // wir malen selbst, sonst blitzt es beim Themenwechsel
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    return RegisterClassExW(&wc) != 0;
}

bool MainWindow::create(HINSTANCE inst, bool start_hidden) {
    inst_ = inst;
    settings_.load();
    theme_.refresh();

    hwnd_ = CreateWindowExW(0, kClassName, kWindowTitle,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    dpi_ = GetDpiForWindow(hwnd_);
    rebuild_fonts();
    build_controls();
    resize_to_design(nullptr);
    apply_theme();

    icon_ = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON), 0));
    if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    tray_.add(hwnd_, WM_APP_TRAY, icon_, kWindowTitle);

    manager_.start(hwnd_, WM_APP_SNAPSHOT, WM_APP_GKEY);

    if (!start_hidden) {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
    return true;
}

// Das Fenster hat eine feste Entwurfsgroesse in DIP. Die Groesse wird deshalb immer aus
// dip(kWinW) und design_height() neu berechnet und nie aus dem alten Rahmen skaliert -- sonst laufen
// Fenstergroesse und Layout auseinander, sobald sich die DPI aendert.
void MainWindow::resize_to_design(const RECT* suggested) {
    RECT want{0, 0, dip(kWinW), dip(design_height())};
    AdjustWindowRectExForDpi(&want, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)),
                             FALSE, 0, dpi_);
    const int w = want.right - want.left;
    const int h = want.bottom - want.top;

    // Passt schon: nicht anfassen. layout() laeuft bei jedem Schnappschuss, und ein
    // SetWindowPos mit unveraenderter Groesse kostet trotzdem ein Neuzeichnen.
    RECT have{};
    GetWindowRect(hwnd_, &have);
    if (!suggested && (have.right - have.left) == w && (have.bottom - have.top) == h) return;

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    int x = 0, y = 0;
    if (suggested) { x = suggested->left; y = suggested->top; }
    else flags |= SWP_NOMOVE;

    SetWindowPos(hwnd_, nullptr, x, y, w, h, flags);
    layout();
}

void MainWindow::rebuild_fonts() {
    if (font_) DeleteObject(font_);
    if (font_head_) DeleteObject(font_head_);

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9, static_cast<int>(dpi_), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    font_ = CreateFontIndirectW(&lf);

    lf.lfWeight = FW_SEMIBOLD;
    font_head_ = CreateFontIndirectW(&lf);
}

void MainWindow::build_controls() {
    combo_ = CreateWindowExW(0, L"ComboBox", nullptr,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                             0, 0, 10, 200, hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_COMBO)),
                             inst_, nullptr);
    detail_ = make_static(hwnd_, inst_, L"");

    head_dpi_ = make_static(hwnd_, inst_, L"Auflösung");
    slider_ = CreateWindowExW(0, kSliderClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              0, 0, 10, 10, hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SLIDER)),
                              inst_, nullptr);
    SendMessageW(slider_, SLM_SETTHEME, 0, reinterpret_cast<LPARAM>(&theme_));

    edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                            0, 0, 10, 10, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT)),
                            inst_, nullptr);
    SendMessageW(edit_, EM_SETLIMITTEXT, 5, 0);
    edit_unit_ = make_static(hwnd_, inst_, L"DPI");

    for (int i = 0; i < 8; ++i) {
        preset_[i] = CreateWindowExW(0, L"Button", L"",
                                     WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                     0, 0, 10, 10, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET + i)),
                                     inst_, nullptr);
    }
    range_ = make_static(hwnd_, inst_, L"");

    head_rate_ = make_static(hwnd_, inst_, L"Signalrate");
    rates_.create(hwnd_, inst_, IDC_RATE);

    head_gkeys_ = make_static(hwnd_, inst_, L"G-Tasten");
    gkeys_.create(hwnd_, inst_, IDC_GKEY_SET, IDC_GKEY_CLR);
    gkey_hint_ = make_static(hwnd_, inst_, L"", SS_ENDELLIPSIS);
    chk_gkeys_ = CreateWindowExW(0, L"Button", L"G-Tasten von ghub-lite auswerten",
                                 WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_GKEYS_ON)),
                                 inst_, nullptr);

    chk_autostart_ = CreateWindowExW(0, L"Button", L"Beim Anmelden starten",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     0, 0, 10, 10, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AUTOSTART)),
                                     inst_, nullptr);
    chk_tray_ = CreateWindowExW(0, L"Button", L"Beim Schließen in den Infobereich",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                0, 0, 10, 10, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TRAY)),
                                inst_, nullptr);
    Button_SetCheck(chk_autostart_, settings_.autostart() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(chk_tray_, settings_.minimize_to_tray() ? BST_CHECKED : BST_UNCHECKED);

    status_ = make_static(hwnd_, inst_, L"Suche Geräte …", SS_ENDELLIPSIS);

    // Schnellwahlknoepfe aus der INI beschriften.
    const auto& presets = settings_.presets();
    for (size_t i = 0; i < 8; ++i) {
        if (i < presets.size()) {
            wchar_t label[16];
            swprintf(label, 16, L"%u", presets[i]);
            SetWindowTextW(preset_[i], label);
            ShowWindow(preset_[i], SW_SHOW);
        } else {
            ShowWindow(preset_[i], SW_HIDE);
        }
    }

    apply_fonts();
}

void MainWindow::apply_fonts() {
    HWND all[] = {combo_, detail_, head_dpi_, edit_, edit_unit_, range_, head_rate_,
                  head_gkeys_, gkey_hint_, chk_gkeys_, chk_autostart_, chk_tray_, status_};
    for (HWND h : all) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    for (HWND h : preset_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    rates_.set_font(font_);
    gkeys_.set_font(font_);
    SendMessageW(head_dpi_, WM_SETFONT, reinterpret_cast<WPARAM>(font_head_), TRUE);
    SendMessageW(head_rate_, WM_SETFONT, reinterpret_cast<WPARAM>(font_head_), TRUE);
    SendMessageW(head_gkeys_, WM_SETFONT, reinterpret_cast<WPARAM>(font_head_), TRUE);
}

// Welches Panel ist sichtbar? Danach richtet sich die Fensterhoehe.
int MainWindow::design_height() const {
    const hidpp::DeviceState* m = current();
    if (m && m->kind == hidpp::DeviceKind::Keyboard)
        return kWinHBase + gkeys_.height(kGKeyRowH, kGKeyGap);
    return kWinHMouse;
}

void MainWindow::show_mouse_panel(bool on) {
    const int cmd = on ? SW_SHOW : SW_HIDE;
    for (HWND h : {head_dpi_, slider_, edit_, edit_unit_, range_, head_rate_}) ShowWindow(h, cmd);

    // Die Schnellwahlknoepfe richten sich zusaetzlich nach der Anzahl in der INI --
    // unbelegte bleiben auch dann versteckt, wenn das Maus-Panel sichtbar ist.
    const size_t count = settings_.presets().size();
    for (size_t i = 0; i < 8; ++i)
        ShowWindow(preset_[i], (on && i < count) ? SW_SHOW : SW_HIDE);

    if (!on) rates_.set_rates({}, 0);
}

void MainWindow::show_keyboard_panel(bool on) {
    const int cmd = on ? SW_SHOW : SW_HIDE;
    for (HWND h : {head_gkeys_, gkey_hint_, chk_gkeys_}) ShowWindow(h, cmd);
    gkeys_.show(on);
}

void MainWindow::layout() {
    // Die tatsaechliche DPI hier nachziehen: beim Anlegen des Fensters steht sie noch nicht
    // zuverlaessig fest (das Fenster haengt dann noch an keinem Monitor), und ein
    // WM_DPICHANGED kommt in dem Fall gar nicht. Ohne das laufen Fenstergroesse und
    // Layout auseinander -- das Fenster ist fuer 96 dpi bemessen, gezeichnet wird fuer 120.
    if (!laying_out_) {
        const UINT real = GetDpiForWindow(hwnd_);
        if (real && real != dpi_) {
            dpi_ = real;
            rebuild_fonts();
            apply_fonts();
        }
        laying_out_ = true;
        resize_to_design(nullptr);
        laying_out_ = false;
    }

    const int pad = dip(kPad);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - 2 * pad;
    const int line = dip(20);
    const int ctl = dip(26);

    int y = pad;
    MoveWindow(combo_, pad, y, w, dip(200), TRUE);
    y += ctl + dip(6);
    MoveWindow(detail_, pad, y, w, line, TRUE);
    y += line + dip(16);

    const hidpp::DeviceState* m = current();
    const bool keyboard = m && m->kind == hidpp::DeviceKind::Keyboard;

    if (keyboard) {
        MoveWindow(head_gkeys_, pad, y, w, line, TRUE);
        y += line + dip(8);
        gkeys_.layout(pad, y, w, dip(kGKeyRowH), dip(kGKeyGap));
        y += gkeys_.height(dip(kGKeyRowH), dip(kGKeyGap)) + dip(10);
        MoveWindow(gkey_hint_, pad, y, w, line, TRUE);
        y += line + dip(10);
        MoveWindow(chk_gkeys_, pad, y, w, dip(22), TRUE);
        y += dip(30);
    } else {
        MoveWindow(head_dpi_, pad, y, w, line, TRUE);
        y += line + dip(8);

        const int edit_w = dip(62);
        const int unit_w = dip(30);
        const int slider_w = w - edit_w - unit_w - dip(12);
        MoveWindow(slider_, pad, y - dip(2), slider_w, dip(28), TRUE);
        MoveWindow(edit_, pad + slider_w + dip(8), y, edit_w, dip(23), TRUE);
        MoveWindow(edit_unit_, pad + slider_w + dip(12) + edit_w, y + dip(3), unit_w, line, TRUE);
        y += dip(34);

        const int pw = dip(62), gap = dip(8);
        for (int i = 0; i < 8; ++i) MoveWindow(preset_[i], pad + i * (pw + gap), y, pw, ctl, TRUE);
        y += ctl + dip(8);
        MoveWindow(range_, pad, y, w, line, TRUE);
        y += line + dip(16);

        MoveWindow(head_rate_, pad, y, w, line, TRUE);
        y += line + dip(8);
        rates_.layout(pad, y, dip(92), ctl, dip(4));
        y += ctl + dip(18);
    }

    MoveWindow(chk_autostart_, pad, y, w, dip(22), TRUE);
    y += dip(26);
    MoveWindow(chk_tray_, pad, y, w, dip(22), TRUE);
    y += dip(30);
    MoveWindow(status_, pad, y, w, line, TRUE);

    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::apply_theme() {
    theme_.apply_frame(hwnd_);
    theme_.apply_controls(hwnd_);
    SendMessageW(slider_, SLM_SETTHEME, 0, reinterpret_cast<LPARAM>(&theme_));
    InvalidateRect(hwnd_, nullptr, TRUE);
}

const hidpp::DeviceState* MainWindow::current() const {
    if (!snap_ || current_key_.empty()) return nullptr;
    return snap_->find(current_key_);
}

void MainWindow::set_status(const std::wstring& text, bool error) {
    status_error_ = error;
    set_text(status_, text);
    InvalidateRect(status_, nullptr, TRUE);
}

void MainWindow::update_device_list(const hidpp::Snapshot& snap) {
    // Nur neu aufbauen, wenn sich die Liste wirklich geaendert hat -- sonst klappt die
    // Combobox bei jedem Tick zu.
    const int have = ComboBox_GetCount(combo_);
    bool same = have == static_cast<int>(snap.devices.size());
    if (same) {
        for (size_t i = 0; i < snap.devices.size(); ++i) {
            const auto* stored = reinterpret_cast<const wchar_t*>(
                ComboBox_GetItemData(combo_, static_cast<int>(i)));
            if (!stored || snap.devices[i].key != stored) { same = false; break; }
        }
    }
    if (same) return;

    for (int i = 0; i < have; ++i)
        delete[] reinterpret_cast<wchar_t*>(ComboBox_GetItemData(combo_, i));
    ComboBox_ResetContent(combo_);

    for (const auto& m : snap.devices) {
        const std::wstring label = m.info.name.empty() ? L"Logitech-Maus" : m.info.name;
        const int idx = ComboBox_AddString(combo_, label.c_str());
        auto* key = new wchar_t[m.key.size() + 1];
        wcscpy_s(key, m.key.size() + 1, m.key.c_str());
        ComboBox_SetItemData(combo_, idx, reinterpret_cast<LPARAM>(key));
    }

    int select = 0;
    for (size_t i = 0; i < snap.devices.size(); ++i)
        if (snap.devices[i].key == current_key_) select = static_cast<int>(i);
    if (!snap.devices.empty()) {
        ComboBox_SetCurSel(combo_, select);
        current_key_ = snap.devices[static_cast<size_t>(select)].key;
    } else {
        current_key_.clear();
    }
}

void MainWindow::update_device_view(const hidpp::Snapshot& snap) {
    const hidpp::DeviceState* m = current();
    updating_ = true;

    if (!m) {
        set_text(detail_, snap.ghub_running ? L"Kein Gerät — läuft noch G HUB?"
                                            : L"Kein unterstütztes Gerät gefunden.");
        set_text(range_, L"");
        set_text(edit_, L"");
        EnableWindow(slider_, FALSE);
        EnableWindow(edit_, FALSE);
        for (HWND b : preset_) EnableWindow(b, FALSE);
        rates_.enable(false);
        show_keyboard_panel(false);
        show_mouse_panel(true);
        updating_ = false;
        return;
    }

    const bool keyboard = m->kind == hidpp::DeviceKind::Keyboard;
    show_mouse_panel(!keyboard);
    show_keyboard_panel(keyboard);

    // Kopfzeile: Verbindung, Betriebsmodus, Akku.
    std::wstring detail = m->connected ? L"verbunden" : L"nicht verbunden";
    detail += L"  ·  ";
    detail += hidpp::onboard_mode_text(m->info.onboard);
    if (m->info.has_battery) {
        wchar_t buf[64];
        swprintf(buf, 64, L"  ·  Akku %u %%", m->info.battery_percent);
        detail += buf;
        // Der Ladezustand kommt nur dazu, wenn er etwas aussagt -- "Akku 84 % (Akku)"
        // waere nur Rauschen.
        if (m->info.charge != hidpp::ChargeState::Discharging) {
            detail += L" (";
            detail += hidpp::charge_state_text(m->info.charge);
            detail += L")";
        }
    }
    set_text(detail_, detail);

    const bool live = m->connected;

    if (keyboard) {
        gkeys_.set_key_count(m->gkeys.count);
        gkeys_.set_enabled(live);
        load_gkey_bindings();

        const auto ds = settings_.device(m->key);
        Button_SetCheck(chk_gkeys_, ds.gkeys_active ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(chk_gkeys_, live ? TRUE : FALSE);

        set_text(gkey_hint_,
                 snap.ghub_running
                     ? L"G HUB läuft und greift auf dieselben Tasten zu — besser beenden."
                     : L"Die Tasten wirken nur, solange ghub-lite läuft.");

        gkeys_.show(true);
        layout();

        wchar_t tip[128];
        swprintf(tip, 128, L"ghub-lite — %s\n%u G-Tasten", m->info.name.c_str(), m->gkeys.count);
        tray_.set_tip(tip);

        updating_ = false;
        return;
    }

    EnableWindow(slider_, live && m->dpi.valid);
    EnableWindow(edit_, live && m->dpi.valid);
    rates_.enable(live && m->rate.valid);

    if (m->dpi.valid) {
        SendMessageW(slider_, SLM_SETRANGE, m->dpi.min(), m->dpi.max());
        // Schrittweite des ersten Bereichs als Raster; feiner braucht es die UI nicht.
        uint16_t step = 1;
        for (const auto& s : m->dpi.segments) if (s.step) { step = s.step; break; }
        SendMessageW(slider_, SLM_SETSTEP, step, 0);
        set_dpi_fields(m->dpi.current);

        std::wstring info = m->dpi.describe();
        if (m->dpi.dflt) {
            wchar_t buf[48];
            swprintf(buf, 48, L"  ·  Standard %u", m->dpi.dflt);
            info += buf;
        }
        set_text(range_, info);
    } else {
        set_text(range_, L"Dieses Gerät bietet keine einstellbare Auflösung.");
        SetWindowTextW(edit_, L"");
    }

    const auto& presets = settings_.presets();
    for (size_t i = 0; i < 8; ++i)
        EnableWindow(preset_[i], live && m->dpi.valid && i < presets.size());

    rates_.set_rates(m->rate.rates_hz, m->rate.current_hz);
    layout();

    // Tooltip im Infobereich auf den aktuellen Stand bringen.
    wchar_t tip[128];
    swprintf(tip, 128, L"ghub-lite — %s\n%u DPI · %u Hz",
             m->info.name.c_str(), m->dpi.current, m->rate.current_hz);
    tray_.set_tip(tip);

    updating_ = false;
}

// Gespeicherte Werte an den Manager reichen, sobald ein Geraet zum ersten Mal auftaucht.
// Ohne das wuerde beim Programmstart nichts wiederhergestellt -- die Einstellungen waeren
// zwar geladen, kaemen aber nie beim Geraet an.
void MainWindow::prime_desired(const hidpp::Snapshot& snap) {
    for (const auto& d : snap.devices) {
        if (!d.connected || d.key.empty() || d.key[0] == L'?') continue;
        if (!primed_.insert(d.key).second) continue;

        const auto ds = settings_.device(d.key);
        const bool gkeys = ds.gkeys_active && d.gkeys.valid;
        if (!ds.dpi && !ds.rate_hz && !gkeys && !ds.host_mode) continue;

        manager_.set_desired(d.key, hidpp::Desired{ds.dpi, ds.rate_hz, gkeys, ds.host_mode});
    }
}

// Hat der Manager von sich aus in den Host-Modus geschaltet (weil das Geraet die
// Signalrate sonst verweigert), muss das in die Einstellungen -- sonst waere es nach dem
// naechsten Start vergessen und die Rate ginge wieder nicht.
void MainWindow::sync_host_mode(const hidpp::Snapshot& snap) {
    for (const auto& d : snap.devices) {
        // Bewusst ohne connected-Pruefung: hier wird nur ein Merker gespeichert, und ein
        // Funkgeraet kann zwischen zwei Ticks als getrennt gelten, obwohl der Befehl eben
        // noch durchging.
        if (d.key.empty() || d.key[0] == L'?') continue;
        auto ds = settings_.device(d.key);
        if (ds.host_mode == d.host_mode_forced) continue;
        ds.host_mode = d.host_mode_forced;
        settings_.set_device(d.key, ds);
    }
}

void MainWindow::on_snapshot() {
    snap_ = manager_.snapshot();
    if (!snap_) return;

    // Wunschwerte muessen auch im Infobereich durchgereicht werden -- das ist die
    // Wiederherstellung nach Reconnect und haengt nicht am Fenster.
    prime_desired(*snap_);
    sync_host_mode(*snap_);

    // Liegt ghub-lite im Tray, sieht niemand hin: dann kein Neuaufbau der Geraeteliste,
    // keine dreissig MoveWindow-Aufrufe. Beim Wiederanzeigen holt toggle_window() das nach.
    if (!IsWindowVisible(hwnd_)) return;

    update_device_list(*snap_);
    update_device_view(*snap_);

    // Nur uebernehmen, wenn der Manager etwas Neues zu sagen hat. Sonst wuerde jeder Tick
    // dieselbe alte Meldung erneut setzen und dabei kurzlebige Rueckmeldungen wie
    // "G1 → Strg+Umschalt+F13" nach wenigen Sekunden wegwischen.
    if (!snap_->status.empty()) {
        if (snap_->status != last_manager_status_) {
            last_manager_status_ = snap_->status;
            set_status(snap_->status, snap_->status_is_error);
        }
    } else if (snap_->ghub_running) {
        set_status(L"Hinweis: G HUB läuft und setzt Werte womöglich zurück.", true);
    }
}

void MainWindow::set_dpi_fields(int value) {
    SendMessageW(slider_, SLM_SETVALUE, static_cast<WPARAM>(value), 0);
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", value);
    SetWindowTextW(edit_, buf);
}

int MainWindow::read_dpi_field() const {
    wchar_t buf[16] = {};
    GetWindowTextW(edit_, buf, 16);
    return _wtoi(buf);
}

void MainWindow::commit_dpi(int value) {
    const hidpp::DeviceState* m = current();
    if (!m || !m->dpi.valid || value <= 0) return;
    const uint16_t snapped = m->dpi.snap(value);
    set_dpi_fields(snapped);

    // Kein set_desired hier: der Manager traegt den gesetzten Wert selbst in den
    // Wunschzustand ein. Ein set_desired wuerde die ganze Struktur ersetzen und dabei
    // Dinge ueberschreiben, die nur der Manager kennt -- etwa einen gerade erzwungenen
    // Host-Modus. set_desired ist ausschliesslich fuer prime_desired() beim Start da.
    manager_.set_dpi(current_key_, snapped);
    auto ds = settings_.device(current_key_);
    ds.dpi = snapped;
    settings_.set_device(current_key_, ds);
}

void MainWindow::commit_rate(uint16_t hz) {
    if (!current() || hz == 0) return;
    manager_.set_rate(current_key_, hz);
    auto ds = settings_.device(current_key_);
    ds.rate_hz = hz;
    settings_.set_device(current_key_, ds);
}

void MainWindow::load_gkey_bindings() {
    const hidpp::DeviceState* m = current();
    if (!m) return;
    const auto ds = settings_.device(m->key);
    for (uint8_t g = 1; g <= m->gkeys.count && g <= app::kMaxGKeys; ++g) {
        app::Keystroke k;
        const std::wstring stored = ds.gkey(g - 1);
        gkeys_.set_binding(g, app::from_storage(stored, &k) ? app::to_display(k) : L"");
    }
}

// Kommt vom Verteiler-Thread des Managers ueber PostMessage -- hier laeuft es auf dem
// UI-Thread, wo auch die Belegungstabelle liegt.
void MainWindow::on_gkey_pressed(uint8_t gkey, uint32_t unit_id) {
    // Nach Unit-ID suchen, nicht nach der Auswahl in der Combobox: die Tasten sollen auch
    // wirken, wenn gerade die Maus ausgewaehlt ist oder das Fenster im Tray liegt.
    wchar_t key[16];
    swprintf(key, 16, L"%08X", unit_id);

    const auto ds = settings_.device(key);
    if (!ds.gkeys_active) return;

    wchar_t msg[160];
    app::Keystroke k;
    if (!app::from_storage(ds.gkey(gkey - 1), &k)) {
        swprintf(msg, 160, L"G%u ist nicht belegt.", gkey);
        set_status(msg, false);
        return;
    }

    // Rueckmeldung in der Statuszeile: sonst laesst sich nicht unterscheiden, ob die Taste
    // nicht ankam oder die Kombination beim Ziel nichts bewirkt.
    const bool ok = app::send(k);
    swprintf(msg, 160, ok ? L"G%u → %s" : L"G%u → %s (Senden fehlgeschlagen)",
             gkey, app::to_display(k).c_str());
    set_status(msg, !ok);
}

void MainWindow::on_gkey_captured(uint8_t gkey) {
    const hidpp::DeviceState* m = current();
    if (!m || gkey == 0 || gkey > app::kMaxGKeys) return;

    auto ds = settings_.device(m->key);
    ds.gkeys.resize(app::kMaxGKeys);
    ds.gkeys[gkey - 1] = app::to_storage(gkeys_.captured());
    settings_.set_device(m->key, ds);

    wchar_t msg[128];
    if (ds.gkeys[gkey - 1].empty())
        swprintf(msg, 128, L"G%u gelöscht.", gkey);
    else
        swprintf(msg, 128, L"G%u belegt mit %s.", gkey, app::to_display(gkeys_.captured()).c_str());
    set_status(msg, false);

    // Ohne aktiven Software-Modus wuerde die frische Belegung stillschweigend nichts tun.
    if (!ds.gkeys_active && !ds.gkeys[gkey - 1].empty())
        set_status(std::wstring(msg) + L" Noch nicht aktiv — Haken unten setzen.", true);
}

void MainWindow::toggle_window(bool show) {
    if (show) {
        ShowWindow(hwnd_, SW_SHOW);
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        // Waehrend das Fenster im Tray lag, wurde die Anzeige nicht mitgefuehrt -- jetzt
        // einmal aus dem letzten Schnappschuss nachziehen, damit nichts Veraltetes steht.
        if (snap_) {
            update_device_list(*snap_);
            update_device_view(*snap_);
        }
        manager_.refresh(true);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void MainWindow::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    const hidpp::DeviceState* m = current();

    if (m) {
        AppendMenuW(menu, MF_STRING | MF_DISABLED,
                    0, m->info.name.empty() ? L"Logitech-Maus" : m->info.name.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        const auto& presets = settings_.presets();
        for (size_t i = 0; i < presets.size(); ++i) {
            wchar_t label[32];
            swprintf(label, 32, L"%u DPI", presets[i]);
            const UINT flags = MF_STRING |
                               (m->dpi.valid && m->dpi.current == presets[i] ? MF_CHECKED : 0) |
                               (m->connected && m->dpi.valid ? 0u : MF_GRAYED);
            AppendMenuW(menu, flags, static_cast<UINT_PTR>(IDM_PRESET + i), label);
        }

        if (!m->rate.rates_hz.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            auto sorted = m->rate.rates_hz;
            std::sort(sorted.begin(), sorted.end(), std::greater<uint16_t>());
            for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
                wchar_t label[32];
                swprintf(label, 32, L"%u Hz", sorted[i]);
                const UINT flags = MF_STRING |
                                   (m->rate.current_hz == sorted[i] ? MF_CHECKED : 0) |
                                   (m->connected ? 0u : MF_GRAYED);
                AppendMenuW(menu, flags, static_cast<UINT_PTR>(IDM_RATE + i), label);
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Fenster anzeigen");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Beenden");

    POINT pt{};
    GetCursorPos(&pt);
    // Ohne SetForegroundWindow bleibt das Menue offen stehen, wenn man daneben klickt.
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Gemeinsamer Ausstieg fuer WM_DESTROY und WM_ENDSESSION. Mehrfach aufrufbar: alle drei
// Schritte tun beim zweiten Mal nichts mehr.
void MainWindow::shutdown_devices() {
    gkeys_.cancel_capture();   // Hook nie haengen lassen
    manager_.stop();
    tray_.remove();
}

void MainWindow::paint(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    FillRect(dc, &rc, theme_.bg_brush());

    // Trennlinien zwischen den Abschnitten: an den Kopfzeilen ausgerichtet.
    HPEN pen = CreatePen(PS_SOLID, 1, theme_.colors().border);
    HGDIOBJ old = SelectObject(dc, pen);
    const int pad = dip(kPad);
    // Nur ueber sichtbaren Kopfzeilen: die des anderen Panels stehen noch an ihrer alten
    // Stelle und wuerden sonst eine Linie mitten durch die Liste ziehen.
    for (HWND head : {head_dpi_, head_rate_, head_gkeys_}) {
        if (!IsWindowVisible(head)) continue;
        RECT hr{};
        GetWindowRect(head, &hr);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&hr), 2);
        const int y = hr.top - dip(9);
        MoveToEx(dc, pad, y, nullptr);
        LineTo(dc, rc.right - pad, y);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

LRESULT CALLBACK MainWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            // Auf An-/Abstecken von HID-Geraeten lauschen.
            GUID hid_guid{};
            HidD_GetHidGuid(&hid_guid);
            DEV_BROADCAST_DEVICEINTERFACE_W filter{};
            filter.dbcc_size = sizeof(filter);
            filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            filter.dbcc_classguid = hid_guid;
            RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wp);
            const HWND ctl = reinterpret_cast<HWND>(lp);
            SetBkMode(dc, TRANSPARENT);
            if (ctl == status_)
                SetTextColor(dc, status_error_ ? theme_.colors().warning : theme_.colors().text_dim);
            else if (ctl == detail_ || ctl == range_ || ctl == edit_unit_)
                SetTextColor(dc, theme_.colors().text_dim);
            else
                SetTextColor(dc, theme_.colors().text);
            return reinterpret_cast<LRESULT>(theme_.bg_brush());
        }

        case WM_NOTIFY: {
            const auto* nm = reinterpret_cast<const NMHDR*>(lp);
            if (nm->code == NM_CUSTOMDRAW && rates_.owns(nm->hwndFrom))
                return rates_.custom_draw(*reinterpret_cast<const NMCUSTOMDRAW*>(lp), theme_);
            break;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, theme_.colors().text);
            SetBkColor(dc, theme_.colors().panel);
            return reinterpret_cast<LRESULT>(theme_.panel_brush());
        }

        case WM_SETTINGCHANGE:
            if (is_color_scheme_change(lp) && theme_.refresh()) apply_theme();
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            rebuild_fonts();
            apply_fonts();
            // Nur die vorgeschlagene Position uebernehmen, die Groesse rechnen wir selbst.
            resize_to_design(reinterpret_cast<const RECT*>(lp));
            return 0;
        }

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layout();
            return 0;

        case WM_APP_SNAPSHOT:
            on_snapshot();
            return 0;

        case WM_APP_GKEY:
            // Nur die steigende Flanke loest aus; das Loslassen interessiert nicht.
            if (HIWORD(wp) != 0)
                on_gkey_pressed(static_cast<uint8_t>(LOWORD(wp)), static_cast<uint32_t>(lp));
            return 0;

        case WM_GKEY_CAPTURED:
            on_gkey_captured(static_cast<uint8_t>(wp));
            return 0;

        case WM_SLIDER_CHANGED: {
            const int value = static_cast<int>(lp);
            const bool final_value = HIWORD(wp) != 0;
            if (updating_) return 0;
            wchar_t buf[16];
            swprintf(buf, 16, L"%d", value);
            SetWindowTextW(edit_, buf);
            if (final_value) commit_dpi(value);
            return 0;
        }

        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONDBLCLK:
                case NIN_SELECT:
                    toggle_window(!IsWindowVisible(hwnd_));
                    break;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    show_tray_menu();
                    break;
                default:
                    break;
            }
            return 0;

        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                // Ereignisse kommen in Schueben; einmal nachlaufend sammeln.
                SetTimer(hwnd_, kDeviceChangeTimer, 700, nullptr);
            }
            return TRUE;

        case WM_TIMER:
            if (wp == kDeviceChangeTimer) {
                KillTimer(hwnd_, kDeviceChangeTimer);
                manager_.refresh(true);
            }
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);

            if (id == IDC_COMBO && code == CBN_SELCHANGE) {
                const int sel = ComboBox_GetCurSel(combo_);
                if (const auto* key = reinterpret_cast<const wchar_t*>(ComboBox_GetItemData(combo_, sel)))
                    current_key_ = key;
                if (snap_) update_device_view(*snap_);
                return 0;
            }
            if (id == IDC_EDIT && code == EN_KILLFOCUS && !updating_) {
                commit_dpi(read_dpi_field());
                return 0;
            }
            if (id >= IDC_PRESET && id < IDC_PRESET + 8 && code == BN_CLICKED) {
                const auto& p = settings_.presets();
                const size_t i = static_cast<size_t>(id - IDC_PRESET);
                if (i < p.size()) commit_dpi(p[i]);
                return 0;
            }
            if (id >= IDC_RATE && id <= rates_.last_id() && code == BN_CLICKED) {
                if (!updating_) commit_rate(rates_.rate_for_id(id));
                return 0;
            }
            if (id == IDC_AUTOSTART && code == BN_CLICKED) {
                const bool on = Button_GetCheck(chk_autostart_) == BST_CHECKED;
                settings_.set_autostart(on);
                Button_SetCheck(chk_autostart_, settings_.autostart() ? BST_CHECKED : BST_UNCHECKED);
                set_status(settings_.autostart() == on
                               ? (on ? L"Autostart eingerichtet." : L"Autostart entfernt.")
                               : L"Autostart-Eintrag ließ sich nicht schreiben.",
                           settings_.autostart() != on);
                return 0;
            }
            if (id == IDC_TRAY && code == BN_CLICKED) {
                settings_.set_minimize_to_tray(Button_GetCheck(chk_tray_) == BST_CHECKED);
                return 0;
            }
            if (id == IDC_GKEYS_ON && code == BN_CLICKED) {
                if (const auto* m = current()) {
                    const bool on = Button_GetCheck(chk_gkeys_) == BST_CHECKED;
                    auto ds = settings_.device(m->key);
                    ds.gkeys_active = on;
                    settings_.set_device(m->key, ds);
                    // set_gkey_active traegt den Wunsch selbst ein -- kein set_desired,
                    // das wuerde den uebrigen Wunschzustand ueberschreiben.
                    manager_.set_gkey_active(current_key_, on);
                }
                return 0;
            }
            if (gkeys_.on_command(id)) return 0;

            if (id == IDM_SHOW) { toggle_window(true); return 0; }
            if (id == IDM_EXIT) { quitting_ = true; DestroyWindow(hwnd_); return 0; }
            if (id >= IDM_PRESET && id < IDM_PRESET + 8) {
                const auto& p = settings_.presets();
                const size_t i = static_cast<size_t>(id - IDM_PRESET);
                if (i < p.size()) commit_dpi(p[i]);
                return 0;
            }
            if (id >= IDM_RATE && id < IDM_RATE + 8) {
                if (const auto* m = current()) {
                    auto sorted = m->rate.rates_hz;
                    std::sort(sorted.begin(), sorted.end(), std::greater<uint16_t>());
                    const size_t i = static_cast<size_t>(id - IDM_RATE);
                    if (i < sorted.size()) commit_rate(sorted[i]);
                }
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            if (!quitting_ && settings_.minimize_to_tray() && tray_.visible()) {
                toggle_window(false);
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            // Nach dieser Nachricht beendet Windows den Prozess, ohne dass noch WM_DESTROY
            // kommt. Ohne Aufraeumen hier blieben Host-Modus und G-Tasten-Software-Modus
            // bei jedem Abmelden und Herunterfahren stehen -- wie nach Stop-Process -Force.
            // Kein Deadlock beim Warten auf den Worker: er und die Verteiler posten nur.
            if (wp) {
                ShutdownBlockReasonCreate(hwnd_, L"ghub-lite stellt die Geräte zurück …");
                shutdown_devices();
                ShutdownBlockReasonDestroy(hwnd_);
            }
            return 0;

        case WM_DESTROY: {
            shutdown_devices();
            const int n = ComboBox_GetCount(combo_);
            for (int i = 0; i < n; ++i)
                delete[] reinterpret_cast<wchar_t*>(ComboBox_GetItemData(combo_, i));
            if (font_) DeleteObject(font_);
            if (font_head_) DeleteObject(font_head_);
            font_ = font_head_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace ui
