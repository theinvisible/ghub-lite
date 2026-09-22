# ghub-lite

Schlanke Win32-Alternative zu Logitech G HUB für das, was man wirklich laufend braucht:
**DPI-Auflösung** und **Signalrate** von Logitech-Mäusen sowie die **G-Tasten** der
G-Series-Tastaturen.

Ein einzelnes Binary, ~290 KB, keine Abhängigkeiten außer System-DLLs, kein Dienst,
kein Installer. Dark Mode folgt dem Systemthema.

Zum Vergleich: G HUB fährt dafür ein Electron-Frontend plus Dauerdienst hoch — auf dem
Entwicklungsrechner waren das fünf `lghub.exe` plus `lghub_agent`, Tray und Updater.

## Stand

Entwickelt und verifiziert gegen eine **Logitech G502 X PLUS** an einem
POWERPLAY/LIGHTSPEED-Empfänger (`VID_046D&PID_C53A`) und eine **Logitech G815** per USB
(`VID_046D&PID_C33F`).

| Was | Wie |
|---|---|
| DPI lesen/setzen | HID++ 2.0 Feature `0x2201` v2 — verifiziert |
| Signalrate lesen/setzen | HID++ 2.0 Feature `0x8060` v0 — verifiziert |
| G-Tasten erkennen, Software-Modus | Feature `0x8010` v0, 5 Tasten — verifiziert |
| G-Tastendruck → Kombination senden | verifiziert — Bit 0 = G1, aufsteigend bis G5 |
| DPI über `0x2202` | implementiert, mangels Gerät **nicht** gegengeprüft |
| Signalrate über `0x8061` (2k/4k/8k) | implementiert, mangels Gerät **nicht** gegengeprüft |
| Akku | `0x1004` UnifiedBattery, sonst `0x1000` |
| Onboard-/Host-Modus | `0x8100` fn2 lesen, fn1 nur als Moduswechsel für die Signalrate — kein Profilspeicher-Write |

## Wie es funktioniert

Logitech-Geräte sprechen HID++ 2.0 über vendor-definierte HID-Collections. Welche
Usage-Page dabei benutzt wird, ist **geräteabhängig** — gemessen an dieser Hardware:

```
Maus am Empfänger   MI_02&COL01/COL02   UP:FF00  U:0001 / U:0002    short + long
Tastatur G815       MI_01&COL04/COL05   UP:FF43  U:0602 / U:0604    long + very long
```

Die Usage-Nummern sind also nicht übertragbar, die **Reportlängen** dagegen schon
(7 / 20 / 64 Byte inkl. Report-ID). Deshalb sortiert `find_endpoints()` die Collections
nach Länge statt nach Usage — sonst bräuchte jedes Gerät einen Sonderfall.

Das ist der Kniff unter Windows: die eigentliche Maus-Collection trägt die Kennung
`HID_DEVICE_SYSTEM_MOUSE` und lässt sich nicht mit Schreibzugriff öffnen — die
Vendor-Collections dagegen schon, ohne Adminrechte.

Gesendet wird ausschließlich über den Long-Kanal. Ein **Verteiler-Thread** liest dauerhaft
auf allen offenen Collections und sortiert jede Meldung ein: passt sie zu einer wartenden
Anfrage, bekommt sie der Wartende, sonst geht sie an den Meldungs-Handler. Die
Unterscheidung ist verlässlich, weil Antworten die swId ihrer Anfrage tragen (reihum
`2…15`, damit eine verspätete Antwort nicht bei der nächsten Anfrage landet) und
unaufgeforderte Meldungen `swId 0`. Ohne diesen Dauerbetrieb gingen G-Tastendrücke
verloren, weil zwischen zwei Anfragen niemand liest.

Feature-Indizes sind **geräteabhängig** und werden zur Laufzeit über `IRoot` (`0x0000`,
liegt immer auf Index 0) aufgelöst und gecacht. Nichts davon ist hartkodiert.

## Bedienung

Fenster: Gerät wählen, DPI per Schieberegler, Zahlenfeld oder Schnellwahl setzen,
Signalrate per Radioknopf. Beides wirkt sofort.

Das Symbol im Infobereich bietet dieselben Werte als Kontextmenü.

### Warum nur ein DPI-Wert und nicht fünf Stufen?

`0x2201 setSensorDpi` setzt genau **eine** aktive Auflösung. Die fünf Stufen, die G HUB
anzeigt, liegen im Onboard-Profilspeicher der Maus, und da schreibt ghub-lite bewusst nicht
hinein. Die Schnellwahlknöpfe sind deshalb *unsere* Werte aus `settings.ini`, kein
Gerätezustand.

### G-Tasten

Die Geräteliste oben im Fenster führt Maus **und** Tastatur; der Bereich darunter wechselt
je nach Gerätetyp. Bei der Tastatur steht dort eine Zeile pro G-Taste: aktuelle Belegung,
*Ändern*, *Löschen*.

*Ändern* startet die Aufnahme — die nächste Tastenkombination wird übernommen, Escape
bricht ab. Während der Aufnahme hängt ein `WH_KEYBOARD_LL`-Hook, der die Tasten schluckt,
damit die aufgenommene Kombination nicht nebenbei irgendwo im System landet. Ohne Hook
wären Win-Kombinationen und Druck gar nicht erfassbar.

**Das ist keine Umbelegung im Gerät.** `0x8010` schaltet die G-Tasten nur in den
Software-Modus: ihre Druckereignisse kommen als HID++-Meldungen bei ghub-lite an, und
ghub-lite sendet daraufhin die hinterlegte Kombination per `SendInput`. Daraus folgt:

* **Läuft ghub-lite nicht, tun die G-Tasten nichts.** Deshalb der Haken *G-Tasten von
  ghub-lite auswerten* und die Option *Beim Anmelden starten*.
* Beim Beenden wird der Software-Modus abgeschaltet, damit die Tasten auf ihre
  Onboard-Belegung zurückfallen statt tot zu bleiben.
* Nach Aus/Ein oder Neuanstecken wird er automatisch wieder gesetzt.

Echte Umbelegung ginge nur über das Onboard-Profil (`0x8100`) — siehe *Nicht enthalten*.

### Persistenz

Die Maus läuft im Onboard-Modus; ein gesetzter Wert überlebt Aus-/Einschalten nicht
zwingend. ghub-lite merkt sich die Werte deshalb selbst und wendet sie erneut an:

* beim Programmstart
* bei `WM_DEVICECHANGE` (Empfänger an-/abgesteckt)
* sobald ein zuvor stummes Gerät wieder antwortet (Aufwachen, Funkwiederkehr)

Dafür muss das Programm laufen — daher Infobereich-Symbol und die Option
*Beim Anmelden starten* (Eintrag unter `HKCU\...\CurrentVersion\Run`, nur auf Klick).

## Einstellungen

`%APPDATA%\ghub-lite\settings.ini`, von Hand editierbar:

```ini
[app]
minimize_to_tray=1
presets=800,1600,3200,6400      ; Schnellwahlknöpfe, bis zu 8

[device.C4F515BE]               ; Schlüssel = Unit-ID aus HID++ 0x0003
dpi=3200
rate=1000

[device.30394719]               ; die Tastatur
gkeys_active=1
gkey1=Ctrl+Shift+F13            ; sprachneutral abgelegt, deutsch angezeigt
gkey2=Alt+F4
```

## G HUB

Für **DPI und Signalrate** muss G HUB nicht beendet sein — die Schreibzugriffe
funktionieren auch parallel. Aber `lghub_agent` pollt dieselben Geräte und setzt eigene
Werte durch, sobald es meint, etwas korrigieren zu müssen.

Für die **G-Tasten** ist ein beendetes G HUB praktisch Voraussetzung: es beansprucht
denselben Software-Modus und bekommt dieselben Tastendrücke, würde also parallel eigene
Aktionen auslösen.

ghub-lite weist auf laufende `lghub*`-Prozesse hin, beendet sie aber nie von sich aus.

## Bauen

```
cmake --preset msvc-release
cmake --build --preset msvc-release
```

Braucht VS 2022 (MSVC 14.4x) und das Windows SDK. Statische CRT, damit die Exe ohne
VC++-Redistributable läuft.

## Werkzeuge

`hidpp_dump.exe` ist die Konsolen-Probe, mit der der Protokollteil entwickelt wurde:

```
hidpp_dump                      Endpunkte, Steckplätze, Feature-Tabelle, Rohantworten
hidpp_dump --extended           zusätzlich die weniger sicher dokumentierten Getter
hidpp_dump --slot 1             nur einen Geräteindex
hidpp_dump --set-dpi 1600       schreiben und zurücklesen
hidpp_dump --set-rate 500
hidpp_dump --manager            Selbsttest der Schicht, die auch die GUI benutzt
hidpp_dump --listen 30          G-Tasten in den Software-Modus und rohe Meldungen mitschreiben
```

Sie ruft von sich aus ausschließlich Getter auf. Gesetzt wird nur über die expliziten
Schalter. Eine Referenzausgabe liegt in `docs/devlog/`.

### Steckplatz-Antworten des Empfängers

Beim Abklopfen der Indizes `0xFF` und `1…6` antwortet der Empfänger unterschiedlich, und
der Unterschied ist wichtig:

| Antwort | Bedeutung |
|---|---|
| gültige Ping-Antwort | Gerät da und wach |
| `8F 09` Ressourcenfehler | gepaart, aber gerade im Schlaf |
| `8F 08` unbekanntes Gerät | Steckplatz leer |
| `8F 01` unbekannte Unterfunktion | der Empfänger selbst auf `0xFF` |
| Zeitüberschreitung | gepaart, funkt aber nicht — leere Plätze antworten prompt |

Deshalb verschwindet eine schlafende Maus in ghub-lite nicht aus der Liste, sondern steht
als *nicht verbunden* drin und wird beim Aufwachen automatisch nachgezogen.

## Nicht enthalten

RGB, Makros, M1/M2/M3-Bänke, Onboard-Profile schreiben, andere Aktionen als
Tastenkombinationen (Programmstart, Textbaustein), Tastenbelegung der Maus.

Das Onboard-Profil-Schreiben (`0x8100`) wäre der nächste sinnvolle Schritt: damit
überlebten DPI, Signalrate und G-Tasten-Belegung auch ohne laufendes Programm. Es braucht
aber Lesen und Schreiben des Profilspeichers samt Backup/Restore als Sicherheitsnetz und
kann nur, was die Firmware kann.

## Signalrate und Onboard-Modus

Gemessen an G502 X PLUS und G815: steht das Gerät im **Onboard-Modus** (`0x8100` fn2
meldet `01`), lehnt es `setReportRate` mit HID++-Fehler `0x02` ab — die Signalrate gehört
dort dem Profil im Gerät. **DPI lässt sich trotzdem setzen.** Im **Host-Modus** (`02`)
funktioniert beides. G HUB schaltet beim Start auf Host-Modus, weshalb die Rate mit
laufendem G HUB änderbar ist und ohne es nicht.

ghub-lite löst das selbst, aber zurückhaltend:

* Erst wird die Rate normal gesetzt. Nur wenn das **scheitert und das Gerät im
  Onboard-Modus steht**, schaltet ghub-lite auf Host-Modus und versucht es erneut.
* Klappt es auch dann nicht, wird der Modus sofort zurückgenommen — kein Umschalten
  ohne Gegenwert.
* Ein erzwungener Host-Modus landet als `host_mode=1` in der INI und wird beim nächsten
  Start wiederhergestellt, bevor die Rate gesetzt wird.
* **Beim Beenden schaltet ghub-lite zurück auf Onboard**, damit das Gerät ohne laufendes
  Programm wieder sein eigenes Profil benutzt. Die Rate fällt dabei auf den Profilwert
  zurück — das ist gewollt und der Preis dafür, nichts zu hinterlassen.

`0x8100` fn1 ist damit die einzige schreibende Stelle an diesem Feature, und sie ist ein
reiner Moduswechsel — **kein** Schreiben in den Profilspeicher. Wer den erzwungenen
Host-Modus loswerden will, setzt `host_mode=0` in der INI.

Zum Prüfen von Hand:

```
hidpp_dump --set-mode onboard|host [Index]   Betriebsmodus setzen
hidpp_dump --host-test [Hz] [Index]          Moduswechsel + Rate messen, Zustand wiederherstellen
```
