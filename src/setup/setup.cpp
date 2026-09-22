// ghub-lite-setup -- Installer pro Benutzer.
//
// Installiert nach %LOCALAPPDATA%\Programs\ghub-lite, legt einen Startmenue-Eintrag und einen
// Eintrag unter "Apps & Features" an. Alles unter HKCU und im Benutzerprofil, deshalb ohne
// Adminrechte -- passend zu ghub-lite selbst, das ebenfalls asInvoker laeuft.
//
//   ghub-lite-setup.exe              mit Nachfrage installieren bzw. aktualisieren
//   ghub-lite-setup.exe /quiet       ohne Nachfrage (nach jedem Build)
//   uninstall.exe /uninstall [/quiet]
//
// Eine laufende Instanz wird vorher ueber ihren Beenden-Befehl geschlossen, nie hart: nur so
// stellt sie die Geraete zurueck (Onboard-Modus, G-Tasten). Siehe CLAUDE.md, Falle 6.

#include "app/ipc.h"
#include "app/settings.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kTitle[]        = L"ghub-lite Setup";
constexpr wchar_t kVersion[]      = L"" GL_VERSION;
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ghub-lite";
constexpr wchar_t kAppExe[]       = L"ghub-lite.exe";
constexpr wchar_t kUninstallExe[] = L"uninstall.exe";
constexpr wchar_t kShortcut[]     = L"ghub-lite.lnk";
constexpr wchar_t kDirSuffix[]    = L"\\Programs\\ghub-lite";

// Ausgang fuer Skripte: 0 = erledigt, 1 = abgebrochen, 2 = Fehler.
constexpr int kExitOk = 0, kExitCancel = 1, kExitError = 2;

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

// Immer neu gebildet, nie aus der Registry gelesen: der Deinstaller loescht dieses
// Verzeichnis rekursiv, ein untergeschobener Pfad waere fatal.
std::wstring install_dir() {
    const std::wstring base = known_folder(FOLDERID_LocalAppData);
    return base.empty() ? std::wstring() : base + kDirSuffix;
}

std::wstring shortcut_path() {
    const std::wstring programs = known_folder(FOLDERID_Programs);
    return programs.empty() ? std::wstring() : programs + L"\\" + kShortcut;
}

std::wstring error_text(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg = (n && buf) ? std::wstring(buf, n) : std::wstring(L"unbekannter Fehler");
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) msg.pop_back();
    return msg + L" [" + std::to_wstring(code) + L"]";
}

void show_error(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), kTitle, MB_ICONERROR | MB_OK);
}

bool has_flag(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        while (*a == L'/' || *a == L'-') ++a;   // /quiet, -quiet und --quiet gleich behandeln
        if (_wcsicmp(a, name) == 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Laufende Instanz
// ---------------------------------------------------------------------------

enum class CloseResult { NotRunning, Closed, Timeout };

CloseResult close_running_instance() {
    HWND wnd = FindWindowW(app::kWindowClass, nullptr);
    if (!wnd) return CloseResult::NotRunning;

    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    HANDLE proc = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;

    PostMessageW(wnd, WM_COMMAND, app::kCmdExit, 0);

    // Auf den Prozess warten, nicht aufs Fenster: das Fenster ist weg, bevor der Aufraeumpfad
    // die Geraete zurueckgestellt und die Exe freigegeben hat.
    if (proc) {
        const DWORD w = WaitForSingleObject(proc, 15000);
        CloseHandle(proc);
        return w == WAIT_OBJECT_0 ? CloseResult::Closed : CloseResult::Timeout;
    }
    for (int i = 0; i < 150 && IsWindow(wnd); ++i) Sleep(100);
    return IsWindow(wnd) ? CloseResult::Timeout : CloseResult::Closed;
}

bool close_or_complain(CloseResult* result = nullptr) {
    const CloseResult r = close_running_instance();
    if (result) *result = r;
    if (r != CloseResult::Timeout) return true;
    show_error(L"ghub-lite läuft noch und hat sich nicht innerhalb von 15 Sekunden beendet.\n\n"
               L"Bitte über das Symbol im Infobereich beenden und das Setup erneut starten.");
    return false;
}

void launch(const std::wstring& exe, const std::wstring& dir, bool tray) {
    ShellExecuteW(nullptr, L"open", exe.c_str(), tray ? L"--tray" : nullptr, dir.c_str(),
                  SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Installieren
// ---------------------------------------------------------------------------

bool write_payload(const std::wstring& target, std::wstring* err) {
    HRSRC res = FindResourceW(nullptr, L"PAYLOAD", RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const void* data = mem ? LockResource(mem) : nullptr;
    const DWORD size = res ? SizeofResource(nullptr, res) : 0;
    if (!data || size == 0) {
        *err = L"Das Setup enthält keine Programmdatei.";
        return false;
    }

    // Erst daneben schreiben, dann ersetzen: ein abgebrochener Schreibvorgang hinterlaesst
    // so keine halbe ghub-lite.exe.
    const std::wstring tmp = target + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        *err = L"Datei ließ sich nicht anlegen: " + error_text(GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, data, size, &written, nullptr);
    const DWORD code = GetLastError();
    CloseHandle(f);
    if (!ok || written != size) {
        DeleteFileW(tmp.c_str());
        *err = L"Datei ließ sich nicht schreiben: " + error_text(code);
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD mcode = GetLastError();
        DeleteFileW(tmp.c_str());
        *err = L"ghub-lite.exe ließ sich nicht ersetzen: " + error_text(mcode);
        return false;
    }
    return true;
}

bool create_shortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(dir.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(L"DPI, Signalrate und G-Tasten für Logitech-Geräte");

    IPersistFile* file = nullptr;
    HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) {
        hr = file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
    return SUCCEEDED(hr);
}

DWORD file_size_kb(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return static_cast<DWORD>((fa.nFileSizeLow + 1023) / 1024);
}

bool write_uninstall_entry(const std::wstring& dir) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = true;
    auto sz = [&](const wchar_t* name, const std::wstring& value) {
        ok &= RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                             static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    auto dword = [&](const wchar_t* name, DWORD value) {
        ok &= RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                             sizeof(value)) == ERROR_SUCCESS;
    };

    const std::wstring app = dir + L"\\" + kAppExe;
    const std::wstring uninstall = L"\"" + dir + L"\\" + kUninstallExe + L"\" /uninstall";
    sz(L"DisplayName", L"ghub-lite");
    sz(L"DisplayVersion", kVersion);
    sz(L"DisplayIcon", app);
    sz(L"InstallLocation", dir);
    sz(L"UninstallString", uninstall);
    sz(L"QuietUninstallString", uninstall + L" /quiet");
    dword(L"NoModify", 1);
    dword(L"NoRepair", 1);
    dword(L"EstimatedSize", file_size_kb(app) + file_size_kb(dir + L"\\" + kUninstallExe));
    RegCloseKey(key);
    return ok;
}

// Rueckgabe false = abgebrochen. autostart ist Ein- und Ausgabe (Haken im Dialog).
bool ask_install(const std::wstring& dir, bool update, bool* autostart) {
    const std::wstring instruction = std::wstring(update ? L"ghub-lite aktualisieren auf " : L"ghub-lite ")
                                     + kVersion + (update ? L"" : L" installieren");
    const std::wstring content =
        L"Ziel: " + dir + L"\n\n"
        L"Es werden keine Adminrechte gebraucht. Ein laufendes ghub-lite wird vorher sauber "
        L"beendet und danach neu gestartet. Die Einstellungen bleiben erhalten.";

    const TASKDIALOG_BUTTON buttons[] = {{IDOK, update ? L"Aktualisieren" : L"Installieren"}};

    TASKDIALOGCONFIG tdc{};
    tdc.cbSize = sizeof(tdc);
    tdc.hInstance = GetModuleHandleW(nullptr);
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | (*autostart ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tdc.pszWindowTitle = kTitle;
    tdc.pszMainIcon = MAKEINTRESOURCEW(1);
    tdc.pszMainInstruction = instruction.c_str();
    tdc.pszContent = content.c_str();
    tdc.pButtons = buttons;
    tdc.cButtons = 1;
    tdc.nDefaultButton = IDOK;
    tdc.pszVerificationText = L"Beim Anmelden starten";

    int pressed = 0;
    BOOL checked = FALSE;
    if (FAILED(TaskDialogIndirect(&tdc, &pressed, nullptr, &checked))) return false;
    *autostart = checked != FALSE;
    return pressed == IDOK;
}

int install(bool quiet) {
    const std::wstring dir = install_dir();
    if (dir.empty()) { show_error(L"Der Ordner %LOCALAPPDATA% ließ sich nicht ermitteln."); return kExitError; }
    const std::wstring app = dir + L"\\" + kAppExe;
    const bool update = GetFileAttributesW(app.c_str()) != INVALID_FILE_ATTRIBUTES;

    // Im stillen Modus bleibt der Autostart, wie er ist -- er wird nur auf den installierten
    // Pfad umgebogen. Heute zeigt er sonst womoeglich noch ins Build-Verzeichnis.
    bool autostart = app::read_autostart();
    if (!quiet && !ask_install(dir, update, &autostart)) return kExitCancel;

    CloseResult closed = CloseResult::NotRunning;
    if (!close_or_complain(&closed)) return kExitError;

    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS &&
        GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        show_error(L"Der Zielordner ließ sich nicht anlegen:\n" + dir);
        return kExitError;
    }

    std::wstring err;
    if (!write_payload(app, &err)) { show_error(err); return kExitError; }

    // Sich selbst als Deinstaller danebenlegen. Laeuft das Setup schon von dort (Neuinstallation
    // aus uninstall.exe heraus), gibt es nichts zu kopieren.
    const std::wstring self = app::exe_path();
    const std::wstring uninstaller = dir + L"\\" + kUninstallExe;
    if (_wcsicmp(self.c_str(), uninstaller.c_str()) != 0 &&
        !CopyFileW(self.c_str(), uninstaller.c_str(), FALSE)) {
        show_error(L"Der Deinstaller ließ sich nicht ablegen: " + error_text(GetLastError()));
        return kExitError;
    }

    // Die folgenden Schritte sind Komfort: scheitern sie, ist ghub-lite trotzdem benutzbar.
    std::wstring warnings;
    const std::wstring lnk = shortcut_path();
    if (lnk.empty() || !create_shortcut(lnk, app, dir)) warnings += L"\n• Startmenü-Eintrag";
    if (!write_uninstall_entry(dir)) warnings += L"\n• Eintrag unter „Apps & Features“";
    if (!app::write_autostart(autostart, app)) warnings += L"\n• Autostart";
    if (!warnings.empty())
        show_error(L"ghub-lite ist installiert, aber folgendes ließ sich nicht einrichten:" + warnings);

    // Interaktiv mit Fenster als sichtbare Bestaetigung; still nur, wenn vorher eines lief --
    // dann so, wie es beim Anmelden gestartet wird.
    if (!quiet) launch(app, dir, false);
    else if (closed == CloseResult::Closed) launch(app, dir, true);
    return kExitOk;
}

// ---------------------------------------------------------------------------
// Deinstallieren
// ---------------------------------------------------------------------------

// Die laufende uninstall.exe kann sich nicht selbst loeschen. Ein verstecktes cmd wartet
// kurz, bis dieser Prozess beendet ist, und raeumt dann den Ordner weg.
bool schedule_dir_removal(const std::wstring& dir) {
    // Letzte Sicherung vor rmdir /s: nur genau unser Ordner, und nichts, was die
    // Kommandozeile aufbrechen koennte.
    const size_t suffix_len = wcslen(kDirSuffix);
    if (dir.size() <= suffix_len ||
        _wcsicmp(dir.c_str() + dir.size() - suffix_len, kDirSuffix) != 0 ||
        dir.find_first_of(L"\"%&|<>^") != std::wstring::npos) {
        return false;
    }

    wchar_t sys[MAX_PATH] = {};
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return false;
    const std::wstring cmd_exe = std::wstring(sys) + L"\\cmd.exe";
    std::wstring cmdline = L"\"" + cmd_exe + L"\" /d /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + dir + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Arbeitsverzeichnis bewusst ausserhalb des Ordners, sonst haelt cmd ihn selbst fest.
    if (!CreateProcessW(cmd_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int uninstall(bool quiet) {
    const std::wstring dir = install_dir();
    if (dir.empty()) { show_error(L"Der Ordner %LOCALAPPDATA% ließ sich nicht ermitteln."); return kExitError; }

    if (!quiet &&
        MessageBoxW(nullptr,
                    L"ghub-lite entfernen?\n\nDie Einstellungen in %APPDATA%\\ghub-lite bleiben "
                    L"erhalten und werden bei einer erneuten Installation wieder benutzt.",
                    kTitle, MB_ICONQUESTION | MB_OKCANCEL | MB_DEFBUTTON2) != IDOK) {
        return kExitCancel;
    }

    if (!close_or_complain()) return kExitError;

    app::write_autostart(false);
    if (const std::wstring lnk = shortcut_path(); !lnk.empty()) DeleteFileW(lnk.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);

    // Die Abschlussmeldung vor dem Aufraeumen zeigen: solange sie offen ist, laeuft diese
    // Exe noch aus dem Ordner, und rmdir kaeme nicht an ihr vorbei.
    if (!quiet) MessageBoxW(nullptr, L"ghub-lite wurde entfernt.", kTitle, MB_ICONINFORMATION | MB_OK);

    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES && !schedule_dir_removal(dir)) {
        show_error(L"Der Programmordner ließ sich nicht entfernen:\n" + dir);
        return kExitError;
    }
    return kExitOk;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool quiet = argv && has_flag(argc, argv, L"quiet");
    const bool remove = argv && has_flag(argc, argv, L"uninstall");
    if (argv) LocalFree(argv);

    // Fuer IShellLink; der TaskDialog kommt ohne aus.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int rc = remove ? uninstall(quiet) : install(quiet);
    if (SUCCEEDED(com)) CoUninitialize();
    return rc;
}
