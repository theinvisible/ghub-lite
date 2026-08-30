# ghub-lite — Hinweise für Claude Code

Schlanker Win32-Ersatz für Logitech G HUB: DPI und Signalrate der Maus, G-Tasten der
Tastatur. Fachliche Beschreibung steht im `README.md` — hier steht nur, was beim Arbeiten
am Code hilft.

## Harte Vorgaben

* **Ein einziges Binary**, keine Abhängigkeit außer System-DLLs. Statische CRT. Wer eine
  Bibliothek hinzufügen will, muss das begründen — `dumpbin /dependents` ist der Prüfstein.
* **Reines Win32**, kein Framework, kein Direct3D/Direct2D. Die Oberfläche ist GDI plus
  System-Controls; nur der Schieberegler ist selbst gezeichnet.
* **Kommentare auf Deutsch**, ohne Umlaute im Quelltext (nur in Zeichenketten für die
  Oberfläche). Kommentare erklären das *Warum*, nicht das Was.
* `/W4 /permissive-` muss warnungsfrei bleiben.

## Bauen und Starten

```
cmake --preset msvc-release
cmake --build --preset msvc-release
```

CMake liegt unter
`C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
und ist nicht im PATH.

**Für den Alltagsbetrieb das Release-Binary nehmen**, nicht `cmake-build-relwithdebinfo`
(1,33 MB gegen 322 KB). Es gibt eine Einzelinstanz-Sperre: ein zweiter Start holt das
vorhandene Fenster nach vorn und beendet sich.

## Aufbau

```
src/hidpp/    Protokoll und Geraeteverwaltung, keine UI-Abhaengigkeit
src/ui/       Fenster, Panels, Dark Mode
src/app/      Einstiegspunkt, Einstellungen, Tastenkombinationen senden
tools/        hidpp_dump -- Konsolen-Probe, benutzt dieselbe hidpp-Schicht
```

Drei Threads: der UI-Thread, ein **Worker** im `Manager` (alle Geräte-E/A) und pro
offenem Kanal ein **Verteiler-Thread** in `Channel`, der dauerhaft liest und Antworten von
unaufgeforderten Meldungen trennt. Die UI besitzt keine Geräte-Handles und blockiert nie.

## Gemessenes Protokollwissen

Nicht aus Dokumentation, sondern an dieser Hardware erhoben. Referenzausgaben liegen in
`docs/devlog/`.

| | |
|---|---|
| Maus G502 X PLUS | Empfänger `PID_C53A`, Usage-Page `0xFF00`, Geräteindex 1, Features `0x2201` v2 + `0x8060` v0 |
| Tastatur G815 | direkt USB `PID_C33F`, Usage-Page **`0xFF43`**, Geräteindex `0xFF`, `0x8010` GKeys v0 mit 5 Tasten |

Usage-Nummern unterscheiden sich je Gerät, **Reportlängen nicht** (7/20/64 Byte). Deshalb
sortiert `find_endpoints()` nach Länge — niemals nach Usage.

**Steckplatz-Antworten** beim Abklopfen von `0xFF` und `1…6`:
`8F 09` = schläft · `8F 08` = leer · `8F 01` = der Empfänger selbst ·
`FF 0A` = nicht unterstützt (Direktanschluss hat keine Funkplätze) · Timeout = da, funkt
aber nicht.

**G-Tasten-Meldung:** `11 FF <featIdx> 00 <Maske…>` — Event 0, swId 0, Bit 0 = G1
aufsteigend, Loslassen als Null-Maske. Voller Zustand bei jeder Änderung.

## Fallen, die schon Zeit gekostet haben

1. **`Device::feature()` darf ein fehlgeschlagenes `getFeature` nicht als „Feature nicht
   vorhanden" merken.** Ein aufwachendes Funkgerät lässt Abfragen ins Leere laufen; wer das
   cacht, hält die Maus für den Rest der Laufzeit für ein Gerät ohne DPI.
2. **Nur eine *Antwort* darf gecacht werden.** Timeout heißt „gerade nicht zu erfahren".
3. **Einmal-Lesen braucht Selbstheilung.** `load_details()` wird nur beim Verbinden
   gerufen; `details_complete` sorgt dafür, dass ein unvollständiger Durchlauf wiederholt
   wird. Ohne das bleibt z. B. die Ratenliste dauerhaft leer.
4. **`Manager::set_desired()` ersetzt den ganzen Wunschzustand** und ist ausschließlich
   für `prime_desired()` beim Start da. `set_dpi`/`set_rate`/`set_gkey_active` tragen ihre
   Werte selbst ein — wer daneben `set_desired` ruft, überschreibt Dinge, die nur der
   Manager kennt (etwa einen erzwungenen Host-Modus).
5. **Onboard- gegen Host-Modus:** im Onboard-Modus lehnt das Gerät `setReportRate` mit
   `0x02` ab, DPI geht weiterhin. ghub-lite schaltet nur bei Bedarf um und nimmt es beim
   Beenden zurück.
6. **`Stop-Process -Force` umgeht den Aufräumpfad.** Dann bleiben Host-Modus und
   G-Tasten-Software-Modus stehen. Zum Beenden `WM_COMMAND` mit `IDM_SHOW`/`IDM_EXIT`
   (201) posten oder das Tray-Menü benutzen.

## Testen am Gerät

`hidpp_dump.exe` ruft von sich aus nur Getter auf; gesetzt wird ausschließlich über die
Schalter.

```
hidpp_dump                       Endpunkte, Steckplätze, Feature-Tabelle, Rohantworten
hidpp_dump --manager             Selbsttest der Schicht, die auch die GUI benutzt
hidpp_dump --slot 1              nur ein Geräteindex
hidpp_dump --set-dpi 1600
hidpp_dump --set-rate 500
hidpp_dump --listen 30           G-Tasten mitschreiben
hidpp_dump --set-mode onboard 1  definierte Ausgangslage herstellen
hidpp_dump --host-test 500 1     Moduswechsel messen, Zustand wiederherstellen
```

**Die Maus schläft ständig.** Ein schlafendes Gerät antwortet schnell *ablehnend*, nicht
gar nicht — wer das nicht unterscheidet, hält funktionierenden Code für kaputt. Vor jeder
Messung den Zustand mit `--manager` festhalten, sonst sind Vorher/Nachher-Vergleiche
wertlos.

### Fenster von außen prüfen

* `Get-Process.MainWindowHandle` ist **0**, solange das Fenster im Infobereich liegt —
  über `EnumWindows` und die Klasse `GhubLiteMain` suchen.
* `GetWindowText` liest **Edit-Controls prozessübergreifend nicht** aus (Statics und
  Buttons schon). Für den echten Zustand einen Screenshot machen.
* PowerShell ist DPI-unaware: vor `GetWindowRect`/`PrintWindow` einmal
  `SetThreadDpiAwarenessContext(-4)` aufrufen, sonst wirkt jedes korrekte Layout
  abgeschnitten.

## Bewusst nicht enthalten

Onboard-Profilspeicher schreiben, RGB, Makros, M1/M2/M3-Bänke, andere G-Tasten-Aktionen als
Tastenkombinationen. Das sind Produktentscheidungen des Nutzers, keine offenen Aufgaben —
nicht ungefragt einbauen.
