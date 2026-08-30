// ghub-lite -- schlanke Alternative zu Logitech G HUB fuer DPI und Signalrate.
//
// Einzelbinary: statische CRT, keine externen Abhaengigkeiten, nur System-DLLs.

#include "ui/dpi_panel.h"
#include "ui/main_window.h"
#include "ui/theme.h"

#include <windows.h>
#include <commctrl.h>

namespace {

bool has_flag(const wchar_t* cmdline, const wchar_t* flag) {
    return cmdline && wcsstr(cmdline, flag) != nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    // Nur eine Instanz: eine zweite wuerde um denselben HID++-Kanal konkurrieren.
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\ghub-lite-single-instance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"GhubLiteMain", nullptr)) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ui::Theme::init_process();

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    if (!ui::register_slider_class(inst)) return 1;
    if (!ui::MainWindow::register_class(inst)) return 1;

    ui::MainWindow window;
    if (!window.create(inst, has_flag(cmdline, L"--tray"))) {
        MessageBoxW(nullptr, L"Das Fenster ließ sich nicht anlegen.", L"ghub-lite",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window.hwnd(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (once) CloseHandle(once);
    return static_cast<int>(msg.wParam);
}
