// Was andere Prozesse ueber ein laufendes ghub-lite wissen muessen: die zweite Instanz beim
// Nach-vorn-Holen und der Installer beim sauberen Beenden. Bewusst ein eigener Header ohne
// UI-Abhaengigkeit, damit das Setup ihn einbinden kann und die Werte nicht auseinanderlaufen.
#pragma once

namespace app {

inline constexpr wchar_t kWindowClass[] = L"GhubLiteMain";

// WM_COMMAND an das Hauptfenster. Beenden laeuft ueber den normalen Aufraeumpfad
// (Onboard-Modus zurueck, G-Tasten-Software-Modus aus) -- anders als TerminateProcess.
inline constexpr int kCmdShow = 200;
inline constexpr int kCmdExit = 201;

} // namespace app
