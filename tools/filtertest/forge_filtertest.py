#!/usr/bin/env python3
"""Filter and output test of a Forge DSP with a multichannel audio interface (all 8 analog outputs).

The interface plays periodic pink noise into the analog inputs of the Forge and records the 8 analog
outputs at the same time, in ONE full-duplex stream (same clock, constant latency over the whole
run). For every test case the tool writes one setting to every output (a filter in a free biquad
slot, gain, polarity, delay, mute), measures and compares the change against the firmware formulas:

    measured change  = H(with setting) / H(baseline)       per output, magnitude and phase
    expected change  = response of the biquad / the setting

Nothing else of the tuning is touched: each setting is set back right after its measurement, the
whole tuning is exported before and compared after the run (backup file in the result folder).

    python3 forge_filtertest.py selftest
    python3 forge_filtertest.py devices
    python3 forge_filtertest.py check  --port COM5 --device "Scarlett 18i20"
    python3 forge_filtertest.py map    --port COM5 --device "Scarlett 18i20"
    python3 forge_filtertest.py run    --port COM5 --device "Scarlett 18i20" [--map 1,2,3,4,5,6,7,8]
    python3 forge_filtertest.py restore --port COM5 results/<run>/tuning-before.cfg

See README.md (connection plan, settings of the interface, safety).
License: CC BY-NC-SA 4.0, as forge-dsp-firmware (forge_tuning.py is tools/c45b3-host/c45b3/tuning.py of it).
"""
import argparse
import cmath
import datetime
import html
import json
import math
import os
import re
import sys
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import forge_tuning as tuning  # noqa: E402  (export/restore over the text protocol, from forge-dsp-firmware)

FS = 48000            # sample rate of the interface
N = 65536             # period of the test signal (1.37 s at 48 kHz)
FS_DSP = 96000        # the Forge computes its biquads at 96 kHz
PERIODS = 5           # averaged periods per measurement
SETTLE = 2.0          # s after a change before measuring (fade of clickless, mute ramps)
TOL_DB, TOL_DEG, TOL_MS = 1.0, 10.0, 0.02


# --- Forge ---------------------------------------------------------------------------------------
class Forge:
    def __init__(self, port, baud=38400):
        import serial
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.app = tuning.App(self.ser, baud, log=print)
        self.written = {}   # register -> value before the test (for the restore)

    def get(self, reg):
        for l in self.app.query("get.1 " + reg, quiet=0.3, limit=5):
            m = tuning.ANSWER.match(l)
            if m and m.group(1) == reg:
                return m.group(2)
        return None

    def get_all(self, pattern):
        out = {}
        for l in self.app.query("get.1 " + pattern, quiet=0.5, limit=60):
            m = tuning.ANSWER.match(l)
            if m:
                out[m.group(1)] = m.group(2)
        return out

    def set(self, reg, value, remember=True):
        if remember and reg not in self.written:
            old = self.get(reg[:-5] if reg.endswith(".calc") else reg)
            if old is not None:
                self.written[reg] = old
        body = f"{reg}={value}"
        ans = self.app.query(f"set.1 {body};{tuning.checksum(body)}", quiet=0.15, limit=5)
        ok = any(" ok" in l for l in ans)
        if not ok:
            raise RuntimeError(f"Forge refused {body}: {' '.join(a.strip() for a in ans) or 'no answer'}")

    def wait_fades(self, timeout=30.0):
        """Port firmware: wait until no clickless fade runs (get biquad.clickless.busy = 0). The fades
        share the main loop, eight large changes take up to about 16 s; a fixed wait measured
        intermediate filters (Prüfung 19.11.2026). Firmware without the command: return at once."""
        end = time.monotonic() + timeout
        while True:
            busy = self.get("biquad.clickless.busy")
            if busy is None or not busy.strip().isdigit() or int(busy) == 0:
                return
            if time.monotonic() > end:
                print(f"   Hinweis: nach {timeout:.0f} s laufen noch {busy} Überblendungen")
                return
            time.sleep(0.25)

    def undo(self, reg):
        old = self.written.pop(reg, None)
        if old is not None:
            self.set(reg, old, remember=False)

    def undo_all(self):
        for reg in list(self.written):
            self.undo(reg)


def identify(f):
    info = {k: f.get(k) for k in ("system.key.info", "system.firmware", "system.firmware.port")}
    if not info["system.key.info"]:
        raise SystemExit("No answer from the Forge (port, cable, 38400 Bd; is another program using the port?)")
    return info


def free_slots(f, outs):
    """a bypassed biquad slot per output, not one of the last two (a loudness block may use them)"""
    bq = f.get_all("biquad.*")
    per = {}
    for reg, v in bq.items():
        m = re.fullmatch(r"biquad\.(\d+)\.(\d+)", reg)
        if m:
            per.setdefault(int(m.group(1)), {})[int(m.group(2))] = v
    slots = {}
    for o in outs:
        s = per.get(o, {})
        cand = sorted(s)[:-2] if len(s) > 2 else []
        # only empty places (type 0 or never written); a switched-off filter keeps its values
        free = [n for n in cand if s[n].startswith("[0,") or s[n].startswith("[255,")]
        if free:
            slots[o] = free[0]
    return slots


# --- signal and audio ------------------------------------------------------------------------------
def pink_period(db_rms=-18.0, seed=7):
    rng = np.random.default_rng(seed)
    f = np.fft.rfftfreq(N, 1 / FS)
    mag = np.zeros_like(f)
    band = (f >= 15) & (f <= 21000)
    mag[band] = 1 / np.sqrt(f[band])
    X = mag * np.exp(2j * np.pi * rng.random(len(f)))
    x = np.fft.irfft(X, N)
    return x / np.sqrt(np.mean(x ** 2)) * 10 ** (db_rms / 20)


class Duplex:
    """one full-duplex stream: plays the period endlessly on the play channels, records all inputs"""

    def __init__(self, device, play, rec, x):
        import sounddevice as sd
        self.sd, self.x, self.play, self.rec = sd, x.astype(np.float32), [c - 1 for c in play], [c - 1 for c in rec]
        dev = sd.query_devices(device)
        self.nout, self.nin = max(self.play) + 1, max(self.rec) + 1
        if self.nout > dev["max_output_channels"] or self.nin > dev["max_input_channels"]:
            raise SystemExit(f"{dev['name']}: {dev['max_output_channels']} out / {dev['max_input_channels']} in, "
                             f"needed {self.nout} / {self.nin}")
        self.pos = 0
        self.buf = np.zeros((FS * 60, len(self.rec)), np.float32)   # last 60 s
        self.written = 0
        self.lock = threading.Lock()
        self.peak = np.zeros(len(self.rec))
        self.stream = sd.Stream(device=device, samplerate=FS, channels=(self.nin, self.nout), dtype="float32",
                                callback=self.cb, blocksize=1024, latency="high")

    def cb(self, indata, outdata, frames, t, status):
        idx = (self.pos + np.arange(frames)) % N
        outdata[:] = 0
        for c in self.play:
            outdata[:, c] = self.x[idx]
        self.pos += frames
        with self.lock:
            rows = indata[:, self.rec]
            i = self.written % len(self.buf)
            j = min(frames, len(self.buf) - i)
            self.buf[i:i + j] = rows[:j]
            self.buf[:frames - j] = rows[j:]
            self.written += frames
            self.peak = np.maximum(self.peak, np.abs(rows).max(0))

    def __enter__(self):
        self.stream.start()
        time.sleep(1.0)
        return self

    def __exit__(self, *a):
        self.stream.stop()
        self.stream.close()

    def take(self, periods=PERIODS):
        """recorded periods aligned to the play position (frame k of the recording = frame k of the
        stream): -> array (periods, N, channels). Same latency for the whole stream."""
        with self.lock:
            end = self.written
        start = (end // N - periods - 1) * N
        if start < 0 or end - start > len(self.buf):
            raise RuntimeError("recording too short")
        idx = np.arange(start, start + periods * N) % len(self.buf)
        with self.lock:
            seg = self.buf[idx].copy()
        return seg.reshape(periods, N, -1)

    def wait_periods(self, periods=PERIODS):
        time.sleep((periods + 1.2) * N / FS)

    def measure(self, settle=SETTLE, periods=PERIODS):
        time.sleep(settle)
        self.wait_periods(periods)
        seg = self.take(periods)
        X = np.fft.rfft(self.x.astype(np.float64))
        Y = np.fft.rfft(seg.astype(np.float64), axis=1)            # (periods, bins, ch)
        use = np.abs(X) > np.abs(X).max() * 1e-6                   # outside the noise band: no value
        H = np.where(use[None, :, None], Y / np.where(use, X, 1)[None, :, None], 0)
        Hm = H.mean(0)
        noise = (np.abs(H - Hm[None]) ** 2).mean(0) / periods      # variance of the mean
        return Hm, noise

    def reset_peak(self):
        with self.lock:
            self.peak[:] = 0


# --- models ------------------------------------------------------------------------------------------
def bq_coeffs(t, F, g_db10, q1000, b=0):
    """the biquad formulas of the firmware (libs/dsp/tas3108, as tests/test_cmd.py bq_reference)"""
    w = 2 * math.pi * F / FS_DSP
    Qv, G = q1000 / 1000.0, g_db10 / 10.0
    P = 10 ** (G / 40.0)
    co, sn = math.cos(w), math.sin(w)
    al = lambda q: sn / (2 * q)
    c = {}
    if t == 1:
        a = al(Qv)
        c = dict(A0=1 + a / P, A1=2 * co, A2=-(1 - a / P), B0=1 + a * P, B1=-2 * co, B2=1 - a * P)
    elif t in (9, 10, 11, 12, 13):
        q = {9: 0.7071067811865476, 10: 0.5773502691896257, 11: 0.5, 12: 1.0, 13: Qv}[t]
        a = al(q)
        c = dict(B0=(1 + co) / 2, B1=-(1 + co), B2=(1 + co) / 2, A0=1 + a, A1=2 * co, A2=-(1 - a))
    elif t in (3, 4, 5, 6, 7):
        q = {3: 0.7071067811865476, 4: 0.5773502691896257, 5: 0.5, 6: 1.0, 7: Qv}[t]
        a = al(q)
        c = dict(B0=(1 - co) / 2, B1=1 - co, B2=(1 - co) / 2, A0=1 + a, A1=2 * co, A2=-(1 - a))
    elif t in (14, 15):
        b1, b2 = P + 1, P - 1
        b3, b4 = b1 * co, b2 * co
        bso = math.sqrt(P) / Qv * sn
        if t == 15:
            c = dict(B0=P * ((b1 + b4) + bso), B1=-2 * (P * (b2 + b3)), B2=P * ((b1 + b4) - bso),
                     A0=(b1 - b4) + bso, A1=-2 * (b2 - b3), A2=-((b1 - b4) - bso))
        else:
            c = dict(B0=P * ((b1 - b4) + bso), B1=2 * (P * (b2 - b3)), B2=P * ((b1 - b4) - bso),
                     A0=(b1 + b4) + bso, A1=2 * (b2 + b3), A2=-((b1 + b4) - bso))
    elif t == 20:
        a = al(Qv)
        c = dict(A0=1 + a, B2=1 + a, A1=2 * co, B1=-2 * co, B0=1 - a, A2=-(1 - a))
    else:
        c = dict(A0=1, A1=0, A2=0, B0=1, B1=0, B2=0)
    a0 = c["A0"]
    return c["B0"] / a0, c["B1"] / a0, c["B2"] / a0, c["A1"] / a0, c["A2"] / a0


def bq_resp(coef, f):
    b0, b1, b2, a1, a2 = coef
    z1 = np.exp(-2j * np.pi * f / FS_DSP)
    return (b0 + b1 * z1 + b2 * z1 * z1) / (1 - a1 * z1 - a2 * z1 * z1)


# test cases: (id, title, kind, value, model(f) -> complex)
def cases(port_fw):
    def bq(t, F, g, q):
        coef = bq_coeffs(t, F, g, q)
        return lambda f: bq_resp(coef, f)
    c = [
        ("eq1k", "EQ 1 kHz +6 dB, Q 1,41", "bq", [1, 1000, 60, 1414, 0, 128], bq(1, 1000, 60, 1414)),
        ("eq250", "EQ 250 Hz −9 dB, Q 4", "bq", [1, 250, -90, 4000, 0, 128], bq(1, 250, -90, 4000)),
        ("eq6k", "EQ 6,3 kHz +4 dB, Q 0,7", "bq", [1, 6300, 40, 700, 0, 128], bq(1, 6300, 40, 700)),
        ("ls100", "Tiefen-Shelf 100 Hz +6 dB", "bq", [14, 100, 60, 707, 0, 128], bq(14, 100, 60, 707)),
        ("hs8k", "Höhen-Shelf 8 kHz −6 dB", "bq", [15, 8000, -60, 707, 0, 128], bq(15, 8000, -60, 707)),
        ("hp80", "Hochpass 80 Hz Butterworth 12 dB/Okt.", "bq", [9, 80, 0, 707, 0, 128], bq(9, 80, 0, 707)),
        ("lp3k", "Tiefpass 3 kHz Butterworth 12 dB/Okt.", "bq", [3, 3000, 0, 707, 0, 128], bq(3, 3000, 0, 707)),
        ("hpq", "Hochpass 200 Hz, Q 1,2 (Typ 13)", "bq", [13, 200, 0, 1200, 0, 128], bq(13, 200, 0, 1200)),
        ("ap1k", "Allpass 2. Ordnung 1 kHz, Q 0,7 (Phase)", "bq", [20, 1000, 0, 707, 0, 128], bq(20, 1000, 0, 707)),
        ("gain", "Gain −6 dB", "gain", -6.0, lambda f: np.full(len(f), 10 ** (-6 / 20), complex)),
        ("phase", "Polarität umgekehrt", "phase", 1, lambda f: np.full(len(f), -1.0 + 0j)),
        ("delay", "Laufzeit +1,00 ms (96 Samples)", "delay", 96, lambda f: np.exp(-2j * np.pi * f * 0.001)),
        ("mute", "Mute", "mute", 1, None),
    ]
    return c


# --- evaluation --------------------------------------------------------------------------------------
def bands(lo=40.0, hi=18000.0, per_oct=6):
    k = np.arange(math.floor(per_oct * math.log2(lo / 1000)), math.ceil(per_oct * math.log2(hi / 1000)) + 1)
    c = 1000 * 2.0 ** (k / per_oct)
    return [(x * 2 ** (-0.5 / per_oct), x, x * 2 ** (0.5 / per_oct)) for x in c]


def band_values(f, Hb, Hn, noise_b, model=None):
    """per band: complex ratio Hn/Hb weighted with |Hb|^2, SNR of the baseline, model averaged alike"""
    out = []
    w = np.abs(Hb) ** 2
    for lo, c, hi in bands():
        m = (f >= lo) & (f < hi)
        if m.sum() < 8:
            continue
        if np.sum(w[m]) <= 0:
            continue
        r = np.sum(Hn[m] * np.conj(Hb[m])) / np.sum(w[m])
        snr = 10 * np.log10(np.sum(w[m]) / max(np.sum(noise_b[m]), 1e-30))
        mo = np.sum(model(f[m]) * w[m]) / np.sum(w[m]) if model else None
        out.append((c, r, snr, mo))
    return out


def judge(case, vals, f, Hb, Hn):
    cid, title, kind, value, model = case
    good = [(c, r, s, mo) for c, r, s, mo in vals if s >= 20]
    if kind == "mute":
        p = [abs(r) for c, r, s, mo in good]
        att = -20 * math.log10(max(np.median(p), 1e-9)) if p else 0
        return att >= 40 or (good and max(p) < 0.02), f"{att:.0f} dB leiser", {}
    if not good:
        return False, "kein Signal an diesem Ausgang (Rauschabstand < 20 dB in allen Bändern)", {}
    if kind == "delay":
        # coarse: peak of the cross-correlation of the change; fine: residual phase of the bands
        C = Hn * np.conj(Hb)
        r = np.fft.irfft(C, N)
        p = int(np.argmax(np.abs(r)))
        p = p - N if p > N // 2 else p
        tc = p / FS
        # bins of the bands with enough signal; the residual after the coarse delay is below one
        # sample at 48 kHz (< 180° up to 24 kHz), so no unwrapping is needed
        m = np.zeros(len(f), bool)
        for lo, c, hi in bands():
            if any(abs(c - g[0]) < 1e-6 for g in good):
                m |= (f >= lo) & (f < hi)
        wt = np.abs(Hb[m]) ** 2
        res = np.angle(C[m] * np.exp(2j * np.pi * f[m] * tc))
        tf = -np.sum(wt * f[m] * res) / np.sum(wt * f[m] ** 2) / (2 * math.pi)
        ms = (tc + tf) * 1000
        return abs(ms - value / FS_DSP * 1000) <= TOL_MS, f"{ms:.3f} ms (Soll {value / FS_DSP * 1000:.3f})", {}
    worst_db, worst_deg, at = 0.0, 0.0, None
    checked = 0
    for c, r, s, mo in good:
        if abs(mo) < 10 ** (-24 / 20):     # deep stopband of the model: too close to the noise
            continue
        if 20 * math.log10(abs(r) + 1e-12) + s < 20:   # the new level close to the noise
            continue
        ddb = 20 * math.log10(abs(r) / abs(mo))
        ddeg = abs((math.degrees(cmath.phase(r) - cmath.phase(mo)) + 180) % 360 - 180)
        checked += 1
        if abs(ddb) > abs(worst_db):
            worst_db, at = ddb, c
        worst_deg = max(worst_deg, ddeg)
    if not checked:
        return False, "keine auswertbaren Bänder", {}
    ok = abs(worst_db) <= TOL_DB and worst_deg <= TOL_DEG
    return ok, f"max. {worst_db:+.2f} dB (bei {at:.0f} Hz), max. {worst_deg:.1f}°, {checked} Bänder", {}


# --- report ------------------------------------------------------------------------------------------
def svg_plot(series, title, ylab, ylim=None, w=520, h=220):
    """tiny log-frequency SVG plot without dependencies; series: [(label, xs, ys, css)]"""
    pts_all = [y for _, _, ys, _ in series for y in ys if y is not None and np.isfinite(y)]
    if not pts_all:
        return ""
    y0, y1 = ylim or (min(pts_all) - 1, max(pts_all) + 1)
    X = lambda x: 40 + (math.log10(x) - math.log10(30)) / (math.log10(20000) - math.log10(30)) * (w - 50)
    Y = lambda y: 10 + (y1 - min(max(y, y0), y1)) / (y1 - y0) * (h - 30)
    s = [f'<svg viewBox="0 0 {w} {h}" class="plot" role="img" aria-label="{html.escape(title)}">']
    for fx in (50, 100, 200, 500, 1000, 2000, 5000, 10000):
        s.append(f'<line x1="{X(fx):.1f}" x2="{X(fx):.1f}" y1="10" y2="{h-20}" class="grid"/>'
                 f'<text x="{X(fx):.1f}" y="{h-6}" class="ax">{fx if fx < 1000 else str(fx // 1000) + "k"}</text>')
    for k in range(5):
        yv = y0 + (y1 - y0) * k / 4
        s.append(f'<line x1="40" x2="{w-10}" y1="{Y(yv):.1f}" y2="{Y(yv):.1f}" class="grid"/>'
                 f'<text x="34" y="{Y(yv)+4:.1f}" class="ax" text-anchor="end">{yv:.0f}</text>')
    for label, xs, ys, css in series:
        p = " ".join(f"{X(x):.1f},{Y(y):.1f}" for x, y in zip(xs, ys) if y is not None and np.isfinite(y))
        s.append(f'<polyline points="{p}" class="{css}"><title>{html.escape(label)}</title></polyline>')
    s.append(f'<text x="44" y="22" class="ax">{html.escape(ylab)}</text></svg>')
    return "".join(s)


CSS = """
:root{--bg:#fff;--fg:#1d2026;--muted:#5b6270;--line:#d9dde3;--ok:#137a3c;--bad:#b3261e;--meas:#1f6fd1;--model:#c46a00}
@media (prefers-color-scheme:dark){:root{--bg:#15171b;--fg:#e8eaee;--muted:#9aa3b2;--line:#2c3038;--ok:#5ad38a;--bad:#ff7a70;--meas:#6aa9ff;--model:#ffb05c}}
body{background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif;margin:0 auto;max-width:1100px;padding:16px}
h1{font-size:1.4rem}h2{font-size:1.1rem;margin-top:2rem}table{border-collapse:collapse;width:100%}
td,th{border-bottom:1px solid var(--line);padding:4px 6px;text-align:left;vertical-align:top}.ok{color:var(--ok)}.bad{color:var(--bad)}
.muted{color:var(--muted)}.plots{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(100%,500px),1fr));gap:8px}
.plot{width:100%;height:auto}.grid{stroke:var(--line);stroke-width:1}.ax{fill:var(--muted);font-size:10px}
.meas{fill:none;stroke:var(--meas);stroke-width:2}.model{fill:none;stroke:var(--model);stroke-width:1.5;stroke-dasharray:5 3}
"""


def write_report(d, meta, results, plots):
    ok = sum(1 for r in results if r["ok"])
    rows = "".join(f"<tr><td>{html.escape(r['case'])}</td><td>Ausgang {r['out']}</td>"
                   f"<td class='{'ok' if r['ok'] else 'bad'}'>{'OK' if r['ok'] else 'FEHLER'}</td><td>{html.escape(r['detail'])}</td></tr>"
                   for r in results)
    page = (f"<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            f"<title>Forge Filtertest</title><style>{CSS}</style><h1>Forge Filtertest</h1>"
            f"<p class='muted'>{html.escape(meta['date'])} · {html.escape(meta['forge'].get('system.key.info') or '')} · "
            f"Firmware {html.escape(str(meta['forge'].get('system.firmware.port') or meta['forge'].get('system.firmware')))} · "
            f"Interface {html.escape(meta['device'])}</p>"
            f"<p><strong>{ok} von {len(results)} Prüfungen bestanden.</strong> Toleranz ±{TOL_DB} dB / ±{TOL_DEG}° je "
            f"Sechstel-Oktave mit ≥ 20 dB Rauschabstand, Laufzeit ±{TOL_MS} ms. Blau gemessen, orange gestrichelt Soll "
            f"(Formeln der Firmware).</p>{plots}<h2>Alle Prüfungen</h2><table><tr><th>Test</th><th>Ausgang</th>"
            f"<th>Ergebnis</th><th>Detail</th></tr>{rows}</table>"
            + (f"<h2>S/PDIF-Ausgang</h2><p>{html.escape(meta['spdif']['text'])}</p>" if meta.get("spdif") else "")
            + f"<p class='muted'>Abstimmung vor und nach dem Lauf: {html.escape(meta['tuning'])}</p>")
    open(os.path.join(d, "report.html"), "w", encoding="utf-8").write(page)
    json.dump({"meta": meta, "results": results}, open(os.path.join(d, "results.json"), "w"), indent=1, default=str)


# --- commands ----------------------------------------------------------------------------------------
def parse_list(s):
    return [int(x) for x in s.split(",") if x.strip()]


def cmd_devices(a):
    import sounddevice as sd
    print(sd.query_devices())


def open_all(a):
    f = Forge(a.port)
    info = identify(f)
    print("Forge:", info)
    return f, info


def cmd_check(a):
    global FS
    FS = a.rate
    f, info = open_all(a)
    x = pink_period(a.level)
    with Duplex(a.device, parse_list(a.play), parse_list(a.rec), x) as d:
        d.reset_peak()
        H, n = d.measure(settle=0.5, periods=3)
        peak = d.peak.copy()
    print(f"Forge: {info.get('system.key.info')}, Firmware {info.get('system.firmware.port') or info.get('system.firmware')}")
    fr = np.fft.rfftfreq(N, 1 / FS)
    m = (fr > 100) & (fr < 10000)
    print("Eingang  Spitze dBFS  Pegel rel. dB  Rauschabstand dB")
    for i, ch in enumerate(parse_list(a.rec)):
        lv = 10 * np.log10(np.mean(np.abs(H[m, i]) ** 2) + 1e-30)
        snr = 10 * np.log10(np.sum(np.abs(H[m, i]) ** 2) / max(np.sum(n[m, i]), 1e-30))
        warn = "  ÜBERSTEUERT, Gain/Pad prüfen" if peak[i] > 0.89 else ("  kein Signal" if snr < 15 else "")
        print(f"{ch:6d}   {20*np.log10(peak[i]+1e-9):9.1f}   {lv:12.1f}   {snr:14.1f}{warn}")


def cmd_map(a):
    """which interface input belongs to which Forge output: mute all but one, in turn"""
    global FS
    FS = a.rate
    f, info = open_all(a)
    outs = list(range(1, 9))
    x = pink_period(a.level)
    found = {}
    try:
        with Duplex(a.device, parse_list(a.play), parse_list(a.rec), x) as d:
            for o in outs:
                for p in outs:
                    f.set(f"mute.out.{p}", 0 if p == o else 1)
                H, n = d.measure(settle=1.5, periods=2)
                fr = np.fft.rfftfreq(N, 1 / FS)
                m = (fr > 100) & (fr < 10000)
                lv = [10 * np.log10(np.mean(np.abs(H[m, i]) ** 2) + 1e-30) for i in range(H.shape[1])]
                best = int(np.argmax(lv))
                second = sorted(lv)[-2]
                found[o] = parse_list(a.rec)[best] if lv[best] - second > 15 else None
                print(f"Forge-Ausgang {o} -> Eingang {found[o]}  ({lv[best] - second:.0f} dB über dem nächsten)")
    finally:
        f.undo_all()
    if all(found.values()):
        print("--map " + ",".join(str(found[o]) for o in outs))


def spdif_probe(f, dup, outs, scol):
    """where the S/PDIF output of the Forge takes its signal from: gain -6 dB on one output after
    the other (mute works at the DAC, not in the DSP, so it would not show)"""
    fr = np.fft.rfftfreq(N, 1 / FS)
    m = (fr > 200) & (fr < 8000)
    H0, n0 = dup.measure()
    lv = lambda H, c: 10 * np.log10(np.mean(np.abs(H[m, c]) ** 2) + 1e-30)
    snr = [10 * np.log10(np.sum(np.abs(H0[m, c]) ** 2) / max(np.sum(n0[m, c]), 1e-30)) for c in scol]
    res = {"signal": [round(x, 1) for x in snr], "follows": {}}
    if max(snr) < 20:
        res["text"] = "kein Signal am S/PDIF-Eingang des Interface (Takt/Lock prüfen)"
        return res
    for o in outs:
        g = int(f.get(f"gain.out.{o}") or 223)
        f.set(f"gain.out.{o}", max(0, g - 12))
        H, _ = dup.measure()
        f.undo(f"gain.out.{o}")
        d = [lv(H, c) - lv(H0, c) for c in scol]
        res["follows"][o] = [round(x, 1) for x in d]
        print(f"   gain.out.{o} −6 dB: S/PDIF L/R ändert sich um {d[0]:+.1f}/{d[-1]:+.1f} dB")
    hit = {ch: [o for o, d in res["follows"].items() if d[i] < -4] for i, ch in enumerate(("L", "R")[:len(scol)])}
    if any(hit.values()):
        res["text"] = "S/PDIF out liegt HINTER der Ausgangskette: " + ", ".join(f"{ch} folgt Ausgang {v}" for ch, v in hit.items() if v)
    else:
        res["text"] = "S/PDIF out ändert sich mit keinem Ausgangs-Gain: Signal liegt VOR der Ausgangskette (Eingang/Durchschleifen)"
    return res


def cmd_run(a):
    global FS
    FS = a.rate
    f, info = open_all(a)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    d = os.path.join(a.out, stamp)
    os.makedirs(d, exist_ok=True)
    print("Sichere die Abstimmung ...")
    model, before = tuning.export(f.app)
    open(os.path.join(d, "tuning-before.cfg"), "w", encoding="latin-1").write(tuning.to_cfg(model, before, ["vor dem Filtertest"]))
    rec = parse_list(a.rec)
    spdif = parse_list(a.spdif) if a.spdif else []
    mp = parse_list(a.map) if a.map else rec
    outs = [o for o in range(1, 9) if o <= len(mp)]
    col = {o: rec.index(mp[o - 1]) for o in outs}
    slots = free_slots(f, outs)
    missing = [o for o in outs if o not in slots]
    if missing:
        print(f"Kein freier Filterplatz an Ausgang {missing}: dort keine Filtertests")
    port_fw = bool(info.get("system.firmware.port"))
    x = pink_period(a.level)
    results, plots = [], []
    spdif_res = None
    fr = np.fft.rfftfreq(N, 1 / FS)
    try:
        with Duplex(a.device, parse_list(a.play), rec + spdif, x) as dup:
            print("Grundmessung ...")
            Hb, nb = dup.measure()
            for case in cases(port_fw):
                if a.only and case[0] not in a.only.split(","):
                    continue
                cid, title, kind, value, model_fn = case
                print(f"{title} ...")
                targets = [o for o in outs if kind != "bq" or o in slots]
                failed = {}
                for o in list(targets):
                  try:
                    if kind == "bq":
                        f.set(f"biquad.{o}.{slots[o]}.calc", "[" + ",".join(map(str, value)) + "]")
                    elif kind == "gain":
                        g = int(f.get(f"gain.out.{o}") or 223)
                        f.set(f"gain.out.{o}", max(0, g + round(value * 2)))
                    elif kind == "phase":
                        f.set(f"phase.out.{o}", 1 - int(f.get(f"phase.out.{o}") or 0))
                    elif kind == "delay":
                        f.set(f"delay.{o}", int(f.get(f"delay.{o}") or 0) + value)
                    elif kind == "mute":
                        f.set(f"mute.out.{o}", 1)
                  except RuntimeError as e:
                    failed[o] = str(e); targets.remove(o)
                if kind == "bq":
                    f.wait_fades()
                Hn, nn = dup.measure()
                for o, e in failed.items():
                    results.append({"case": title, "id": cid, "out": o, "ok": False, "detail": e})
                    print(f"   Ausgang {o}: FEHLER {e}")
                for o in targets:   # set back right away
                    reg = {"bq": f"biquad.{o}.{slots.get(o)}.calc", "gain": f"gain.out.{o}", "phase": f"phase.out.{o}",
                           "delay": f"delay.{o}", "mute": f"mute.out.{o}"}[kind]
                    try:
                        f.undo(reg)        # the value read before (an empty place: type 0)
                    except RuntimeError:
                        if kind == "bq":   # the firmware refuses the factory zeros on some versions
                            f.set(reg, "[0,1000,0,707,0,128]", remember=False)
                        else:
                            raise
                for o in targets:
                    c = col[o]
                    vals = band_values(fr, Hb[:, c], Hn[:, c], nb[:, c], model_fn)
                    ok, detail, _ = judge(case, vals, fr, Hb[:, c], Hn[:, c])
                    results.append({"case": title, "id": cid, "out": o, "ok": bool(ok), "detail": detail})
                    print(f"   Ausgang {o}: {'OK    ' if ok else 'FEHLER'} {detail}")
                    if model_fn and kind not in ("delay",):
                        xs = [v[0] for v in vals]
                        meas = [20 * math.log10(abs(v[1]) + 1e-12) if v[2] >= 20 else None for v in vals]
                        mod = [20 * math.log10(abs(v[3]) + 1e-12) for v in vals]
                        series = [("gemessen", xs, meas, "meas"), ("Soll", xs, mod, "model")]
                        plot = svg_plot(series, f"{title}, Ausgang {o}", "dB", (-30, 12))
                        if kind == "bq" and value[0] == 20:
                            ph = [math.degrees(cmath.phase(v[1])) if v[2] >= 20 else None for v in vals]
                            pm = [math.degrees(cmath.phase(v[3])) for v in vals]
                            plot = svg_plot([("gemessen", xs, ph, "meas"), ("Soll", xs, pm, "model")], f"{title}, Ausgang {o}", "Grad", (-180, 180))
                        plots.append((title, o, plot))
                if kind == "bq":
                    f.wait_fades()   # the set back fades too; the next case writes the same places
                time.sleep(SETTLE)
            if spdif:
                print("S/PDIF-Ausgang: woher kommt sein Signal ...")
                spdif_res = spdif_probe(f, dup, outs, [len(rec) + i for i in range(len(spdif))])
                print("  ", spdif_res["text"])
    finally:
        f.undo_all()
        print("Prüfe die Abstimmung nach dem Lauf ...")
        _, after = tuning.export(f.app)
        diff = tuning.compare(before, after)
        state = "unverändert" if not diff else f"{len(diff)} Werte weichen ab: {diff[:5]} (zurückschreiben: restore {d}/tuning-before.cfg)"
        print("Abstimmung:", state)
    by_case = {}
    for title, o, p in plots:
        by_case.setdefault(title, []).append(f"<figure><figcaption>Ausgang {o}</figcaption>{p}</figure>")
    plot_html = "".join(f"<h2>{html.escape(t)}</h2><div class='plots'>{''.join(v)}</div>" for t, v in by_case.items())
    meta = {"date": stamp, "forge": info, "device": a.device, "tuning": state, "slots": slots, "map": mp,
            "play": a.play, "level_dbfs": a.level, "rate": FS, "spdif": spdif_res}
    write_report(d, meta, results, plot_html)
    ok = sum(r["ok"] for r in results)
    print(f"\n{ok}/{len(results)} bestanden. Bericht: {os.path.join(d, 'report.html')}")


def cmd_selftest(a):
    """the evaluation against synthetic measurements (no hardware): every case must pass, a wrong
    filter must fail"""
    rng = np.random.default_rng(1)
    f = np.fft.rfftfreq(N, 1 / FS)
    x = pink_period(); X = np.fft.rfft(x); use = np.abs(X) > np.abs(X).max() * 1e-6
    base = bq_resp(bq_coeffs(9, 2000, 0, 707), f) ** 2 * np.exp(-2j * np.pi * f * 0.0123)   # tweeter-like output

    def meas(H):
        Hs = []
        for _ in range(PERIODS):
            Y = X * H + (rng.normal(size=len(f)) + 1j * rng.normal(size=len(f))) * np.abs(X).mean() * 0.003
            Hs.append(np.where(use, Y / np.where(use, X, 1), 0))
        Hs = np.array(Hs); Hm = Hs.mean(0)
        return Hm, (np.abs(Hs - Hm) ** 2).mean(0) / PERIODS
    Hb, nb = meas(base)
    bad = 0
    for c in cases(True):
        cid, title, kind, value, model = c
        H = base * 0 if kind == "mute" else base * np.exp(-2j * np.pi * f * value / FS_DSP) if kind == "delay" else base * model(f)
        Hn, _ = meas(H)
        ok, detail, _ = judge(c, band_values(f, Hb, Hn, nb, model), f, Hb, Hn)
        bad += not ok
        print(f"{'OK    ' if ok else 'FEHLER'} {title}: {detail}")
    c = cases(True)[0]
    Hn, _ = meas(base * bq_resp(bq_coeffs(1, 1100, 60, 1414), f))
    ok, detail, _ = judge(c, band_values(f, Hb, Hn, nb, c[4]), f, Hb, Hn)
    print(f"{'FEHLER' if ok else 'OK    '} falscher Filter (1,1 statt 1 kHz) wird erkannt: {detail}")
    bad += ok
    print("Selbsttest bestanden" if not bad else f"Selbsttest: {bad} Fehler")
    sys.exit(1 if bad else 0)


def cmd_restore(a):
    f = Forge(a.port)
    identify(f)
    model, entries = tuning.parse_cfg(open(a.file, encoding="latin-1").read())
    w, e = tuning.restore(f.app, model, entries)
    print(f"{w} Werte geschrieben, {e} Fehler")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("devices", help="audio devices")
    sub.add_parser("selftest", help="check the evaluation without hardware")
    for name in ("check", "map", "run"):
        p = sub.add_parser(name)
        p.add_argument("--port", required=True, help="serial port of the Forge (COM5, /dev/ttyUSB0, /dev/cu.SLAB_USBtoUART)")
        p.add_argument("--device", required=True, help="audio device (name part or number from 'devices')")
        p.add_argument("--play", default="3,4,5,6,7,8", help="output channels of the interface with the test signal (1-based)")
        p.add_argument("--rec", default="1,2,3,4,5,6,7,8", help="input channels of the interface (1-based)")
        p.add_argument("--level", type=float, default=-18.0, help="RMS level of the pink noise in dBFS")
        p.add_argument("--rate", type=int, default=48000, help="sample rate of the interface (96000 with --spdif, see README)")
        if name == "run":
            p.add_argument("--map", help="interface input of Forge output 1..8 (from 'map'); default = --rec in order")
            p.add_argument("--only", help="only these tests, e.g. eq1k,delay")
            p.add_argument("--out", default=os.path.join(HERE, "results"))
            p.add_argument("--spdif", help="interface inputs with the S/PDIF output of the Forge, e.g. 9,10 (see README)")
    p = sub.add_parser("restore")
    p.add_argument("--port", required=True)
    p.add_argument("file")
    a = ap.parse_args()
    {"devices": cmd_devices, "selftest": cmd_selftest, "check": cmd_check, "map": cmd_map, "run": cmd_run, "restore": cmd_restore}[a.cmd](a)


if __name__ == "__main__":
    main()
