#!/usr/bin/env python3
"""Cut, clean and level the dawn-chorus clips listed in assets/birds/clips.json.

    make_bird_clips.py <dir with the decoded source WAVs (22.05 kHz mono)>

For each clip: band-pass to the species' range (drops wind rumble, insects and
other birds), a gentle FFT denoise, 30 ms fades, then level to -20 dBFS RMS
over the loud part with peaks capped at -1.5 dBFS. Writes
assets/birds/<species>_<n>.wav (22.05 kHz mono s16). Needs ffmpeg and numpy.
The sources and licences are in clips.json and NOTICES.md.
"""
import json, os, subprocess, sys, wave
import numpy as np

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cfg = json.load(open(os.path.join(root, "assets/birds/clips.json")))
srcdir = sys.argv[1]
out_dir = os.path.join(root, "assets/birds")

def read(p):
    w = wave.open(p); return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768

for sp, spec in cfg["species"].items():
    for n, (key, a, b) in enumerate(spec["clips"]):
        src = os.path.join(srcdir, cfg["sources"][key]["wav"])
        tmp = os.path.join(out_dir, ".tmp.wav")
        af = (f"atrim={a}:{b},asetpts=N/SR/TB,highpass=f={spec['hp']}:poles=2,highpass=f={spec['hp']}:poles=2,"
              f"lowpass=f={spec['lp']}:poles=2,lowpass=f={spec['lp']}:poles=2,afftdn=nr={spec.get('nr',10)}:nf=-45,"
              f"afade=t=in:d=0.03,areverse,afade=t=in:d=0.03,areverse")
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", src, "-af", af, "-ac", "1", "-ar", "22050", "-sample_fmt", "s16", tmp], check=True)
        x = read(tmp); os.remove(tmp)
        # level: RMS of the loudest 40% of 20 ms frames -> -20 dBFS, peak <= -1.5 dBFS
        f = 441; fr = x[: len(x) // f * f].reshape(-1, f); e = np.sqrt((fr ** 2).mean(1))
        loud = np.sort(e)[int(len(e) * 0.6):]
        g = 10 ** (-20 / 20) / (np.sqrt((loud ** 2).mean()) + 1e-9)
        g = min(g, 10 ** (-1.5 / 20) / (np.abs(x).max() + 1e-9))
        y = np.clip(x * g, -1, 1)
        # band-limit again after the gain: a big gain lifts the source's codec
        # noise across the whole band (raised-cosine edges, 15% wide)
        Y = np.fft.rfft(y); fq = np.fft.rfftfreq(len(y), 1 / 22050)
        lo, hi = spec["hp"], spec["lp"]
        w = np.clip((fq - lo * 0.85) / (lo * 0.15), 0, 1) * np.clip((hi * 1.15 - fq) / (hi * 0.15), 0, 1)
        y = np.fft.irfft(Y * (0.5 - 0.5 * np.cos(np.pi * w)), len(y)).astype(np.float32)
        if spec.get("gate_db"):
            # noise gate: keep only what rises gate_db over the floor (faint, distant
            # calls in hiss), with 5 ms attack and 60 ms release
            env = np.convolve(np.abs(y), np.ones(110) / 110, mode="same")
            floor = np.percentile(env, 30) + 1e-9
            over = 20 * np.log10(env / floor + 1e-9) - spec["gate_db"]
            gate = np.clip(over / 6.0, 0, 1)
            sm = np.empty_like(gate); a_att, a_rel = 1 - np.exp(-1 / 110), 1 - np.exp(-1 / 1323); v = 0.0
            for i, t in enumerate(gate):
                v += (a_att if t > v else a_rel) * (t - v); sm[i] = v
            y = y * sm
            y = np.clip(y * min(10 ** (-1.5 / 20) / (np.abs(y).max() + 1e-9), 4.0), -1, 1)
        name = os.path.join(out_dir, f"{sp}_{n}.wav")
        w = wave.open(name, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
        w.writeframes((y * 32767).astype(np.int16).tobytes()); w.close()
        snr = 20 * np.log10(np.percentile(e, 95) / (np.percentile(e, 10) + 1e-9))
        print(f"{os.path.basename(name):<16} {len(y)/22050:5.2f}s  gain {20*np.log10(g):+5.1f} dB  dynamic range {snr:4.1f} dB")
