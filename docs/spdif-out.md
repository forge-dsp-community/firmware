# S/PDIF-Ausgang hinter dem DSP

Ab Firmware 2.7.0-beta.1 (#82, #83) kann der optische Port 2 ein Signal aus dem DSP senden. Im Original trug der
Ausgang nur, was der zweite CT7302 gerade abtastete, also wechselnd die Eingänge vor dem DSP.

Der Abgriff liegt **im ersten DSP-Kern**, nicht hinter den Ausgangsketten: Der S/PDIF-Ausgang trägt die
Mixer-Ausgänge 1/2 von Kern 1, also Eingangsdelay, Eingangspegel, Mixer und die beiden Summen-Ketten mit je
72 Biquads. Filter, Laufzeit, Pegel und Mute der Ausgangskanäle (Kern 2/3) wirken nicht auf S/PDIF (Abschnitt 3).

**Stand:** Am 96ELN PCB 4.0 gemessen und gehört (21.11.2026, Firmware 2.9.1-7): Ton am Empfänger, Filter der
Summen-Ketten wirken, `gain.out` wirkt nicht.

## 1. Hardware umbauen

Ab Werk ist Port 2 ein Eingang. Für den Ausgang:

1. Den Empfänger **TORX147PL** an Port 2 gegen einen Sender **TOTX147PL** tauschen (3,3 V, gleiche Bauform).
2. Die **zwei Abblockkondensatoren an Pin 3 von Port 2 entfernen** (Schaltplan Grundplatine V40 S. 3, je einer
   gegen GND). Beim TORX ist Pin 3 Vcc, beim TOTX der Signaleingang. Bleiben sie drin, glätten sie `OUT.SPDIF.1`
   zu einer Gleichspannung: Die LED leuchtet dauerhaft, an JP1 Pad 8 liegen etwa 1,4 V, kein Empfänger lockt.
   Finden per Durchgangsprüfung: eine Seite an JP1 Pad 7 (bzw. an der Brücke 7–8), die andere an GND. Am 96ELN
   PCB 4.0 sind es die beiden hochkant stehenden Kondensatoren direkt neben dem Modul von Port 2, auf Höhe der
   Brücke 7–8. Die zwei Kondensatoren an Pin 2 (in Stellung O Vcc des TOTX) bleiben.
3. Den Lötjumper **„sec.spdif“ (JP1)** von **I** auf **O** umsetzen:
   - Brücken 2–4, 1–3 und 5–7 entfernen,
   - Brücken 1–2, 3–5 und 7–8 setzen.
   - Nie gleichzeitig 1–3 und 3–5 brücken: Das verbindet 3,3 V mit GND.

Pinbelegung, Legende O/B/I und Foto: [hardware.md](hardware.md#optische-anschlüsse-und-lötjumper-secspdif-jp1).

Port 2 leuchtet danach dauerhaft rot, sobald der CT7302 sendet. Das Leuchten allein beweist noch keinen gültigen
Strom (siehe Punkt 2).

## 2. Einstellen

In der GUI unter Geräteeinstellungen › S/PDIF-Ausgang:

| Einstellung | Befehl | Werte |
|---|---|---|
| Quelle | `set system.spdif.out` | 0 = Eingänge abtasten (wie im Original), 1 = Signal aus dem DSP |
| Abtastrate | `set system.spdif.rate` | kHz: 0 = wie der DSP (Referenztakt), 32, 44 (44,1), 48, 88 (88,2), 96, 176 (176,4), 192 |

- Die Rate gilt nur, solange „Signal aus dem DSP“ gewählt ist. 48 kHz am Gerät geprüft (Empfänger lockt bei 48 kHz).
- Beide Werte stehen im Port-Bereich des EEPROM (0x0F14, 0x0F15). Ein Werks-Reset setzt sie zurück.
- Abgelehnt werden die Befehle ohne zweiten CT7302 (0x22) und mit der Vierkanal-Option, weil Port 2 dann Eingang ist.

## 3. Signal auf S/PDIF legen

Der CT7302 an 0x22 liest `DA.IN.4`. Das ist Datenausgang DO7/DO8 von **Kern 1**, also die Mixer-Zeilen 3/4
(`mixer.P.3.G` = links, `mixer.P.4.G` = rechts, in der GUI „Ausgang 1/2“). Von dort geht das Signal zusätzlich zu
Kern 3, der für Kanal 1/2 Filter, Laufzeit und Pegel rechnet und an den D/A-Wandler gibt. Diese Bearbeitung sieht
der S/PDIF-Ausgang nicht.

```
Kern 1: Eingänge → Delay → Pegel → Mixer ─┬─ Summe A → Kette 1 (72 Biquads) ─┐
                                          └─ Summe B → Kette 2 (72 Biquads) ─┤
        Mixer-Zeile 3/4 = Mischung + Kette 1/2 (Spalte G9/G10) ◄──────────────┘
                 │
                 ├─► DA.IN.4 ─► CT7302 0x22 ─► S/PDIF (Port 2)
                 └─► Kern 3: Filter, Delay, Pegel Kanal 1/2 ─► D/A-Wandler
```

Stereo mit Filtern auf S/PDIF:

- **Summe A** (`mixer.P.1.G`) nur aus dem linken Eingang, **Summe B** (`mixer.P.2.G`) nur aus dem rechten.
- **Ausgang 1** ← Kette 1 (`mixer.P.3.9 = 100`), **Ausgang 2** ← Kette 2 (`mixer.P.4.10 = 100`).
- Die direkten Wege der Eingänge in die Zeilen 3/4 auf 0, sonst mischt sich ungefiltertes Signal dazu.
- Filter & EQ auf **Summe A = links, Summe B = rechts** (je 72 Biquads).

Was wirkt und was nicht:

| | auf S/PDIF |
|---|---|
| Eingangsdelay, Eingangspegel, Mixer, Summen-Ketten (Filter & EQ der Summen) | wirkt |
| Filter, Laufzeit, Phase, Pegel (`gain.out`) der Ausgänge 1–8 | wirkt nicht (gemessen: alle `gain.out` auf 0, Ton bleibt) |
| `mute.out` | wirkt nicht (Mute im D/A-Wandler) |
| Lautstärkeblock auf die Summen A/B (#71) | wirkt (skaliert die Mixer-Koeffizienten in Kern 1) |

Für Stille den Mixer oder die Summen herunterziehen. Laufzeit je Kanal gibt es auf diesem Weg nur über das
Eingangsdelay.

## Grenzen

- **Rate und Empfänger:** Viele Empfänger im Auto nehmen nur 44,1 oder 48 kHz an.
- **Rate und Sender:** Der CT7302 sendet bis 192 kHz. Ob der TOTX147PL das mitmacht, ist nicht geprüft; mit
  44,1 und 48 kHz ist man auf der sicheren Seite.
- **Rate gilt für den ganzen Baustein:** Die Abtastrate gilt für alle Ausgänge des CT7302 an 0x22, also auch für seinen
  I²S-Ausgang (`CT7302.2.SDATA`, nur in der Vierkanal-Option genutzt). Verlässt man den DSP-Modus, setzt die Firmware
  die Startwerte zurück.
- **Versorgung:** Bei Unterspannung (Netzteil auf 10 V eingebrochen) fallen die Analogeingänge aus, Digitalteil, DSP
  und S/PDIF laufen weiter. Am Ausgang kommen dann nur Fetzen beim Schalten an.

## Fehlersuche

| Befund | Bedeutung |
|---|---|
| Port 2 dunkel | Modul oder Jumper |
| Port 2 leuchtet, kein Empfänger lockt, ~1,4 V DC an JP1 Pad 8 | Abblockkondensatoren an Pin 3 noch drin (Umbau Schritt 2) |
| 0x7A an 0x22 (`get i2c.1.34.122`) Bits 7:5 gesetzt | am gewählten Eingang des CT7302 nur Nullen: nichts auf Mixer-Zeile 3/4 geroutet |
| Mit `FORCE_STOP_SPDIF_TX` (`set i2c.1.34.16=200`, zurück mit 192) geht die LED aus | Port 2 hängt am Sender von 0x22 |

Schleifentest ohne externes Gerät: Port 2 optisch auf Port 1 (= S/PDIF IN-2 von 0x20) stecken, `mixer.in=3`.
Lockt 0x20, zeigt 0x89 die Rate (`0xA3` = 48 kHz) und 0x72–0x76 den Kanalstatus des Senders (Register 0x0B–0x0F
von 0x22, ab Werk `00 00 00 31 01`). G7/G8 nicht auf Ausgänge routen, sonst Rückkopplung.

## Gerätetest 21.11.2026 (96ELN PCB 4.0, Firmware 2.9.1-7-g6805a5d)

- Port 2 hängt am CT7302 0x22: `FORCE_STOP_SPDIF_TX` an 0x22 schaltet die LED des TOTX aus, an 0x20 nicht.
- Mit den Kondensatoren an Pin 3: 1,38 V DC an JP1 Pad 8, kein Lock am DIR9001 (Aktivlautsprecher), an 0x20 über
  die optische Schleife und über eine Drahtbrücke Pad 8 → Pad 4 (IN.SPDIF.3), auch mit 0x22 in Werkseinstellung
  (`RESTART_ALL`). Nach dem Auslöten: 0x20 lockt über die Schleife bei 48 kHz und liest den Kanalstatus von 0x22.
- `DA.IN.4` = Mixer-Zeile 3/4 von Kern 1: Testton nur auf Zeile 3 → 0x7A = 0x45 (links Daten), nur auf Zeile 4 →
  0x25 (rechts Daten), Zeilen 5–10 → 0xE5. Am Lautsprecher wirken nur Filter der Summen-Ketten, `gain.out` auf 0
  ändert nichts.
- `DA.IN.4` liegt an I²S IN-2 beider CT7302 (0x20 mit `mixer.in=8` sieht dieselben Daten).
- Register 5 Bits 6:4 (`SW_SEL_SRC_RATIO_124816`) bleiben bei `apply_rate()` auf 001; der Empfänger lockt trotzdem
  bei 48 kHz, in Modus 3 also ohne Wirkung.
- `ct7302_init_sub()` löscht in 0x4D `VOLUME_SHIFT_GAIN` (+18 dB → +0 dB); mit Register 9/10 = 0x24 gibt 0x22 damit
  netto −18 dB aus. Nicht geändert, offen.
- Optischer Port 1 = S/PDIF IN-2 von 0x20 (`mixer.in=3`), siehe hardware.md.
