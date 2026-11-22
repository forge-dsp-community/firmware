# forge-dsp-firmware

Freie Firmware in C für die Forge-DSP-Geräte von Mark Duprau (Forge Audio, 2011–2021). Sie portiert die
BASCOM-Firmware 1.11.6000 (bitbucket.org/forge-dsp/firmware) für **alle Varianten** nach avr-gcc/PlatformIO.
Geflasht wird über den vorhandenen Bootloader chip45boot3, direkt aus VS Code.

## Kompatibilität

Welche Firmware passt auf welche Hardware. Seit 2.5.0 gibt es **eine Release-Datei je Gerätefamilie**: alle 96ELN
(PCB 3.0 und 4.0) und der CORE D8 teilen sich eine Datei, dazu 96ELN-3 und 96LN. Die Kennung (Modell, PCB) steht im
EEPROM und bleibt beim Update erhalten ([docs/single-firmware.md](docs/single-firmware.md), #31); die PCB 3.0 des 96ELN
erkennt die Firmware beim Start am fehlenden Taktgenerator Si5338 (#69). Die Web-GUI wählt die Datei selbst.

| Gerät | PCB | `system.key.info` | Release-Datei (ab 2.5.0) | Umgebung | Original 1.11.6000 (Release [0.0.1116000](https://github.com/forge-dsp-community/firmware/releases/tag/0.0.1116000)) | Am Gerät getestet |
|---|---|---|---|---|---|---|
| Forge DSP 96ELN | 3.0 | `96ELN-4.0` (sic, Quelltext) | `forge-dsp-96ELN-<v>.hex` | `V4x-96ELN` | `forge-dsp-V30-96ELN-bascom-1.11.6000.hex` | – (nativ) |
| Forge DSP 96ELN | 4.0 | `96ELN-4.0` | `forge-dsp-96ELN-<v>.hex` | `V4x-96ELN` | `forge-dsp-V40-96ELN-bascom-1.11.6000.hex` | ja (DEV-Gerät) |
| ETON CORE D8 | 4.0 / 4.1 | `CORED8-4.0` / `CORED8-4.1` | `forge-dsp-96ELN-<v>.hex` | `V4x-96ELN` | `forge-dsp-V40-CORED8-…` / `forge-dsp-V41-CORED8-bascom-1.11.6000.hex` | – |
| Forge DSP 96ELN-3 | 4.0 / 4.1 | `96ELN3-4.0` / `96ELN3-4.1` | `forge-dsp-96ELN-3-<v>.hex` | `V4x-96ELN-3` | `forge-dsp-V40-96ELN-3-…` / `forge-dsp-V41-96ELN-3-bascom-1.11.6000.hex` | – |
| Forge DSP 96LN | 3.0 | `96LN-3.0` | `forge-dsp-96LN-<v>.hex` | `V30-96LN` | `forge-dsp-V30-96LN-bascom-1.11.6000.hex` | – |

Bis 2.4.1 hießen die Dateien nach der Umgebung (`V30-96ELN`, `V4x-96ELN`, …) und die PCB 3.0 des 96ELN hatte eine eigene.
Jede Umgebung gibt es als sichere Variante (`<name>`, kein Ton) und als vollständige (`<name>-bringup`, die Release-Datei).
`V30-96ELN` bleibt für die Entwicklung und den Rückweg zum Original.

Alle: ATxmega256A3U, interner 32-MHz-Takt.

**Stand:** Der Funktionsumfang des Originals ist portiert: komplettes Textprotokoll, Register und EEPROM-Layout,
Presets, Biquads, Mixer, Delays, MODC-Steuerblöcke, Events, ADC/Temperatur, IR-Empfänger und die Einschaltsequenz
aller Bausteine. Nativ getestet für alle 7 Varianten (über 160 Protokolltests, dazu die DSP-Programme im
Simulator); auf einem Forge 96ELN (PCB 4.0) im Dauertest und im Fahrzeug: Start auf der Abstimmung des Originals ohne
Werks-Reset, Biquad-Koeffizienten bitgenau, Audioweg gemessen (#49), Hörtest im Auto bestanden. Aktuelles Release:
**[2.9.1](https://github.com/forge-dsp-community/firmware/releases/latest)** (Versionsnummern bewusst oberhalb der
letzten BASCOM-Firmware 1.11.6000). Remote-Out ist wie im Original aktiv, mit angeschlossener Endstufe aber noch nicht
geprüft. Fahrplan in den GitHub-Issues.

## Bedienen: Web-GUI

Neben der Original-GUI (Java, Windows) gibt es eine eigene Oberfläche im Browser:
**[forge-dsp-community.github.io/gui](https://forge-dsp-community.github.io/gui/)**
([Quelltext](https://github.com/forge-dsp-community/gui)). Sie verbindet sich über Web Serial direkt mit dem USB-Port
des Forge, nichts zu installieren, und nutzt die Erweiterungen dieser Firmware: Pegelanzeige, Biquad-Verteilung,
Weichenansicht, Limiter, Presets vergleichen, Geräteeinstellungen in Worten (Ein-/Ausschalten, Takt, S/PDIF, Clickless),
Startzeiten, Speicherkarte und Log, Diagnose und das **Firmware-Update im Browser** (Seite *Firmware*, mit Sicherung
vorher, auf die Karte im Forge oder als Dateien). Mit der Original-Firmware funktioniert sie ebenfalls, ohne die
Erweiterungen.

## Was die Port-Firmware zusätzlich kann

Ausführlich mit allen Unterschieden und Befehlen: **[docs/port-changes.md](docs/port-changes.md)**.
`get.1 system.help` listet alle Systembefehle der laufenden Firmware.

### Klang und DSP

- **Biquads frei verteilen** (`get.1 system.biquads`, `set.1 system.biquads.K=[…]`): Jeder DSP-Kern hat einen
  gemeinsamen Pool, die Filterzahl je Ausgang bzw. Summe ist einstellbar (z. B. 170 Bänder für einen Ausgang);
  Summe A/B haben zusammen 28 Biquads mehr als im Original (172 statt 144). Die MCU erzeugt das passende
  DSP-Programm selbst; gespeicherte Bänder wandern mit (#47).
- **Limiter je Ausgang im DSP** (`set.1 system.dsp.limiter.N=-60`, Attack/Release gemeinsam): weich geregelt und hart
  auf der Schwelle begrenzt, schützt Endstufen und Lautsprecher; ausgeschaltet bitgleich zum Programm ohne Limiter (#58).
- **Optimierte DSP-Programme** (Vorgabe in den Vollstart-Builds): gleiche Signalverarbeitung, im Simulator bitgleich,
  mit freien Takten für die Erweiterungen (#47).
- **Clickless** (Vorgabe aus, `set.1 biquad.clickless=1`): Filter- und Mixer-Änderungen werden ohne Aussetzer
  eingeblendet. Typwechsel, Hinzufügen und Entfernen von Filtern laufen über eine kurze Delle des Kanals
  (etwa 0,2 s) statt über Mute und Knacken (#92). Andere Änderungen (Delay, Eingangswahl) sind nur noch ca. 1,4 s
  statt 2 s stumm (#44, #50).
- **Loudness** (gehörrichtige Lautstärke) als neuer MODC-Block, einstellbar im Loudness-Dialog der Original-GUI;
  optional **dynamisch** nach dem gemessenen Eingangspegel ([Konfiguration](docs/port-changes.md#loudness-m6-6), #6, #51).
  Braucht die letzten zwei Filter seiner Kanäle frei und lässt sich nur dann einschalten; vorhandene Filter ersetzt er nie (#77).
- **GALA** (geschwindigkeitsabhängige Lautstärke und Loudness) als neuer MODC-Block: Geschwindigkeit per Befehl
  (`set.1 system.kmh=80`, z. B. vom Headunit) oder als Tachoimpulse an einem Masseeingang
  ([Konfiguration](docs/port-changes.md#gala-71), #71).
- **Neue Filtertypen:** 17 BandPass (im Original geplant, #20), 21 Allpass 1. Ordnung und 22 „Phase-Shift mit
  Wunschwinkel“ (z. B. −45° bei 80 Hz) für den Phasenabgleich (#48).
- **MODC-Blöcke aus der Original-GUI**, die dort keine Firmware hatten: Mono je Ausgangspaar (#14), analoger Mischer
  (#16), automatische Eingangswahl (#17).
- **S/PDIF-Ausgang aus dem DSP** (`set.1 system.spdif.out=1`, Rate mit `system.spdif.rate`): Der optische Port 2
  sendet nach einem Umbau (Sendemodul, zwei Kondensatoren, Lötjumper) den Mixer-Ausgang 1/2 mit den Filtern der
  Summen A/B, mit 0 dB am Sender (#82, #83). Umbau und Routing: [docs/spdif-out.md](docs/spdif-out.md).

### Pegel, Diagnose, Log

- **Pegelanzeige** (`get.1 system.vu`, `get.1 system.vu.out`, `get.1 system.vu.spdif`): Effektivpegel aller Eingänge,
  Ausgänge und des S/PDIF-Abgriffs (Kern 1 DO7/DO8, #91) für das VU-Meter der GUI, gemessen nur solange jemand fragt
  (#52). Beim 96LN nicht verfügbar (#90).
- **Binäres Log auf der microSD** (`set.1 system.debug.out=4`, Stufe mit `system.debug.mode`): Start mit Reset-Ursache,
  Fehlerzähler, DSP-Ereignisse, Presets, Versorgung als kleine Sätze statt Text, ausgewertet in der GUI
  ([docs/log-format.md](docs/log-format.md), #60).
- **Startprotokoll der DSP-Kerne** (`get.1 system.hardware.dsp`, `.1`/`.2`): Ladezeiten je I2C-Bus, Status, Version, ob ein
  EEPROM neu geschrieben und richtig zurückgelesen wurde, auch für den Start vor einem DSP-Neustart (#45, #57).
- **Versorgungsspannung überwachen** (`get.1 system.supply`): 3,3-V-Schiene, Bordspannung über einen GVOL-Eingang,
  Ereignis und auf Wunsch Stummschalten bei Unterspannung ([docs/supply.md](docs/supply.md), #46).
- **ACC aus** (`system.shutdown.hold`): sofort stumm, Endstufen bleiben einstellbar lange an (Vorgabe 5 s); kommt ACC
  vorher zurück, läuft der Ton ohne Neustart weiter (#68).
- **Start schneller und messbar** (`get.1 system.startup.time`): Zeit bis zu jeder Startphase und im Bootloader davor; die
  Prüfung der DSP-Programme und das erste Preset lesen erst und schreiben nur, was abweicht. Nach einem Update blieb bis
  2.3.0 die Bootloader-Sequenz im EEPROM stehen, jeder Kaltstart wartete dann 58,6 s im Bootloader; jetzt wird sie
  gelöscht. Am 96ELN kalt rund 68 s → 9,6 s bis Ton (#67). Laufzeit seit dem Einschalten: `get.1 system.uptime`.
- **Laufzeiten der Hauptschleife** (`get.1 system.timing`): längste Laufzeit jeder Aufgabe seit dem Start, zum Finden von
  Hängern (#72).
- **Fehlerzähler zurücksetzen** (`set.1 system.errors=0`), im Original nur per Werks-Reset.
- **I2C-Diagnose**: Bausteine direkt lesen und schreiben (`i2c.B.A.…`, `set.1 i2c.B.A.buffer=[…]`), damit z. B. den
  DSP-Speicher per PEEK (#51).
- **USB-Audio nur bis 96 kHz, mit Forge-Namen** (V40-Boards, XHRA-2HPA): Die Firmware schreibt beim Start den
  Konfigblock des XMOS, mit 44,1–96 kHz und dem Namen „Forge DSP <Modell> - Audio“ und der MCU-ID als Seriennummer. Das geht nur bei einem Start ohne
  microSD-Karte und ohne Host am USB-Audio-Port; danach bleibt der Block im Flash, USB-Audio läuft auch mit Karte.
  Lesen: `get.1 extender.usb.flash` / `get.1 extender.usb.config`. Zurück zum Originalblock: `set.1 extender.usb.config=0`
  (#76).
- **Grenzen lesbar** (`get.1 system.limits.*`): Wertebereiche, Filtertypen und Laufzeit-Speicher je DSP, damit eine GUI
  nichts fest einbauen muss (#54).

### microSD

- **Presets von der Karte** (im Original aus Lizenzgründen entfernt): `.cfg`-Dateien der GUI als `preset_N.cfg` lädt
  der Forge beim Start in die Presets (FatFs statt AVR-DOS, am Gerät geprüft, #24).
- **Preset exportieren** (`set.1 system.sd.export=N`) im selben Format.
- **Dateien über die serielle Verbindung** (`system.sd.open/create/read/write/close/delete`): in Blöcken mit Prüfsumme,
  die GUI hat dafür eine Seite *Speicherkarte* (#59). `get.1 system.sd` / `system.sd.files` zeigen Karte und Dateien.
- **Sicherung vor dem Update** (`set.1 system.sd.backup.flash|eeprom|preset|N=S`, Platz S = 1…9): Die Firmware kopiert
  Anwendungs-Flash, MCU-EEPROM, Preset-EEPROM und die EEPROMs der DSP-Kerne (Programm und Biquads) selbst auf die Karte
  (`FLASH_S.BIN`, `EEPROM_S.BIN`, `PRESET_S.BIN`, `DSPN_S.BIN`) und prüft jede Datei durch Zurücklesen (#61).
- **Sicherung zurückspielen** (`set.1 system.sd.restore.eeprom|preset|N=S`): EEPROMs aus Platz S zurück, geprüft und mit
  Neustart; nur für dasselbe Modell und PCB (`INFO_S.TXT`) (#65).

### Update, Start, Bedienung

- **Preset-Wechsel schneller** (#66): Nur Biquad-Plätze, die sich ändern, werden in die DSPs geschrieben, mit 2 statt
  16 ms Wartezeit und Zurücklesen; am 96ELN 0,65 statt 12 s (schlimmster Fall 5,5 statt 13,4 s). Das Speichern in die
  EEPROMs wird zurückgelesen und bei Abweichung wiederholt.
- **USB-COM schneller** (`set.1 system.baud.usb=384`): bis 460800 Baud auf Anfrage, zurück auf 38400 nach Neustart oder 30 s
  Ruhe; zusammen mit `get biquad.*.used` (nur belegte Filter, #64) liest die Web-GUI in rund 1,6 statt 9,4 s ein (#62).

- **Firmware-Update ohne Taster:** `set.1 system.boot` bringt den Forge direkt in den Bootloader. Grundlage ist eine
  Analyse des ausgelesenen Bootloaders ([docs/bootloader.md](docs/bootloader.md), #22). Update aus VS Code, mit
  `c45b3` oder im Browser über die Web-GUI.
- **Abstimmung sichern und vergleichen** beim Update (`c45b3 export-tuning / compare-tuning / restore-tuning`), im
  `.cfg`-Format der Original-GUI (#30).
- **Robuster Start**: DSP-Kerne, die beim Einschalten noch laden, werden nachgefragt; geschriebene DSP-Programme werden
  zurückgelesen, der Bus danach freigegeben; fehlt ein Kern trotzdem, startet die Forge genau einmal neu (#45, #57).
- **Drei Release-Dateien statt sieben** (96ELN mit CORE D8, 96ELN-3, 96LN): die Kennung (Modell, PCB) steht im EEPROM
  (`get.1 system.key.info.list`, `set.1 system.key.info="CORED8-4.1"`, #31), die PCB 3.0 des 96ELN erkennt die
  Firmware am fehlenden Si5338 und behält deren EEPROM-Aufteilung (#69).
- **Namen der Eingänge** im Gerät (`set.1 system.channel.in.N="…"`, #55).
- **Eingangswahl** `mixer.in` für die aktive Mixer-Position, mit Ereignis an die GUI (#23); IR-Taste 2 schaltet den
  Eingang weiter (#27).
- **Startbefehle**: bis zu 8 Befehle im EEPROM, die nach dem Laden des Presets laufen (#40).
- **Bootloader auslesen** (nur lesend, `get.1 system.hardware.boot.N`); das Abbild liegt unter [`bootloader/`](bootloader/).

### Behobene Fehler des Originals

- **USB-COM robust gegen Störbytes und falsche Raten** (#72): Zeilen über 127 Byte (z. B. Bootloader-Suche vor einem Befehl)
  verschoben bisher alle folgenden Antworten um eine Zeile; führende Störbytes werden übersprungen, und eine Seite mit
  38400 holt den Forge von 460800 sofort zurück statt nach 30 s.

- `preset.erase=4` löscht bei 4 Presets nicht mehr Preset 0 (#33), Rohwert-Biquads `biquad.….raw` funktionieren
  (#35), `get event.N.value` antwortet (#36), Delays in ms/cm/inch lassen sich lesen (#21), keine Speicherzugriffe
  hinter Arrays und Tabellen bei ungültigen Werten.
- **Sicherer Sysinit, vollständiger Werks-Reset** (#81): Ein leeres oder fremdes EEPROM löscht nichts, nur nie
  beschriebene Bereiche bekommen Werkswerte. `system.reinit` löscht alle Einstellungen, behält aber die Kalibrierungen
  (Haupttakt, Versorgung) und gibt sie vorher aus.
- Trotz der Erweiterungen (optimierte DSP-Programme mit Pool-Tabellen, FatFs, Limiter, Log, Sicherung auf die Karte)
  und obwohl eine Datei jetzt zwei Platinen bedient, rund **13 % kleiner** als das Original: 96ELN 174,6 KB statt
  200,0 KB, 96ELN-3 175,0 statt 200,9 KB, 96LN 162,9 statt 194,2 KB (ohne die Debug-Texte des Originals, #41).

## Build-Optionen

In `platformio.ini` unter `[common]`:

| Option | Standard | Wirkung |
|---|---|---|
| `FORGE_BRINGUP` | 0 | 1 = vollständige Peripherie-Init wie das Original. 0 = sichere Variante: Erkennung und Protokoll, Peripherie im Reset, **kein Ton** |
| `FORGE_AUDIO` | 0 (sicher), 1 (`-bringup`, seit 2.0.0) | 1 = Remote-Out (Endstufen) nach dem Laden des Presets wie im Original einschalten |
| `FORGE_FACTORY_RESET` | 0 | 1 = leeres/fremdes EEPROM löst den Werks-Reset aus wie im Original (löscht Abstimmungen und Presets), braucht `FORGE_BRINGUP=1` |
| `FORGE_I2C_KHZ`, `FORGE_I2C_FAST_KHZ` | 100 / 100 | Grundtakt der I2C-Busse und Takt geprüfter Übertragungen (400 möglich, dann gelegentlich korrigierte Abweichungen, #66) |
| `FORGE_I2C_CHECK_KHZ` | 400 | nur für Lesevorgänge beim Start, die gegen eine Prüfsumme verglichen werden (DSP-Programme, Biquads vor dem ersten Preset, #67) |
| `FORGE_DSP_WRITEDELAY` | 2 | Wartezeit in ms nach einem Schreiben in die DSPs (Original 16), jedes Schreiben wird zurückgelesen (#66) |
| `FORGE_DSP_OPT` | 0 (sicher), 1 (`-bringup`) | 1 = optimierte DSP-Programme: gleiche Signalverarbeitung, rund 130 bzw. 40 Takte je Sample weniger; Voraussetzung für Biquad-Pools, Ausgangspegel und Limiter (#47, #52, #58). V30-96LN behält sein Originalprogramm |

Lizenz: CC BY-NC-SA 4.0 (`LICENSE.txt`), wie das Original. `lib/fatfs`: FatFs R0.16 von ChaN (BSD-1-Clause, `lib/fatfs/LICENSE.txt`). `tools/c45b3-host` und die Bootloader-Quellen in
`tools/c45b3-host/tests/ctarget/upstream` stehen unter MIT (chip45boot3, Erik Lins).

## VS Code / PlatformIO
0. Einmalig `tools/get-toolchain.sh`: lädt avr-gcc 16.2.1 mit einem Compiler-Fix (Release
   `toolchain-avr-gcc-16.2.1-p1` dieses Repos, Linux x86_64, sha256 im Skript) und verlinkt ihn nach
   `.toolchain/avr-gcc`. Patch, Testfall und Build-Skript: `tools/toolchain/`. Grund: BASCOM rechnet Biquads in 64-Bit-`double`, das
   PlatformIO-Paket (avr-gcc 7.3) kann nur 32 Bit. Gebaut wird mit `-mdouble=64`.
1. Projekt öffnen, unten in der Statusleiste die Umgebung (= Variante) wählen.
2. `upload_port` in `platformio.ini` fest auf den CP2102 des Forge setzen (`/dev/serial/by-id/usb-Silicon_Labs_CP2102…`).
3. *PlatformIO: Upload*. Ablauf:
   - `get.1 system.key.info`: Modell und PCB des angeschlossenen Forge müssen zur Umgebung passen, sonst Abbruch.
     Für jeden 96ELN (PCB 3.0 und 4.0) und den CORE D8 ist das die Umgebung `V4x-96ELN` (#69).
   - **Bootloader:** Läuft schon die Port-Firmware, schickt das Tool `set.1 system.boot` und der Forge geht von selbst
     in den Bootloader (kein Taster). Mit der Original-Firmware startet er dabei nur neu: dann während der Wartezeit
     (60 s) den Forge ausschalten, den Taster neben dem USB-Port halten, einschalten, halten bis die orange LED blinkt.
   - Vorher: Abstimmung (geladenes Preset, Kanalnamen, IR-Codes, MODC) nach `backups/<datum>/tuning-before.cfg`
     sichern, im Format der Original-GUI.
   - Flash schreiben (Seiten werden im Bootloader geprüft), zurücklesen, Anwendung starten.
   - Danach: Abstimmung erneut lesen und vergleichen. Mit `[forge] restore_tuning = yes` werden Abweichungen
     zurückgeschrieben (nur die Unterschiede, nur bei gleichem Modell).
4. Project Tasks → Custom:
   - *Forge: versions*: Version von Bootloader und Firmware.
   - *Forge: backup*: Flash und EEPROM nach `backups/<Datum>/`, soweit lesbar.
   - *Forge: flash release 1.11.6000 (…)*: Rückweg auf die offizielle Firmware der Variante (96ELN PCB 3.0: in der
     Umgebung `V30-96ELN`). Sie wird aus dem
     GitHub-Release [0.0.1116000](https://github.com/forge-dsp-community/firmware/releases/tag/0.0.1116000) geladen
     und gegen `scripts/release-0.0.1116000.sha256` geprüft (Cache `.pio/release/`).
5. Monitor: 38400 Baud, CR LF. Format wie die GUI: `get.1 system.ping`, `set.1 <register>=<wert>;<cs>` (`1` = dieses
   Gerät, `cs` = Summe der Zeichen ab dem Register, 2 Hex). Ohne Leerzeichen nach dem Befehlszeichen liest das
   Original das erste Zeichen als Ziel (`<system…` = Slave).

c45b3 braucht nur pyserial, das liefert PlatformIO mit. CLI: `pio run -e V4x-96ELN-3 -t upload`.

## Aufbau
- `src/`: gemeinsamer Code aller Varianten (`src/cmd/`: Textprotokoll), `include/`: Header.
- `tests/`: `test_cmd.py` übersetzt die Firmware-Module nativ für jede Variante (`make -C tests/native`) und prüft
  Protokoll, EEPROM-Inhalte, Biquads (gegen eine unabhängige Referenz) und die Peripherie auf einem I2C-Modell.
- `docs/port-changes.md`: Ergänzungen und Abweichungen gegenüber dem Original.
- `docs/hardware.md`: Hardware-Referenz (Lötjumper „sec.spdif“ O/B/I, optische Module, CT7302-Bausteine).
- `docs/spdif-out.md`: S/PDIF-Ausgang aus dem DSP (Abgriff in Kern 1: Mixer und Summen-Ketten, nicht die Ausgangskanäle) – Umbau inkl. Kondensatoren an Port 2, Einstellungen, Routing, Fehlersuche.
- `docs/pruefung-2026-11-19.md`: Befunde der Prüfung vom 19.11.2026, Änderungen und Anleitung für die Nachmessung.
- `tools/toolchain/`: Compiler-Patch, Testfälle (ICE in lower-subreg, Fehlübersetzung beim Bitpaar-Tausch #84) und Build-Skript für die gepinnte Toolchain.
- `src/flavours/<Variante>/`: **generiert** mit `tools/gen_flavour.py` aus dem BASCOM-Quelltext. `flavour.h` enthält alle
  `const` des aufgelösten Builds als `BAS_*` und die Pin-Aliase als `PIN_*`, `flavour_pins.c` die Pin-Init aus `pins.bas`.
  Prüfen: `python3 tools/gen_flavour.py <forge-dsp/firmware> src/flavours --check`.
- `tools/bas_resolve.py`: löst `$include`/`#IF` eines BASCOM-Builds auf (`docs/inventory-*.md`, `--flat` = nur aktiver Code
  mit Datei:Zeile, Referenz beim Portieren).
- `tools/c45b3-host/`: Linux-Host für chip45boot3, mit Tests gegen ein Modell und gegen den nativ übersetzten Original-
  Bootloader-Code. `tests/pty_bridge.py` emuliert Firmware + Bootloader an einem pty für Upload-Tests ohne Hardware.
- `tools/sim/`: die Firmware als WebAssembly-Simulator für die Web-GUI (Befehle, EEPROM, Steuerblöcke ohne Gerät),
  gebaut mit emsdk 6.0.13 (`make -C tools/sim`).
- `tools/filtertest/`: Filter-, Gain-, Polaritäts-, Laufzeit- und Mute-Test aller 8 Analogausgänge mit einem
  Mehrkanal-Interface (z. B. Scarlett 18i20), Soll aus den Formeln der Firmware; Anschlussplan und Prompt für Claude in
  `tools/filtertest/README.md` bzw. `PROMPT.md`, simulierter Komplettlauf in `tests/test_filtertest.py`.
- Offizielle HEX 1.11.6000 aller Varianten: GitHub-Release `0.0.1116000` (nicht im git).

## Sicherheit
- Der Bootloader-Bereich ist per Lockbits geschützt (BLBB = WLOCK, am Gerät bestätigt: lesbar, nicht beschreibbar). Das Tool schreibt nie dorthin und sendet die
  abschließende Leer-Frame der Windows-GUI nicht (sie würde die Seite hinter dem Image löschen).
- Vor dem ersten eigenen Flashen: *Forge: backup*, Abstimmung über die Original-GUI sichern.
- Remote-Out (Endstufen) schalten die Vollstart-Builds (`-bringup`, Release ab 2.0.0) wie das Original nach dem Laden des
  Presets ein; die sicheren Builds lassen ihn aus.
- Abstimmungen werden nur durch einen ausdrücklichen Werks-Reset (`system.reinit`) gelöscht; Rückweg jederzeit über *Forge: flash release 1.11.6000*.
