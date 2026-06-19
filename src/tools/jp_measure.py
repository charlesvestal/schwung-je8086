#!/usr/bin/env python3
"""Build JP-8000 measurement patches as Roland sysex and drive jp8000_render to
measure the real DSP transfer curves (cutoff Hz, envelope times, detune cents).

Roland DataSet1:  F0 41 10 00 06 12 <A1 A2 A3 A4> <data...> <chk> F7
Temp-performance PatchUpper param at logical offset `off`:
  address = [0x01, 0x00, hi, lo]  where (hi<<8|lo) = 0x4000 + off
  checksum = (0x80 - (sum(addr+data) & 0x7F)) & 0x7F
"""
import struct, subprocess, sys, os, wave, array, math

ROMDIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../dist/jp8000/roms"))
RENDER = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build-native/jp8000_render"))

# Patch byte offsets (jeLib Patch enum)
P = dict(Osc1Wave=0x1E, Osc1C1=0x1F, Osc1C2=0x20, OscBal=0x17,
         Osc2Wave=0x21, FiltType=0x27, CutSlope=0x28, Cut=0x29, Res=0x2A,
         FEnvDep=0x2E, FA=0x2F, FD=0x30, FS=0x31, FR=0x32,
         AmpLvl=0x33, AA=0x36, AD=0x37, AS=0x38, AR=0x39,
         FxType=0x3D, FxLvl=0x3E, DelayLvl=0x42, Mono=0x47)

def dataset1(off, value):
    full = 0x4000 + off
    addr = [0x01, 0x00, (full >> 8) & 0x7F, full & 0x7F]
    data = [value & 0x7F]
    chk = (0x80 - (sum(addr + data) & 0x7F)) & 0x7F
    return bytes([0xF0, 0x41, 0x10, 0x00, 0x06, 0x12] + addr + data + [chk, 0xF7])

def base_patch(**overrides):
    """A clean supersaw measurement patch: held amp, no FX, filter isolated."""
    p = {'Osc1Wave': 0, 'Osc1C1': 64, 'Osc1C2': 100, 'OscBal': 0,
         'FiltType': 2, 'CutSlope': 1, 'Cut': 100, 'Res': 0, 'FEnvDep': 64,
         'FA': 0, 'FD': 0, 'FS': 127, 'FR': 0,
         'AmpLvl': 127, 'AA': 0, 'AD': 0, 'AS': 127, 'AR': 0,
         'FxLvl': 0, 'DelayLvl': 0, 'Mono': 0}
    p.update(overrides)
    return b"".join(dataset1(P[k], v) for k, v in p.items())

def render(patch_bytes, out_wav, note=60, vel=100, seconds=2.0, hold=1.4):
    pf = out_wav + ".syx"
    open(pf, "wb").write(patch_bytes)
    script = out_wav + ".txt"
    with open(script, "w") as f:
        f.write(f"render_seconds {seconds}\npatch_file {pf}\n0 on {note} {vel}\n{int(hold*1000)} off {note}\n")
    subprocess.run([RENDER, ROMDIR, script, out_wav], capture_output=True)
    return out_wav

def read_wav(path):
    w = wave.open(path, "rb"); sr = w.getframerate(); n = w.getnframes()
    a = array.array("h"); a.frombytes(w.readframes(n))
    L = [a[i] / 32768.0 for i in range(0, len(a), 2)]
    return sr, L

def _spectrum(L, sr):
    """Hann-windowed magnitude spectrum of the sustain region, via numpy."""
    import numpy as np
    seg = np.array(L[int(0.55 * sr):int(1.15 * sr)], dtype=float)
    if len(seg) < 1024:
        return None, None
    seg = seg * np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(seg))
    freq = np.fft.rfftfreq(len(seg), 1.0 / sr)
    return freq, mag

def spectral_centroid(L, sr, fmax=20000.0):
    """Energy-weighted mean frequency (brightness proxy), capped at fmax."""
    import numpy as np
    freq, mag = _spectrum(L, sr)
    if freq is None: return 0.0
    m = freq <= fmax
    e = mag[m] ** 2
    return float((freq[m] * e).sum() / max(1e-12, e.sum()))

def spectral_rolloff(L, sr, frac=0.85, fmax=30000.0):
    """Frequency below which `frac` of the energy lies (≤ fmax)."""
    import numpy as np
    freq, mag = _spectrum(L, sr)
    if freq is None: return 0.0
    m = freq <= fmax
    e = (mag[m] ** 2); f = freq[m]
    c = np.cumsum(e); tot = c[-1] if len(c) else 0.0
    if tot <= 0: return 0.0
    idx = int(np.searchsorted(c, frac * tot))
    return float(f[min(idx, len(f) - 1)])

def cutoff_hz(L, sr, ref_mag, freq, drop_db=3.0):
    """Cutoff = highest freq where this spectrum is still within drop_db of the
    open-filter reference, scanning down from the top — the filter's -3 dB corner."""
    import numpy as np
    _, mag = _spectrum(L, sr)
    if mag is None: return 0.0
    n = min(len(mag), len(ref_mag))
    m = np.maximum(mag[:n], 1e-9); r = np.maximum(ref_mag[:n], 1e-9)
    ratio_db = 20 * np.log10(m / r)
    band = (freq[:n] >= 80) & (freq[:n] <= 18000)
    idxs = np.where(band & (ratio_db >= -drop_db))[0]
    return float(freq[idxs[-1]]) if len(idxs) else 80.0

def amp_envelope(L, sr, win_ms=15.0, hop_ms=3.0):
    """Smoothed RMS amplitude envelope (single-saw probe → clean, no beating)."""
    import numpy as np
    a = np.array(L); w = int(sr * win_ms / 1000); h = int(sr * hop_ms / 1000)
    env = np.array([np.sqrt((a[i:i+w] ** 2).mean()) for i in range(0, len(a) - w, h)])
    t = np.arange(len(env)) * h / sr
    return t, env

def attack_time(t, env, hold):
    """10%->90% rise toward the sustain level (mean over the second half of the hold)."""
    import numpy as np
    i_sus0 = int(np.searchsorted(t, hold * 0.5)); i_sus1 = int(np.searchsorted(t, hold * 0.95))
    sus = max(env[i_sus0:i_sus1].mean(), 1e-9)
    e = env / sus; a = b = None
    end = int(np.searchsorted(t, hold))
    for i in range(end):
        if a is None and e[i] >= 0.1: a = t[i]
        if e[i] >= 0.9: b = t[i]; break
    return round((b - a), 4) if (a is not None and b is not None) else 0.0

def detune_cents(L, sr, f0):
    """Supersaw partial spread around the fundamental f0, as ± cents."""
    import numpy as np
    freq, mag = _spectrum(L, sr)
    if freq is None: return 0.0
    band = (freq >= f0 * 0.5) & (freq <= f0 * 1.5)
    f = freq[band]; m = mag[band] ** 2
    if m.sum() <= 0: return 0.0
    mean = (f * m).sum() / m.sum()
    var = (((f - mean) ** 2) * m).sum() / m.sum()
    std = var ** 0.5
    return float(1200 * np.log2((mean + std) / max(1e-9, mean)))

def sweep(label, mk_patch, analyze, values):
    out = []
    for v in values:
        w = render(mk_patch(v), f"/tmp/sw_{label}_{v}.wav")
        sr, L = read_wav(w)
        out.append((v, analyze(sr, L)))
    return out

if __name__ == "__main__":
    import json, numpy as np
    print(f"rom={ROMDIR}\nrender={RENDER}")
    calib = {}
    ENV = dict(Osc1Wave=5, Osc1C2=0, OscBal=0, Cut=127, Res=0, FEnvDep=64)  # single saw, filter open

    # ---- CUTOFF -> centroid Hz: single SAW at C2 (note 36), brightness vs JP value ----
    probe = dict(Osc1Wave=5, Osc1C2=0, OscBal=0, CutSlope=1, Res=0)
    cut = []
    for v in list(range(0, 64, 4)) + list(range(64, 128, 12)):
        w = render(base_patch(Cut=v, **probe), f"/tmp/sw_cut_{v}.wav", note=36)
        s2, L = read_wav(w); cut.append((v, round(spectral_centroid(L, s2))))
    calib["cutoff_centroid"] = cut
    print("CUTOFF->centroid:", cut)

    # ---- AMP ATTACK -> seconds ----
    att = []
    for v in range(0, 128, 10):
        w = render(base_patch(AA=v, AD=0, AS=127, AR=0, **ENV), f"/tmp/sw_att_{v}.wav", seconds=5.0, hold=4.0)
        s2, L = read_wav(w); t, env = amp_envelope(L, s2)
        att.append((v, attack_time(t, env, 4.0)))
    calib["amp_attack_s"] = att; print("AMP ATTACK->s:", att)

    # ---- AMP RELEASE -> seconds (note-off at OFFT, time to -20dB) ----
    OFFT = 0.5
    def rel_time(sr, L):
        t, env = amp_envelope(L, sr)
        i0 = int(np.searchsorted(t, OFFT))
        base = max(env[max(0, i0 - 8):i0].mean(), 1e-9)
        for i in range(i0, len(env)):
            if env[i] <= 0.1 * base: return round(t[i] - OFFT, 4)
        return round(t[-1] - OFFT, 4)
    rel = []
    for v in range(0, 128, 10):
        w = render(base_patch(AA=0, AD=0, AS=127, AR=v, **ENV), f"/tmp/sw_rel_{v}.wav", seconds=6.0, hold=OFFT)
        s2, L = read_wav(w); rel.append((v, rel_time(s2, L)))
    calib["amp_release_s"] = rel; print("AMP RELEASE->s:", rel)

    # ---- AMP DECAY -> seconds (peak -> within 5% of sustain), sustain=40 ----
    def dec_time(sr, L, hold):
        t, env = amp_envelope(L, sr)
        end = int(np.searchsorted(t, hold))
        seg = env[:end]
        if len(seg) < 5: return 0.0
        pk = seg.max(); ipk = int(seg.argmax())
        sus = max(seg[int(end*0.8):end].mean(), 1e-9)
        for i in range(ipk, end):
            if seg[i] <= sus + 0.05 * (pk - sus): return round(t[i] - t[ipk], 4)
        return round(t[end-1] - t[ipk], 4)
    dec = []
    for v in range(0, 128, 10):
        w = render(base_patch(AA=0, AD=v, AS=40, AR=0, **ENV), f"/tmp/sw_dec_{v}.wav", seconds=5.0, hold=4.0)
        s2, L = read_wav(w); dec.append((v, dec_time(s2, L, 4.0)))
    calib["amp_decay_s"] = dec; print("AMP DECAY->s:", dec)

    # ---- SUSTAIN level -> 0..1 (steady level vs AS=127 reference) ----
    refw = render(base_patch(AA=0, AD=8, AS=127, AR=0, **ENV), "/tmp/sw_susref.wav", seconds=3.0, hold=2.5)
    sr, RL = read_wav(refw); _, re = amp_envelope(RL, sr); reflvl = max(re[int(len(re)*0.5):int(len(re)*0.8)].mean(), 1e-9)
    sus = []
    for v in range(0, 128, 16):
        w = render(base_patch(AA=0, AD=8, AS=v, AR=0, **ENV), f"/tmp/sw_sus_{v}.wav", seconds=3.0, hold=2.5)
        s2, L = read_wav(w); _, e = amp_envelope(L, s2)
        lvl = e[int(len(e)*0.5):int(len(e)*0.8)].mean() / reflvl
        sus.append((v, round(float(lvl), 3)))
    calib["amp_sustain"] = sus; print("SUSTAIN->level:", sus)

    json.dump(calib, open("/tmp/jp_calib.json", "w"), indent=1)
    print("wrote /tmp/jp_calib.json")
