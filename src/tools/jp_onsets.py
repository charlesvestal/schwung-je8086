#!/usr/bin/env python3
"""
jp_onsets.py -- attack-transient (onset) detector for jp8000_render output.

Why this exists: max-absolute-sample-difference is NOT an arpeggiation
detector.  It gave two confident wrong answers (see
docs/plans/2026-09-02-jp8000-arp-parked.md).  An arpeggiator is a note
*generator*, so the only honest measurement is "how many attacks are in this
audio, and when".

Method: log-compressed spectral flux (half-wave rectified magnitude
difference), adaptive-median threshold, peak picking with a minimum
inter-onset interval.  Validated against a positive control of N separately
scripted notes -- see --self-test.

Reads 24-bit (or 16-bit) stereo WAV; a 16-bit read of a 24-bit file gives a
plausible, wrong envelope, so the width is taken from the header.
"""
import sys
import wave
import numpy as np


def read_wav(path):
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


def spectral_flux(x, sr, n_fft=2048, hop=512, gamma=100.0):
    win = np.hanning(n_fft)
    nframes = 1 + max(0, (len(x) - n_fft) // hop)
    if nframes < 3:
        return np.zeros(0), np.zeros(0)
    frames = np.lib.stride_tricks.as_strided(
        x, shape=(nframes, n_fft),
        strides=(x.strides[0] * hop, x.strides[0])).copy()
    frames *= win
    mag = np.abs(np.fft.rfft(frames, axis=1))
    logmag = np.log1p(gamma * mag)
    d = np.diff(logmag, axis=0)
    flux = np.maximum(d, 0.0).sum(axis=1)
    t = (np.arange(len(flux)) + 1) * hop / sr + n_fft / (2.0 * sr)
    return t, flux


def _moving_median(x, k):
    k |= 1
    pad = k // 2
    xp = np.pad(x, pad, mode='edge')
    return np.array([np.median(xp[i:i + k]) for i in range(len(x))])


def detect_onsets(x, sr, min_ioi_ms=60.0, ratio=2.0, floor_frac=0.06,
                  median_ms=300.0, n_fft=2048, hop=512):
    """Return (times, strengths). floor_frac is a fraction of the global
    flux maximum -- it keeps noise in near-silence from becoming onsets."""
    t, flux = spectral_flux(x, sr, n_fft=n_fft, hop=hop)
    if len(flux) == 0:
        return np.zeros(0), np.zeros(0)
    fps = sr / hop
    med = _moving_median(flux, max(3, int(median_ms / 1000.0 * fps)))
    thr = med * ratio + floor_frac * flux.max()
    cand = np.where((flux > thr) &
                    (flux >= np.roll(flux, 1)) &
                    (flux >= np.roll(flux, -1)))[0]
    picked = []
    min_gap = min_ioi_ms / 1000.0
    for i in cand:
        if picked and t[i] - t[picked[-1]] < min_gap:
            if flux[i] > flux[picked[-1]]:
                picked[-1] = i
            continue
        picked.append(i)
    picked = np.array(picked, dtype=int)
    return t[picked], flux[picked]


def report(path, **kw):
    sr, x = read_wav(path)
    times, strengths = detect_onsets(x, sr, **kw)
    rms = float(np.sqrt((x ** 2).mean()))
    peak = float(np.abs(x).max())
    print(f"{path}: {len(x)/sr:.2f}s @ {sr}Hz  rms={rms:.5f} peak={peak:.5f}")
    print(f"  onsets: {len(times)}")
    for i, (tt, ss) in enumerate(zip(times, strengths)):
        gap = f"  (+{tt - times[i-1]:.3f}s)" if i else ""
        print(f"    {i+1:3d}  t={tt:7.3f}s  strength={ss:8.2f}{gap}")
    if len(times) > 2:
        iois = np.diff(times)
        print(f"  IOI mean={iois.mean():.3f}s  sd={iois.std():.3f}s")
    return len(times)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:]]
    if not args:
        print(__doc__)
        sys.exit(1)
    for p in args:
        report(p)
        print()
