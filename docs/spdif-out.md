# S/PDIF-Ausgang hinter dem DSP

Ab Firmware 2.7.0-beta.1 (#82, #83) kann der optische Port 2 das fertig bearbeitete Signal der Ausgänge 7/8
senden: nach Mixer, Filtern & EQ, Laufzeit und Pegel. Im Original trug der Ausgang nur, was der zweite CT7302 gerade
abtastete, also wechselnd die Eingänge vor dem DSP.

**Stand:** Die Register sind am DEV-Forge geprüft (17.11.2026). Das Signal am Ausgang ist noch nicht gemessen, weil
dort ab Werk ein Empfänger sitzt.

## 1. Hardware umbauen

Ab Werk ist Port 2 ein Eingang. Für den Ausgang:

1. Den Empfänger **TORX147PL** an Port 2 gegen einen Sender **TOTX147PL** tauschen (3,3 V, gleiche Bauform).
2. Den Lötjumper **„sec.spdif“ (JP1)** von **I** auf **O** umsetzen:
   - Brücken 2–4, 1–3 und 5–7 entfernen,
   - Brücken 1–2, 3–5 und 7–8 setzen.
   - Nie gleichzeitig 1–3 und 3–5 brücken: Das verbindet 3,3 V mit GND.

Pinbelegung, Legende O/B/I und Foto: [hardware.md](hardware.md#optische-anschlüsse-und-lötjumper-secspdif-jp1).

Nach dem Umbau leuchtet Port 2 dauerhaft rot, sobald der CT7302 Daten sendet.

## 2. Einstellen

In der GUI unter Geräteeinstellungen › S/PDIF-Ausgang:

| Einstellung | Befehl | Werte |
|---|---|---|
| Quelle | `set system.spdif.out` | 0 = Eingänge abtasten (wie im Original), 1 = Signal hinter dem DSP |
| Abtastrate | `set system.spdif.rate` | kHz: 0 = wie der DSP (Referenztakt), 32, 44 (44,1), 48, 88 (88,2), 96, 176 (176,4), 192 |

- Die Rate gilt nur, solange „Signal hinter dem DSP“ gewählt ist.
- Beide Werte stehen im Port-Bereich des EEPROM (0x0F14, 0x0F15). Ein Werks-Reset setzt sie zurück.
- Abgelehnt werden die Befehle ohne zweiten CT7302 (0x22) und mit der Vierkanal-Option, weil Port 2 dann Eingang ist.

## 3. Signal auf die Ausgänge 7/8 legen

Der Ausgang trägt die DSP-Ausgänge 7 (links) und 8 (rechts).

- **Mixer:** Was auf 7/8 geroutet ist, kommt am S/PDIF an. Ohne Routing ist der Ausgang still, obwohl der Sender
  leuchtet.
- **Namen:** Die GUI nennt die Ausgänge im DSP-Modus „S/PDIF L/R“; eigene Namen gehen vor.
- **Stummschalten:** `mute.out` wirkt im D/A-Wandler, also nicht auf S/PDIF. Für Stille den Pegel herunterziehen.
- **Ansicht „nur digital“:** blendet in Mixer, Filter & EQ und Laufzeit die Ausgänge 1–6 aus. Das ändert nur die
  Ansicht, nicht die Einstellungen.
- **Pegel prüfen:** `get system.vu.out` zweimal abfragen; die Werte 7/8 × 0,5 ergeben dB über −127,5 dBFS.

## Grenzen

- **Rate und Empfänger:** Viele Empfänger im Auto nehmen nur 44,1 oder 48 kHz an.
- **Rate und Sender:** Der CT7302 sendet bis 192 kHz. Ob der TOTX147PL das mitmacht, ist noch nicht geprüft; mit
  44,1 und 48 kHz ist man auf der sicheren Seite.
- **Rate gilt für den ganzen Baustein:** Die Abtastrate gilt für alle Ausgänge des CT7302 an 0x22, also auch für seinen
  I²S-Ausgang. Wohin dieser auf der Platine geht, ist nicht geprüft. Verlässt man den DSP-Modus, setzt die Firmware
  die Startwerte zurück.
- **Fehlersuche:** Leuchtet Port 2 nicht, liegt es an Modul oder Jumper, nicht an der Firmware. Das Register 0x7A des
  Bausteins (`get i2c.1.34.122`) zeigt mit gesetzten Bits 7:5 an, dass am gewählten Eingang Stille anliegt.
