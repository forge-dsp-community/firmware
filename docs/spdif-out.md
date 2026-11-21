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

## Gerätetest 21.11.2026 (96ELN PCB 4.0, Port 2 umgebaut, Firmware 2.9.1-7)

Ergebnis: **kein Ton am Empfänger** (Aktivlautsprecher mit DIR9001, nimmt 28–108 kHz; über dasselbe Kabel
von einer USB-Karte seit Monaten zuverlässig). Ursache offen.

Belegt:

- **`DA.IN.4` trägt an diesem Gerät die Ausgänge 1/2, nicht 7/8.** Testton nur auf einem Ausgang:
  Ausgang 1 → 0x7A = 0x45 (links Daten), Ausgang 2 → 0x25 (rechts Daten), Ausgänge 3–8 → 0xE5 (Stille),
  obwohl `system.vu.out` auf 7/8 −43 dBFS zeigte. Ob das für alle 96ELN PCB 4.0 gilt, ist offen.
- `DA.IN.4` liegt an I²S IN-2 beider CT7302: 0x20 mit `mixer.in=8` (Kanal 7) sieht dieselben Daten
  (0x7A = 0x05, G7/G8 = Ausgang 1/2).
- **Port 2 hängt am CT7302 0x22**: Mit `FORCE_STOP_SPDIF_TX` (Register 0x10 Bit 3) an 0x22 geht die LED des
  TOTX147PL aus, an 0x20 nicht.
- Sender laut Registern frei: 0x10 = 0xC0 (kein Stopp), 0x11 = 0x00 (kein Power-down), 0x06 = 0x78 (kein
  FORCE_MUTE), 0x1E = 0xCF (Takt des S/PDIF-Senders an), 0x7A Bit 2 = 1 (12-MHz-Quarz), Ratenverhältnis
  0x7D–0x7F = 2,0 (96 kHz vom DSP).
- Lautstärke des Bausteins 0x22: Register 9/10 = 0x24 = −18 dB (Startwert der Firmware).
- **Register 5 Bits 6:4 (`SW_SEL_SRC_RATIO_124816`) bleiben auf 001 = 2×**: `apply_rate()` setzt nur Bits 3:0.
  Laut Datenblatt sendet „48“ damit 96 kHz und „96“ 192 kHz (über dem, was TOTX147PL und viele Empfänger
  nehmen). Von Hand 1× gesetzt (0x03, 0x02): weiterhin kein Ton. Fix vorgesehen, sobald der Ausgang läuft.
- Ohne Ergebnis von Hand probiert (je mit Kennton −26 dBFS am Ausgang): 44,1/48/96 kHz, Modus 0
  (Referenztakt), Kanalstatus Byte 3 = 0x32 (48 kHz statt 0x31 „nicht angegeben“), De-Jitter an (0x13 = 0xE0).
- Optischer Port 1 = S/PDIF IN-2 von 0x20 (`mixer.in=3`), Empfang funktioniert (siehe hardware.md).
- Der DAC CS4385 antwortete nach dem Flashen nicht (Fehler 19, rote LED); nach einem Stromlos-Start wieder da.

- **Optische Schleife Port 2 → Port 1** (Port 1 = S/PDIF IN-2 von 0x20, mit einer externen Quelle geprüft):
  0x20 erkennt keinen Strom. 0x7A bleibt 0xE4, CDR-Frequenz (0x80–0x82), Eingangs-PLL (0x89) und der
  gespeicherte Kanalstatus (0x72–0x76, noch der der externen Quelle) ändern sich nicht, mit und ohne Testton.
  Durchprobiert an 0x22: Modus 0, 1, 3, 4, 5, 6 (Register 4 Bits 6:4), Faktor 1×/2×, Software-Modus
  (Register 6 Bit 0). 0x22 meldet dabei Ausgangs-PLL gelockt (0x8A = 0xA3, 48 kHz; Modus 0: 0xA6, 96 kHz).
- Firmware-Init (`ct7302_init_sub`) gegen das Datenblatt geprüft: nichts, was den Sender stoppt oder abschaltet.
  `MISCELLANEOUS1` (0x4D) löscht `VOLUME_SHIFT_GAIN` (+18 dB → +0 dB), mit 0x24 an Register 9/10 also netto
  −18 dB am Ausgang. Leiser, aber nicht stumm; beim Fix mit ansehen.
- Schaltplan Grundplatine V40 S. 3: `OUT.SPDIF.1` geht ohne Bauteil vom OUT_SPDIF-Pin an JP1 Pin 8.

- **Pegel an JP1 Pad 8** (= TOTX147PL Pin 3) gegen GND: 1,38 V DC, also ein schnell schaltendes Signal, kein
  festhängender Pin. Modul: TOTX147PL.
- **Elektrische Schleife** JP1 Pad 8 → Pad 4 (`OUT.SPDIF.1` → über R23 an S/PDIF IN-3 von 0x20, `mixer.in=4`):
  0x20 lockt nicht, Status wie im Leerlauf. Das Sendemodul ist damit nicht die Ursache.
- **0x22 auf Werkseinstellung** (`RESTART_ALL`, Register 0x9F Bit 0, danach nur Register 4 = 0x37 und 5 = 0x03):
  0x20 lockt weder über die optische Schleife noch über die Drahtbrücke. Die Firmware-Init ist damit nicht die
  Ursache.
- Vollvergleich der Register 0x01–0x63 beider CT7302 mit den Datenblatt-Vorgaben: Abweichungen nur dort, wo die
  Firmware beide gleich setzt; Takt-/MPLL-Teil (0x1C–0x2A) auf Vorgabe.

Folgerung: Der Sender von 0x22 hält sich für gelockt (48 kHz) und schaltet seinen Pin schnell, aber das Signal ist
für keinen Empfänger S/PDIF (DIR9001, eigener CT7302, optisch und elektrisch). Firmware-Init und Sendemodul sind
ausgeschlossen. Nächster Schritt braucht ein Messgerät am Pin: Frequenz und Form des Signals (Oszilloskop oder
Logikanalysator mit ≥ 50 MS/s, z. B. RP2040/RP2350 mit LogicAnalyzer-Firmware). Erwartung für S/PDIF bei 48 kHz:
Biphase-Mark mit 6,144 MBaud (Pulse 81/163/244 ns).

Nächster Schritt: elektrische Schleife ohne Optik, Drahtbrücke JP1 Pad 8 (`OUT.SPDIF.1`) → Pad 4
(`IN.SPDIF.3`, über R23 an S/PDIF IN-3 von 0x20; in Stellung O frei). Dann liest 0x20 mit `mixer.in=4` den
Strom von 0x22: Lock, Rate (0x7D–0x7F), Daten (0x7A, G7/G8).
