"""tools/filtertest: a whole 'run' against a simulated Forge (text protocol at the serial level) and a
simulated 8-channel interface: backup, free places, every setting set and set back, report."""
import os
import re
import sys
import tempfile
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "filtertest"))

try:
    import numpy as np
    import forge_filtertest as F
except ImportError:   # numpy missing
    F = None


class FakeForge:
    """registers of a 96ELN with 10 biquads per output; answers as the firmware does"""

    def __init__(self):
        self.r = {"system.key.info": '"BNS-96ELN-4.0-SIM-CCBYNCSA-20261117-MDD"', "system.key.info.type": '"96ELN"',
                  "system.firmware": '"1.11.6000"', "system.firmware.port": '"2.6.2"', "system.ping": '"pong"'}
        for o in range(1, 9):
            self.r[f"gain.out.{o}"] = "223"; self.r[f"mute.out.{o}"] = "0"; self.r[f"phase.out.{o}"] = "0"
            self.r[f"delay.{o}"] = str(10 * o)
            for n in range(1, 11):
                self.r[f"biquad.{o}.{n}"] = "[0,0,0,0,0,0]"
            self.r[f"system.channel.name.{o}"] = f'"Out {o}"'
        self.r["biquad.1.1"] = "[1,500,-30,2000,0,128]"     # a filter of the tuning
        self.r["biquad.2.1"] = "[1,800,20,1000,0,0]"        # switched off: must not be used
        self.r["mixer.position"] = "1"
        for i in range(1, 7):
            self.r[f"gain.in.{i}"] = "223"
        self.r["gain.spdif.1"] = "255"; self.r["gain.spdif.2"] = "255"
        for c in range(1, 11):
            for i in range(1, 9):
                self.r[f"mixer.1.{c}.{i}"] = "0"
        for n in range(1, 33):
            self.r[f"modc.{n}"] = "[0,0,00000000,0,0,0,0,00000000]"
        self.out = b""
        self.writes = []

    # serial interface used by tuning.App
    baudrate = 38400

    def reset_input_buffer(self):
        self.out = b""

    def read(self, n):
        b, self.out = self.out[:n], self.out[n:]
        return b

    def write(self, data):
        line = data.decode("latin-1").strip()
        m = re.match(r"get\.1 (\S+)$", line)
        ans = []
        if m:
            reg = m.group(1)
            if reg.endswith(".*"):
                pre = reg[:-1]
                ans = [f" <.1 {k}={v}" for k, v in self.r.items() if k.startswith(pre)]
                if not ans:
                    ans = [f" !.1 {reg} unknown command"]
            elif reg in self.r:
                ans = [f" <.1 {reg}={self.r[reg]}"]
            else:
                ans = [f" !.1 {reg} unknown command"]
        m = re.match(r"set\.1 (\S+?)=(.*);([0-9A-F]{2})$", line)
        if m:
            reg, val = m.group(1), m.group(2)
            key = reg[:-5] if reg.endswith(".calc") else reg
            if key in self.r:
                self.r[key] = val
                self.writes.append((key, val))
                ans = [f" *.1 {reg} ok"]
            else:
                ans = [f" !.1 {reg} unknown command"]
        self.out += ("\r\n".join(ans) + "\r\n").encode("latin-1")


class FakeDuplex:
    """the 8 outputs as the Forge computes them from its registers: a crossover each, the biquads,
    gain, polarity, delay, mute; noise like a real interface"""
    forge = None

    def __init__(self, device, play, rec, x):
        self.x = x
        self.rng = np.random.default_rng(3)
        self.f = np.fft.rfftfreq(F.N, 1 / F.FS)
        self.X = np.fft.rfft(x)
        cross = [F.bq_resp(F.bq_coeffs(9, 2500, 0, 707), self.f) ** 2, F.bq_resp(F.bq_coeffs(3, 90, 0, 707), self.f)]
        self.base = [cross[o % 2] if o in (1, 2) else np.ones(len(self.f), complex) for o in range(8)]
        self.peak = np.zeros(8)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass

    def reset_peak(self):
        pass

    def channel(self, o):
        r = self.forge.r
        H = self.base[o - 1].copy()
        for n in range(1, 11):
            v = [int(x) for x in r[f"biquad.{o}.{n}"].strip("[]").split(",")]
            if v[0] and v[5] & 0x80:
                H *= F.bq_resp(F.bq_coeffs(v[0], v[1], v[2], v[3]), self.f)
        H *= 10 ** ((int(r[f"gain.out.{o}"]) - 223) / 2 / 20)
        H *= -1 if r[f"phase.out.{o}"] == "1" else 1
        H *= np.exp(-2j * np.pi * self.f * int(r[f"delay.{o}"]) / F.FS_DSP)
        return H * (0 if r[f"mute.out.{o}"] == "1" else 1)

    def measure(self, settle=0, periods=F.PERIODS if F else 5):
        use = np.abs(self.X) > np.abs(self.X).max() * 1e-6
        Hs = []
        for _ in range(periods):
            cols = []
            for o in range(1, 9):
                Y = self.X * self.channel(o) + (self.rng.normal(size=len(self.f)) + 1j * self.rng.normal(size=len(self.f))) * np.abs(self.X).mean() * 0.002
                cols.append(np.where(use, Y / np.where(use, self.X, 1), 0))
            Hs.append(np.stack(cols, 1))
        Hs = np.array(Hs)
        Hm = Hs.mean(0)
        return Hm, (np.abs(Hs - Hm[None]) ** 2).mean(0) / periods


@unittest.skipIf(F is None, "numpy missing")
class FilterTestRun(unittest.TestCase):
    def test_selftest_cases_and_run(self):
        fake = FakeForge()
        before = dict(fake.r)
        sys.modules["serial"] = types.SimpleNamespace(Serial=lambda *a, **k: fake)
        FakeDuplex.forge = fake
        F.Duplex = FakeDuplex
        F.SETTLE = 0
        F.time.sleep = lambda s: None
        with tempfile.TemporaryDirectory() as d:
            a = types.SimpleNamespace(port="SIM", device="SIM", play="3,4,5,6,7,8", rec="1,2,3,4,5,6,7,8",
                                      level=-18.0, map=None, only=None, out=d, rate=48000, spdif=None)
            F.cmd_run(a)
            runs = os.listdir(d)
            self.assertEqual(len(runs), 1)
            res = __import__("json").load(open(os.path.join(d, runs[0], "results.json")))
            html_ = open(os.path.join(d, runs[0], "report.html"), encoding="utf-8").read()
            cfg = open(os.path.join(d, runs[0], "tuning-before.cfg"), encoding="latin-1").read()
        bad = [r for r in res["results"] if not r["ok"]]
        self.assertEqual(bad, [], bad[:5])
        self.assertEqual(len(res["results"]), 13 * 8)
        self.assertEqual(res["meta"]["tuning"], "unverändert")
        self.assertIn("<svg", html_)
        self.assertIn("biquad.1.1.calc=[1,500,-30,2000,0,128]", cfg)
        # the free places: the first empty one, never a switched-off filter or the last two
        self.assertEqual(res["meta"]["slots"]["1"] if "1" in res["meta"]["slots"] else res["meta"]["slots"][1], 2)
        slots = {int(k): v for k, v in res["meta"]["slots"].items()}
        self.assertEqual(slots[2], 2)
        self.assertTrue(all(v <= 8 for v in slots.values()))
        # every register as before
        self.assertEqual(fake.r, before)


if __name__ == "__main__":
    unittest.main()
