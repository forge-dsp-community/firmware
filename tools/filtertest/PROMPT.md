# Prompt für Claude (Claude Code o. ä.)

Diesen Text in eine neue Sitzung im entpackten Ordner `forge-filtertest` einfügen:

---

Ich möchte einen Forge DSP mit meiner Focusrite Scarlett 18i20 4th Gen durch den Filtertest in diesem Ordner
schicken. Lies zuerst `README.md` vollständig und halte dich daran.

Regeln, die immer gelten:

- Am Forge nur mit `forge_filtertest.py` arbeiten. Keine eigenen Befehle an den Forge senden, insbesondere nie
  `system.boot`, `system.reinit`, `system.hardware…`, kein Flashen, kein Firmware-Update, kein Werks-Reset.
- Vor `map` und `run` mich fragen, ob die Ausgänge des Forge nur am Interface hängen (keine Endstufen, keine
  Lautsprecher) und ob die 48-V-Phantomspeisung aller Eingänge aus ist. Erst nach meinem Ja weitermachen.
- Keine Pakete global installieren: ein virtuelles Environment im Ordner anlegen und `requirements.txt` verwenden.
- Meldet das Werkzeug am Ende „Abstimmung: … Werte weichen ab“, mich fragen, bevor du `restore` ausführst.

Ablauf:

1. Python-Umgebung einrichten, `python3 forge_filtertest.py selftest`, das muss „Selbsttest bestanden“ melden.
2. `devices` ausführen, das Scarlett finden. Den seriellen Port des Forge suchen (Windows: COM-Port „Silicon Labs
   CP210x“, macOS `/dev/cu.SLAB_USBtoUART` oder `/dev/cu.usbserial-*`, Linux `/dev/ttyUSB*`). Bei Unklarheit mich
   fragen.
3. Mit mir den Anschlussplan aus der README durchgehen und die Einstellungen in Focusrite Control 2 prüfen lassen
   (48 kHz, Line, 48 V aus, Routing Playback 3–8 → Line Out 3–8, kein Direct Monitor).
4. `check` ausführen und das Ergebnis mit mir besprechen: jeder Eingang mit Signal, keiner übersteuert, möglichst
   über 40 dB Rauschabstand. Gegebenenfalls sagen, wie ich den Gain am Scarlett ändern soll, oder `--level` anpassen.
   Wiederholen, bis es passt.
5. `map` ausführen und die ausgegebene `--map …` merken. Fehlt ein Ausgang („None“), mit mir klären, ob dort ein
   Kabel fehlt oder der Mixer keinen gespeisten Eingang auf diesen Ausgang legt.
6. `run … --map …` ausführen. Danach den Bericht `results/<Datum>/report.html` zusammenfassen: welche Tests an welchen
   Ausgängen fehlgeschlagen sind, mit den Abweichungen. Unterscheiden zwischen Messproblem (kein Signal, zu wenig
   Rauschabstand, Übersteuerung) und echter Abweichung des Forge.
7. Bei Messproblemen die Ursache beheben (mit mir) und die betroffenen Tests mit `--only` wiederholen.
8. Am Ende den Ergebnisordner als ZIP packen, damit ich ihn zurückschicken kann. Die Firmware-Version und das Modell
   aus dem Bericht nennen.

---
