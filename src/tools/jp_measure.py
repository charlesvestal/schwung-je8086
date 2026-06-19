#!/usr/bin/env python3
"""Build JP-8000 measurement patches as Roland sysex and drive jp8000_render to
measure the real DSP transfer curves the JE-8086 (reverse-engineered firmware)
produces.  We trust the EMULATOR as ground truth — it runs the actual firmware
at 88.1 kHz, which is more accurate than Adam Szabo's sample-fit Super Saw model.

Focus (per calibration plan): FILTER RESPONSE (cutoff value -> Hz) and ENVELOPE
BEHAVIOR (amp A/D/S/R -> seconds/level).  Detune/mix are NOT measured here: NuSaw
already models the extracted Super Saw algorithm, so they map analytically.

Roland DataSet1:  F0 41 10 00 06 12 <A1 A2 A3 A4> <data...> <chk> F7
Temp-performance PatchUpper param at logical offset `off`:
  address = [0x01, 0x00, hi, lo]  where (hi<<8|lo) = 0x4000 + off
  checksum = (0x80 - (sum(addr+data) & 0x7F)) & 0x7F

CRITICAL: jp8000_render writes **24-bit** stereo WAV @ 88200 Hz.  read_wav()
decodes that correctly (an earlier 16-bit reader mangled every sample, which
made release/sustain/cutoff look "noisy" — it was a parse bug, not the DSP).
"""
import subprocess, sys, os, wave
import numpy as np

ROMDIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../dist/jp8000/roms"))
RENDER = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../build-native/jp8000_render"))

# Patch byte offsets (jeLib Patch enum, see jemiditypes.h)
P = dict(Osc1Wave=0x1E, Osc1C1=0x1F, Osc1C2=0x20, OscBal=0x17,
         Osc2Wave=0x21, FiltType=0x27, CutSlope=0x28, Cut=0x29, Res=0x2A,
         CutKeyFollow=0x2B, FEnvDep=0x2E, FA=0x2F, FD=0x30, FS=0x31, FR=0x32,
         AmpLvl=0x33, AA=0x36, AD=0x37, AS=0x38, AR=0x39,
         FxType=0x3D, FxLvl=0x3E, DelayLvl=0x42, Mono=0x47)

# Osc1Waveform values: 0=SUPER SAW, 4=PULSE, 5=SAW, 6=TRI
WAVE_SAW = 5
# Osc2Waveform values: 0=PULSE 1=TRI 2=SAW 3=NOISE
# FilterType: 0=HPF 1=BPF 2=LPF ;  CutoffSlope: 0=-12 1=-24 dB/oct
# OscillatorBalance: 0=OSC1 .. 64=center .. 127=OSC2
# Bipolar depth params (-64..+63): 64 = zero.


def dataset1(off, value):
    full = 0x4000 + off
    addr = [0x01, 0x00, (full >> 8) & 0x7F, full & 0x7F]
    data = [value & 0x7F]
    chk = (0x80 - (sum(addr + data) & 0x7F)) & 0x7F
    return bytes([0xF0, 0x41, 0x10, 0x00, 0x06, 0x12] + addr + data + [chk, 0xF7])


def base_patch(**overrides):
    """A clean static measurement patch: held amp, no FX, neutral modulation."""
    p = {'Osc1Wave': WAVE_SAW, 'Osc1C1': 64, 'Osc1C2': 0, 'OscBal': 0, 'Osc2Wave': 2,
         'FiltType': 2, 'CutSlope': 1, 'Cut': 100, 'Res': 0, 'CutKeyFollow': 64, 'FEnvDep': 64,
         'FA': 0, 'FD': 0, 'FS': 127, 'FR': 0,
         'AmpLvl': 120, 'AA': 0, 'AD': 0, 'AS': 127, 'AR': 0,
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
    """Decode the renderer's 24-bit (or 16-bit) stereo WAV to a MONO float array.
    Mono-summing cancels the stereo auto-pan/onset motion so envelopes are clean."""
    w = wave.open(path, "rb")
    sr, nch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
    raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.uint8).reshape(-1, nch * sw)
    chans = []
    for c in range(nch):
        b = raw[:, c * sw:c * sw + sw].astype(np.int32)
        if sw == 3:
            v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
            v = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / 8388608.0
        elif sw == 2:
            v = b[:, 0] | (b[:, 1] << 8)
            v = np.where(v & 0x8000, v - 0x10000, v).astype(np.float64) / 32768.0
        else:
            raise ValueError(f"unsupported sample width {sw}")
        chans.append(v)
    return sr, np.mean(chans, axis=0)


# ---------------------------------------------------------------------------
# Signal metrics
# ---------------------------------------------------------------------------

def amp_envelope(L, sr, win_ms=15.0, hop_ms=5.0):
    w = int(sr * win_ms / 1000); h = int(sr * hop_ms / 1000)
    env = np.array([np.sqrt((L[i:i + w] ** 2).mean()) for i in range(0, len(L) - w, h)])
    t = np.arange(len(env)) * h / sr
    return t, env


def _medfilt(x, k=15):
    k |= 1; pad = k // 2
    xp = np.pad(x, pad, mode='edge')
    return np.array([np.median(xp[i:i + k]) for i in range(len(x))])


def welch_psd(L, sr, t0, t1, nfft=8192):
    seg = L[int(t0 * sr):int(t1 * sr)]
    win = np.hanning(nfft); step = nfft // 2
    acc = np.zeros(nfft // 2 + 1); n = 0
    for i in range(0, len(seg) - nfft, step):
        acc += np.abs(np.fft.rfft(seg[i:i + nfft] * win)) ** 2; n += 1
    if n == 0:  # window shorter than nfft -> single fft
        s = seg * np.hanning(len(seg))
        return np.fft.rfftfreq(len(seg), 1 / sr), np.abs(np.fft.rfft(s)) ** 2
    return np.fft.rfftfreq(nfft, 1 / sr), acc / n


def resonant_peak_hz(L, sr, t0=0.85, t1=1.30, fmin=70.0, fmax=20000.0):
    """With high resonance the filter rings at its corner -> spectral peak = cutoff Hz."""
    f, p = welch_psd(L, sr, t0, t1)
    k = (f >= fmin) & (f <= fmax)
    ps = np.convolve(p[k], np.ones(5) / 5, mode='same')
    return float(f[k][int(ps.argmax())])


# ---------------------------------------------------------------------------
# Envelope-time estimators
# ---------------------------------------------------------------------------

def attack_time(L, sr, hold):
    """10%->90% rise toward the sustain plateau. Median-filtered to ignore the
    fixed onset transient the firmware emits in the first ~0.7 s."""
    t, env = amp_envelope(L, sr)
    em = _medfilt(env, 15)
    i0 = int(np.searchsorted(t, hold * 0.5)); i1 = int(np.searchsorted(t, hold * 0.95))
    sus = max(em[i0:i1].mean(), 1e-9); e = em / sus
    a = b = None; end = int(np.searchsorted(t, hold))
    for i in range(end):
        if a is None and e[i] >= 0.1: a = t[i]
        if e[i] >= 0.9: b = t[i]; break
    return round(b - a, 4) if (a is not None and b is not None) else 0.0


def decay_time(L, sr, hold):
    """Peak -> within 5% of the sustain plateau (sustain probe at AS=40)."""
    t, env = amp_envelope(L, sr)
    end = int(np.searchsorted(t, hold)); seg = env[:end]
    i_start = int(np.searchsorted(t, 0.45))  # skip onset transient
    if end - i_start < 5: return 0.0
    sus = max(seg[int(end * 0.8):end].mean(), 1e-9)
    pk = seg[i_start:].max(); ipk = i_start + int(seg[i_start:].argmax())
    for i in range(ipk, end):
        if seg[i] <= sus + 0.05 * (pk - sus): return round(t[i] - t[ipk], 4)
    return round(t[end - 1] - t[ipk], 4)


def release_time(L, sr, offt):
    """Note-off at offt -> time to drop to 10% (-20 dB) of the pre-off level."""
    t, env = amp_envelope(L, sr)
    i0 = int(np.searchsorted(t, offt))
    base = max(env[max(0, i0 - 8):i0].mean(), 1e-9)
    for i in range(i0, len(env)):
        if env[i] <= 0.1 * base: return round(t[i] - offt, 4)
    return round(t[-1] - offt, 4)


def steady_level(L, sr, t0, t1):
    t, env = amp_envelope(L, sr)
    i0 = int(np.searchsorted(t, t0)); i1 = int(np.searchsorted(t, t1))
    return env[i0:i1].mean()


# ---------------------------------------------------------------------------
# Sweeps
# ---------------------------------------------------------------------------
ENV_PROBE = dict(Osc1Wave=WAVE_SAW, Osc1C2=0, OscBal=0, Cut=127, Res=0, FEnvDep=64)


def sweep_cutoff():
    """JP CutoffFrequency value -> filter corner in Hz, via resonant-peak probe."""
    out = []
    for v in list(range(0, 128, 4)):
        w = render(base_patch(Cut=v, Res=115, Osc1Wave=WAVE_SAW, OscBal=0,
                              CutKeyFollow=64, AmpLvl=110), f"/tmp/sw_cut_{v}.wav",
                   note=36, seconds=1.6, hold=1.4)
        sr, L = read_wav(w)
        out.append((v, round(resonant_peak_hz(L, sr))))
    return out


def sweep_resonance():
    """JP Resonance value -> resonant peak gain (dB over passband), cutoff fixed mid."""
    out = []
    for v in range(0, 128, 12):
        w = render(base_patch(Cut=70, Res=v, Osc1Wave=WAVE_SAW, OscBal=0,
                              CutKeyFollow=64, AmpLvl=110), f"/tmp/sw_res_{v}.wav",
                   note=36, seconds=1.6, hold=1.4)
        sr, L = read_wav(w)
        f, p = welch_psd(L, sr, 0.85, 1.30)
        band = (f >= 100) & (f <= 12000)
        pb = p[band]
        peak = 10 * np.log10(max(pb.max(), 1e-20))
        floor = 10 * np.log10(max(np.median(pb), 1e-20))
        out.append((v, round(peak - floor, 2)))
    return out


def sweep_amp_attack():
    out = []
    for v in range(0, 128, 8):
        w = render(base_patch(AA=v, AD=0, AS=127, AR=0, **ENV_PROBE),
                   f"/tmp/sw_att_{v}.wav", seconds=6.0, hold=5.0)
        sr, L = read_wav(w); out.append((v, attack_time(L, sr, 5.0)))
    return out


def sweep_amp_decay():
    out = []
    for v in range(0, 128, 8):
        w = render(base_patch(AA=0, AD=v, AS=40, AR=0, **ENV_PROBE),
                   f"/tmp/sw_dec_{v}.wav", seconds=6.0, hold=5.0)
        sr, L = read_wav(w); out.append((v, decay_time(L, sr, 5.0)))
    return out


def sweep_amp_release():
    out = []
    OFFT = 0.5
    for v in range(0, 128, 8):
        w = render(base_patch(AA=0, AD=0, AS=127, AR=v, **ENV_PROBE),
                   f"/tmp/sw_rel_{v}.wav", seconds=8.0, hold=OFFT)
        sr, L = read_wav(w); out.append((v, release_time(L, sr, OFFT)))
    return out


def sweep_amp_sustain():
    refw = render(base_patch(AA=0, AD=8, AS=127, AR=0, **ENV_PROBE),
                  "/tmp/sw_susref.wav", seconds=3.0, hold=2.5)
    sr, RL = read_wav(refw); reflvl = max(steady_level(RL, sr, 1.5, 2.3), 1e-9)
    out = []
    for v in range(0, 128, 8):
        w = render(base_patch(AA=0, AD=8, AS=v, AR=0, **ENV_PROBE),
                   f"/tmp/sw_sus_{v}.wav", seconds=3.0, hold=2.5)
        sr, L = read_wav(w)
        out.append((v, round(float(steady_level(L, sr, 1.5, 2.3) / reflvl), 4)))
    return out


if __name__ == "__main__":
    import json
    print(f"rom={ROMDIR}\nrender={RENDER}")
    only = sys.argv[1:] if len(sys.argv) > 1 else None
    sweeps = {
        "cutoff_hz": sweep_cutoff,
        "resonance_db": sweep_resonance,
        "amp_attack_s": sweep_amp_attack,
        "amp_decay_s": sweep_amp_decay,
        "amp_release_s": sweep_amp_release,
        "amp_sustain": sweep_amp_sustain,
    }
    calib = {}
    for name, fn in sweeps.items():
        if only and name not in only:
            continue
        print(f"... {name}", flush=True)
        calib[name] = fn()
        print(f"  {name}: {calib[name]}", flush=True)
    json.dump(calib, open("/tmp/jp_calib.json", "w"), indent=1)
    print("wrote /tmp/jp_calib.json")
