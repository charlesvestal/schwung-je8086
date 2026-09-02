#!/usr/bin/env python3
"""
jp_voice_onsets.py -- count NOTE STARTS by watching the firmware, not the audio.

jp8000_render with JP_UC_LOG=<path> dumps, per 128-sample block, the number of
H8S->ASIC register writes bucketed by (asic, addr>>8): a raw little-endian
uint32 array of shape (nblocks, 4, 64).

When the firmware starts a note it programs a voice, and that voice's address
region goes from silent to written.  A rising edge in a previously-quiet bucket
is therefore a note start -- observed at the source, so it does not care about
the patch's attack time, its delay tail, or its chorus, all of which defeat an
audio-domain onset detector on JP-8000 pad patches.

Usage:  jp_voice_onsets.py <log> [t_start] [t_end]
"""
import sys
import numpy as np

BLOCK = 128
SR = 88200.0
NBUCKETS = 256


def load(path):
    d = np.fromfile(path, dtype='<u4')
    n = len(d) // NBUCKETS
    d = d[:n * NBUCKETS].reshape(n, NBUCKETS)
    t = np.arange(n) * BLOCK / SR
    return t, d


def voice_onsets(t, d, t0=0.0, t1=None, quiet_blocks=40, merge_ms=15.0):
    """Rising edges out of silence, merged across buckets that fire together."""
    n = len(t)
    if t1 is None:
        t1 = t[-1] + 1.0
    edges = []
    active = d > 0
    for b in range(d.shape[1]):
        a = active[:, b]
        if not a.any():
            continue
        # a block is an edge if written now and silent for the preceding window
        for i in np.where(a)[0]:
            lo = max(0, i - quiet_blocks)
            if i == 0 or not a[lo:i].any():
                if t0 <= t[i] <= t1:
                    edges.append((t[i], b))
    edges.sort()
    merged = []
    for tt, b in edges:
        if merged and tt - merged[-1][0] < merge_ms / 1000.0:
            merged[-1][1].append(b)
        else:
            merged.append([tt, [b]])
    return merged


def describe(b):
    return f"asic{b // 64}:{(b % 64) << 8:#06x}"


if __name__ == "__main__":
    path = sys.argv[1]
    t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    t1 = float(sys.argv[3]) if len(sys.argv) > 3 else None
    t, d = load(path)
    ev = voice_onsets(t, d, t0, t1)
    print(f"{path}: {len(t) * BLOCK / SR:.2f}s, window [{t0}, {t1 if t1 else t[-1]:.2f}]")
    print(f"  voice starts: {len(ev)}")
    for i, (tt, bs) in enumerate(ev):
        gap = f"  (+{tt - ev[i-1][0]:.3f}s)" if i else ""
        print(f"    {i+1:3d}  t={tt:7.3f}s  {' '.join(describe(b) for b in bs)}{gap}")
    if len(ev) > 2:
        iois = np.diff([e[0] for e in ev])
        print(f"  IOI mean={iois.mean():.3f}s sd={iois.std():.3f}s min={iois.min():.3f} max={iois.max():.3f}")
