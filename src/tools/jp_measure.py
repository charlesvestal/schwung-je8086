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

if __name__ == "__main__":
    print(f"rom={ROMDIR}\nrender={RENDER}")
    print("=== cutoff sweep (FiltType=LPF, slope=-24, res=0, no env) ===")
    for cut in range(0, 128, 8):
        w = render(base_patch(Cut=cut), f"/tmp/meas_cut{cut}.wav")
        sr, L = read_wav(w)
        cen = spectral_centroid(L, sr); ro = spectral_rolloff(L, sr)
        print(f"  cutoff={cut:3d}  centroid={cen:7.0f} Hz  rolloff85={ro:7.0f} Hz")
