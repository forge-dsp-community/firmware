# Forge Filtertest mit Mehrkanal-Interface (z. B. Focusrite Scarlett 18i20 4th Gen)

Prüft an einem Forge DSP alle 8 Analogausgänge gleichzeitig: Filter (EQ, Shelf, Hoch-/Tiefpass, Allpass), Gain,
Polarität, Laufzeit und Mute, jeweils in Betrag und Phase gegen die Formeln der Firmware.

**So misst das Werkzeug:**

- Das Interface spielt periodisches rosa Rauschen in die Analogeingänge des Forge und nimmt die 8 Ausgänge auf.
  Beides läuft in einem einzigen Vollduplex-Stream, also auf demselben Takt und mit konstanter Latenz.
- Je Testfall setzt das Werkzeug eine Einstellung auf alle Ausgänge, misst und setzt sie sofort zurück.
- Ausgewertet wird die **Änderung** gegenüber der Grundmessung. Die vorhandene Abstimmung, die Weichen und die
  Messkette fallen dabei heraus. Es muss also nichts umgebaut oder neu eingestellt werden.

Dauer: rund 5 Minuten für 13 Testfälle × 8 Ausgänge.

Ergebnis: `results/<Datum>/report.html` mit Kurven je Ausgang und Test, dazu `results.json` und die Sicherung der
Abstimmung (`tuning-before.cfg`).

## Sicherheit, vor dem Anschließen lesen

- **Keine Endstufen und keine Lautsprecher an den Forge-Ausgängen.** Die Ausgänge gehen nur zum Interface. Die Tests
  heben Pegel an (+6 dB), schalten Hochpässe um und verschieben Laufzeiten.
- **Phantomspeisung (48 V) an allen Eingängen des Interface AUS.**
- Am Forge nichts flashen, keinen Werks-Reset auslösen. Das Werkzeug schreibt nur einzelne Werte, sichert vorher die
  ganze Abstimmung, setzt jeden Wert sofort zurück und vergleicht am Ende.
- Während des Laufs keine andere Software mit dem Forge verbinden (GUI, App, Terminal), der Port wäre belegt.

## Material

- Forge DSP (96ELN, 96ELN-3 oder CORE D8) mit eigener 12-V-Versorgung (Labornetzteil, 2 A reichen), USB zum PC
- Focusrite Scarlett 18i20 4th Gen (oder ein anderes Interface mit mindestens 8 Eingängen und 2 Ausgängen)
- 8 Kabel Cinch (RCA) → 6,3-mm-Klinke **TS oder TRS unsymmetrisch**: Forge-Ausgänge → Interface-Eingänge
- 2 bis 6 Kabel 6,3-mm-Klinke → Cinch: Interface-Ausgänge → Forge-Analogeingänge
- PC mit Python 3.10 bis 3.13 (Windows, macOS oder Linux)

## Anschlussplan

```
                 Scarlett 18i20 4th Gen                          Forge DSP
          ┌─────────────────────────────────┐            ┌──────────────────────┐
          │ Line Out 3 ─────────────────────┼──Klinke→Cinch──► Analog-In 1       │
          │ Line Out 4 ─────────────────────┼──────────────► Analog-In 2       │
          │ Line Out 5 ─────────────────────┼──────────────► Analog-In 3       │
          │ Line Out 6 ─────────────────────┼──────────────► Analog-In 4       │
          │ Line Out 7 ─────────────────────┼──────────────► Analog-In 5       │
          │ Line Out 8 ─────────────────────┼──────────────► Analog-In 6       │
          │                                 │            │                      │
          │ Input 1 (Line) ◄────────────────┼──Cinch→Klinke── Ausgang 1        │
          │ Input 2 (Line) ◄────────────────┼──────────────── Ausgang 2        │
          │   …                             │            │   …                  │
          │ Input 8 (Line) ◄────────────────┼──────────────── Ausgang 8        │
          │                                 │            │                      │
          │ USB ─────────── PC ─────────────┼──USB──────── USB-Steuerung (COM) │
          └─────────────────────────────────┘            │ 12 V ◄── Netzteil    │
                                                         └──────────────────────┘
```

| Interface | Kabel | Forge | Hinweis |
|---|---|---|---|
| Line Out 3–8 | Klinke → Cinch | Analog-In 1–6 | gleiches Signal auf alle; weniger Eingänge gehen auch (mind. die, die der Mixer auf die Ausgänge legt) |
| Input 1–8 | Cinch → Klinke | Ausgang 1–8 | Zuordnung prüft das Werkzeug (`map`), Reihenfolge egal |
| USB | | USB-Steuerung | Forge meldet sich als serieller Port (CP2102), 38400 Bd |

- Die **Monitor-Ausgänge 1/2** des Scarlett nicht verwenden: Sie hängen am Lautstärkeregler.
- Die Analogeingänge des Forge stehen auf **Cinch/Low-Level** (Jumper wie im Einbau). Der Hochpegel-Eingang wird
  nicht gebraucht.
- Hängen an einem Forge-Ausgang im Mixer keine der gespeisten Eingänge, bekommt dieser Ausgang kein Signal. Er
  erscheint dann im Bericht als „kein Signal“. Abhilfe: alle 6 Eingänge speisen oder im Mixer einen gespeisten
  Eingang auf den Ausgang legen.

## Einstellungen am Scarlett (Focusrite Control 2)

1. **Abtastrate 48 kHz.**
2. Eingänge 1–8: Line (nicht Inst), **48 V aus**, Air aus, Gain zunächst ganz links. Pad an, falls vorhanden und die
   Aufnahme übersteuert.
3. **Routing:** Playback 3–8 → Line Out 3–8 (Grundeinstellung). Direct Monitor bzw. Mix auf Line 3–8 aus, damit
   nur das Testsignal hinausgeht und die Eingänge nicht zurück in den Forge laufen.
4. Kein anderes Programm spielt über das Interface (Systemklänge auf ein anderes Gerät legen).

## Installation

```
cd forge-filtertest
python3 -m venv .venv
. .venv/bin/activate            # Windows: .venv\Scripts\activate
pip install -r requirements.txt
python3 forge_filtertest.py selftest
```

`selftest` prüft die Auswertung ohne Hardware und muss mit „Selbsttest bestanden“ enden.

Treiber:
- **Windows:** Focusrite-Treiber; für den Forge den Silicon-Labs-CP210x-Treiber (COM-Port im Geräte-Manager).
- **macOS:** Der Port heißt `/dev/cu.SLAB_USBtoUART` oder `/dev/cu.usbserial-…`.
- **Linux:** `/dev/ttyUSB0`, Benutzer in der Gruppe `dialout` bzw. `uucp`.

## Ablauf

```
python3 forge_filtertest.py devices                                    # Name/Nummer des Interface
python3 forge_filtertest.py check --port COM5 --device "18i20"         # Pegel und Rauschabstand je Eingang
python3 forge_filtertest.py map   --port COM5 --device "18i20"         # welcher Eingang welcher Forge-Ausgang ist
python3 forge_filtertest.py run   --port COM5 --device "18i20" --map 1,2,3,4,5,6,7,8
```

1. **`check`:** Jeder Eingang muss Signal zeigen, keiner „ÜBERSTEUERT“, möglichst über 40 dB Rauschabstand.
   Zum Einstellen den Gain am Scarlett hochdrehen, bis die Spitze bei etwa −12 dBFS liegt. Alternativ den Pegel
   mit `--level` ändern (Standard −18 dBFS RMS, z. B. `--level -12`).
2. **`map`:** schaltet nacheinander alle Forge-Ausgänge bis auf einen stumm und meldet, an welchem Eingang er
   ankommt. Danach setzt es alle Stummschaltungen zurück. Die ausgegebene Zeile `--map …` beim `run` angeben.
3. **`run`:** sichert die Abstimmung, misst die Grundlinie und dann alle Testfälle. Am Ende vergleicht es die
   Abstimmung und schreibt den Bericht.

Andere Kanäle: `--play` (Ausgänge des Interface mit Testsignal, Standard `3,4,5,6,7,8`) und `--rec` (Eingänge,
Standard `1,…,8`). Einzelne Tests: `--only eq1k,delay`.

## Optional: S/PDIF-Ausgang des Forge (`--spdif`)

Frage: Liegt das Signal am S/PDIF-Ausgang vor oder hinter der Filterkette?

**Laut Schaltplan und Datenblatt:**
- Der optische Port 2 ist per **Jumper JP1** entweder S/PDIF-Eingang (`IN.SPDIF.3`) oder -Ausgang. Als Ausgang
  speist ihn `OUT.SPDIF.1` des CT7302 Nr. 2 (Grundplatine V40, Schaltplan Seite 3).
- Der CT7302 ist eine „SRC bridge“: Register 0x04, `SW_SEL_INPUT_CHANNEL`, wählt **einen** Eingang. Zur Wahl stehen
  S/PDIF IN-0…4, I2S IN-0 (USB-Audio), I2S IN-1 und I2S IN-2. Der gewählte Eingang geht nach dem Abtastratenwandler
  an **alle** Ausgänge, an I2S zum DSP und an den S/PDIF-Sender. Register 0x06 (`RXTX_CONFIG1`) enthält nur
  Mute und Formate, keine Quellwahl.
- An I2S IN-2 (Eingang 7) liegt `DA.IN.4`, die 4. Datenleitung vom DSP zum D/A-Wandler. Das wäre ein Signal
  **hinter** der Filterkette. Die Firmware wählt diesen Eingang aber nie.
- Die Firmware nutzt den zweiten CT7302 (I²C 0x22) als **Scanner**: Er schaltet in jedem Durchlauf der
  Hauptschleife zum nächsten Eingang, um zu melden, wo ein Signal anliegt (`events_spdif_mainloop`). Nur im
  Vierkanal-Modus (Pin `OPTION_SPDIF_4CH`) bleibt er fest auf dem optischen Eingang 2.
- **Folge,** sofern CT7302 Nr. 2 der Baustein an 0x22 ist (die Adress-Strappung ist im Plan nicht eindeutig
  ablesbar): Der S/PDIF-Ausgang trägt ein **Eingangssignal vor dem DSP**, und zwar wechselnd das des gerade
  gescannten Eingangs. Gefilterte Ausgänge kommen dort mit der heutigen Firmware nicht an.

**Messen:**
- Forge-JP1 auf „Ausgang“.
- Optischen Port 2 an den S/PDIF-Eingang des Scarlett. Optisch, falls sich die Optical-Buchse des Scarlett auf
  S/PDIF umstellen lässt; sonst über einen Toslink→Koax-Wandler an S/PDIF In (Koax).
- Im Scarlett die **Taktquelle auf S/PDIF** und die Rate auf die des Forge stellen (zuerst 96 kHz; die
  S/PDIF-Lock-Anzeige muss grün sein).
- Dann `run … --rate 96000 --spdif 9,10` (Eingangskanäle, unter denen der S/PDIF-Eingang in `devices` bzw. in
  Focusrite Control 2 erscheint).

Nach den Filtertests senkt das Werkzeug den Gain jedes Ausgangs einzeln um 6 dB:
- Ändert sich dabei S/PDIF L/R, liegt das Signal hinter der Kette, und der Bericht nennt den Ausgang.
- Ändert sich nichts, liegt es davor. Der Bericht zeigt das Ergebnis unter „S/PDIF-Ausgang“.

Mute eignet sich dafür nicht, es wirkt erst am D/A-Wandler.

## Was geprüft wird

| Test | Einstellung | Soll |
|---|---|---|
| eq1k, eq250, eq6k | EQ (Typ 1) +6 dB bei 1 kHz Q 1,41 / −9 dB bei 250 Hz Q 4 / +4 dB bei 6,3 kHz Q 0,7 | Formel der Firmware, ±1 dB, ±10° |
| ls100, hs8k | Tiefen-Shelf 100 Hz +6 dB, Höhen-Shelf 8 kHz −6 dB | dto. |
| hp80, lp3k, hpq | Hochpass 80 Hz und Tiefpass 3 kHz Butterworth, Hochpass 200 Hz Q 1,2 | dto. |
| ap1k | Allpass 2. Ordnung 1 kHz (nur Phase) | dto. |
| gain | `gain.out` −6 dB | flach −6 dB |
| phase | `phase.out` umgekehrt | 180° |
| delay | `delay` +96 Samples | 1,000 ms ±0,02 ms |
| mute | `mute.out` | mind. 40 dB leiser |

- **Filterplatz:** Filter landen in einem **leeren** Filterplatz je Ausgang (Typ 0), nie in einem der letzten zwei
  Plätze eines Kanals, die ein Loudness-Block nutzen kann. Hat ein Ausgang keinen freien Platz, entfallen dort die
  Filtertests.
- **Ausgewertet** wird je Sechstel-Oktave zwischen 40 Hz und 18 kHz, nur in Bändern mit mindestens 20 dB
  Rauschabstand. Tiefe Sperrbereiche (Soll unter −24 dB) zählen nicht.
- **Ein Ausgang mit Weiche** (z. B. Hochtöner mit Hochpass 3 kHz) wird nur dort geprüft, wo er Signal führt.

## Wenn etwas schiefgeht

- **Abbruch mitten im Lauf:** Das Werkzeug setzt die geschriebenen Werte im `finally` zurück. Weicht die Abstimmung
  danach ab, meldet es das:
  `python3 forge_filtertest.py restore --port COM5 results/<Datum>/tuning-before.cfg` schreibt die Sicherung zurück.
- **„Forge refused …“** bei einem Ausgang: Der Wert wird dort abgelehnt (z. B. Laufzeit am oberen Ende). Der Test
  zählt als Fehler, der Lauf geht weiter.
- **Brumm und Störlinien:** Masseschleife über die USB-Kabel. Die Auswertung ist relativ, einzelne Störlinien fallen
  heraus; bei Zweifeln Forge und PC an derselben Steckdosenleiste versorgen.

## Ergebnisse zurückschicken

Den Ordner `results/<Datum>/` als ZIP schicken: `report.html`, `results.json` und `tuning-before.cfg`.

Lizenz: CC BY-NC-SA 4.0, wie [forge-dsp-firmware](https://github.com/psych0d0g/forge-dsp-firmware). `forge_tuning.py`
ist `tools/c45b3-host/c45b3/tuning.py` aus diesem Repository.
