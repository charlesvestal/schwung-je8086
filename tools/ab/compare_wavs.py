#!/usr/bin/env python3
"""
compare_wavs.py — audio similarity metrics for A/B testing JP-8000 emulator
vs candidate native engine.

Usage:
    compare_wavs.py <truth.wav> <candidate.wav> [--out report.json] [--plot diff.png]

Output JSON fields:
    spec_mse_db    — mean-squared error of log-magnitude spectrograms, dB
    mfcc_dist      — mean L2 distance between MFCC frames (perceptual)
    env_corr       — Pearson correlation of Hilbert-amplitude envelopes
    rms_diff_db    — overall RMS difference in dB
    peak_diff_db   — overall peak difference in dB
    lufs_diff      — LUFS-K integrated loudness difference (None if pyloudnorm unavailable)
    duration_diff  — sample-count difference (informational)
    score          — composite [0,1] where 1.0 = perfect match
"""

import argparse
import json
import sys
import numpy as np
import soundfile as sf
import librosa


def load_mono(path: str, target_sr: int = 44100):
    audio, sr = sf.read(path, dtype="float32", always_2d=True)
    if audio.shape[1] >= 2:
        audio = audio.mean(axis=1)
    else:
        audio = audio[:, 0]
    if sr != target_sr:
        audio = librosa.resample(audio, orig_sr=sr, target_sr=target_sr)
    return audio, target_sr


def db(x, eps=1e-10):
    return 20.0 * np.log10(np.abs(x) + eps)


def align_lengths(a, b):
    n = min(len(a), len(b))
    diff = abs(len(a) - len(b))
    return a[:n], b[:n], diff


def spec_mse_db(truth, cand, sr, n_fft=2048, hop=512):
    """Mean squared error of log-magnitude STFT, returned in dB scale.
    Lower is better. ≤ -30 dB = strong match on sustained content."""
    St = np.abs(librosa.stft(truth, n_fft=n_fft, hop_length=hop))
    Sc = np.abs(librosa.stft(cand,  n_fft=n_fft, hop_length=hop))
    n = min(St.shape[1], Sc.shape[1])
    Lt, Lc = db(St[:, :n]), db(Sc[:, :n])
    return float(np.mean((Lt - Lc) ** 2))


def mfcc_distance(truth, cand, sr, n_mfcc=20):
    """Mean L2 distance per frame between MFCC vectors.
    Lower is better. < 0.15 = perceptually very close."""
    Mt = librosa.feature.mfcc(y=truth, sr=sr, n_mfcc=n_mfcc)
    Mc = librosa.feature.mfcc(y=cand,  sr=sr, n_mfcc=n_mfcc)
    n = min(Mt.shape[1], Mc.shape[1])
    # normalize MFCC scales so first coefficient (loudness) doesn't dominate
    Mt = Mt[:, :n]; Mc = Mc[:, :n]
    diff = Mt - Mc
    per_frame = np.sqrt((diff ** 2).sum(axis=0))
    return float(np.mean(per_frame) / np.sqrt(n_mfcc))


def envelope_correlation(truth, cand, sr):
    """Pearson correlation of Hilbert amplitude envelopes (smoothed).
    Higher is better. > 0.95 = strong attack/release match."""
    # cheap envelope: short-window RMS
    win = max(1, sr // 200)  # 5 ms
    def env(x):
        x2 = x ** 2
        kern = np.ones(win) / win
        return np.sqrt(np.convolve(x2, kern, mode="same"))
    et, ec = env(truth), env(cand)
    n = min(len(et), len(ec))
    et, ec = et[:n], ec[:n]
    if et.std() < 1e-8 or ec.std() < 1e-8:
        return float(np.nan)
    return float(np.corrcoef(et, ec)[0, 1])


def rms(x):
    return float(np.sqrt(np.mean(x ** 2)) + 1e-12)


def lufs_diff(truth, cand, sr):
    try:
        import pyloudnorm as pyln
    except ImportError:
        return None
    meter = pyln.Meter(sr)
    Lt = meter.integrated_loudness(truth)
    Lc = meter.integrated_loudness(cand)
    return float(Lt - Lc)


def composite_score(metrics):
    """Map metrics to a single [0,1] quality score.
    Tuned so that "indistinguishable in a mix" → ~0.9+."""
    sm  = max(0.0, 1.0 - metrics["spec_mse_db"] / 100.0)
    mf  = max(0.0, 1.0 - metrics["mfcc_dist"] / 1.5)
    ec  = metrics["env_corr"] if not np.isnan(metrics["env_corr"]) else 0.0
    ec  = max(0.0, ec)
    rms_pen = max(0.0, 1.0 - abs(metrics["rms_diff_db"]) / 12.0)
    return float(0.35 * sm + 0.30 * mf + 0.20 * ec + 0.15 * rms_pen)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth")
    ap.add_argument("candidate")
    ap.add_argument("--sr", type=int, default=44100, help="resample to this rate")
    ap.add_argument("--out", help="JSON output path (default: stdout)")
    ap.add_argument("--plot", help="optional PNG diff plot")
    args = ap.parse_args()

    truth, sr = load_mono(args.truth, args.sr)
    cand,  _  = load_mono(args.candidate, args.sr)
    truth_a, cand_a, dur_diff = align_lengths(truth, cand)

    def _f(x):
        if x is None: return None
        v = float(x)
        return None if (np.isnan(v) or np.isinf(v)) else v

    metrics = {
        "spec_mse_db":  _f(spec_mse_db(truth_a, cand_a, sr)),
        "mfcc_dist":    _f(mfcc_distance(truth_a, cand_a, sr)),
        "env_corr":     _f(envelope_correlation(truth_a, cand_a, sr)),
        "rms_diff_db":  _f(db(rms(truth_a)) - db(rms(cand_a))),
        "peak_diff_db": _f(db(np.max(np.abs(truth_a))) - db(np.max(np.abs(cand_a)))),
        "lufs_diff":    _f(lufs_diff(truth_a, cand_a, sr)),
        "duration_diff_samples": int(dur_diff),
    }
    # composite_score consumes only the non-None float metrics
    safe = {k: (v if v is not None else 0.0) for k, v in metrics.items()
            if k in ("spec_mse_db", "mfcc_dist", "env_corr", "rms_diff_db")}
    metrics["score"] = _f(composite_score(safe))
    metrics["truth"] = args.truth
    metrics["candidate"] = args.candidate

    out = json.dumps(metrics, indent=2)
    if args.out:
        with open(args.out, "w") as f:
            f.write(out + "\n")
    else:
        print(out)

    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, axes = plt.subplots(3, 1, figsize=(12, 9))
        t = np.arange(len(truth_a)) / sr
        axes[0].plot(t, truth_a, label="truth", alpha=0.7, lw=0.5)
        axes[0].plot(t, cand_a,  label="candidate", alpha=0.7, lw=0.5)
        axes[0].set_title("waveforms"); axes[0].legend(); axes[0].grid(True, alpha=0.3)
        # spectrograms
        n_fft, hop = 2048, 512
        St = librosa.amplitude_to_db(np.abs(librosa.stft(truth_a, n_fft=n_fft, hop_length=hop)) + 1e-10)
        Sc = librosa.amplitude_to_db(np.abs(librosa.stft(cand_a,  n_fft=n_fft, hop_length=hop)) + 1e-10)
        axes[1].imshow(St, origin="lower", aspect="auto", cmap="magma", vmin=-80, vmax=0)
        axes[1].set_title("truth spectrogram")
        axes[2].imshow(Sc, origin="lower", aspect="auto", cmap="magma", vmin=-80, vmax=0)
        axes[2].set_title("candidate spectrogram")
        plt.tight_layout()
        plt.savefig(args.plot, dpi=80)


if __name__ == "__main__":
    main()
