// Symbol im Infobereich. Haelt nur das Symbol selbst -- das Kontextmenue baut das
// Hauptfenster, weil dort die Geraeteliste und die Schnellwahlwerte liegen.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace ui {

class Tray {
public:
    ~Tray();

    bool add(HWND owner, UINT callback_msg, HICON icon, const std::wstring& tip);

    // Symbol mit den Daten aus add() neu anlegen: nach einem Neustart des Explorers
    // (TaskbarCreated) oder wenn add() beim Anmelden zu frueh kam.
    bool restore();
    void set_tip(const std::wstring& tip);
    void remove();
    bool visible() const { return added_; }

    // Balloon-Hinweis, z.B. beim Wegminimieren.
    void notify(const std::wstring& title, const std::wstring& text);

private:
    NOTIFYICONDATAW nid_{};
    bool added_ = false;
};

} // namespace ui
