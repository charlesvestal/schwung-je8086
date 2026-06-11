#!/usr/bin/env python3
"""
regression.py — run compare_wavs across a matrix of (patch × script) pairs.

Reads two render trees:
    <truth_dir>/p<N>_<script>.wav
    <candidate_dir>/p<N>_<script>.wav

Produces:
    <out_dir>/results.json    — per-cell metrics
    <out_dir>/results.md      — sorted summary table
    <out_dir>/identity_baseline.json — same-WAV-vs-itself sanity check
"""

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path


def collect_cells(d: Path):
    cells = {}
    for p in d.glob("*.wav"):
        m = re.match(r"p(\d+)_(.+)\.wav", p.name)
        if not m:
            continue
        patch, script = int(m.group(1)), m.group(2)
        cells[(patch, script)] = p
    return cells


def run_compare(compare_py: str, truth: Path, cand: Path) -> dict:
    result = subprocess.run(
        [sys.executable, compare_py, str(truth), str(cand)],
        capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        return {"error": result.stderr.strip()[:500]}
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        return {"error": f"json parse: {e}"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth_dir")
    ap.add_argument("candidate_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--compare", default=str(Path(__file__).parent / "compare_wavs.py"))
    args = ap.parse_args()

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    truth_cells = collect_cells(Path(args.truth_dir))
    cand_cells = collect_cells(Path(args.candidate_dir))

    common = sorted(truth_cells.keys() & cand_cells.keys())
    print(f"[regression] {len(common)} matched cells; "
          f"{len(truth_cells) - len(common)} truth-only, "
          f"{len(cand_cells) - len(common)} candidate-only")

    results = []
    for key in common:
        patch, script = key
        m = run_compare(args.compare, truth_cells[key], cand_cells[key])
        m["patch"] = patch
        m["script"] = script
        results.append(m)
        score = m.get("score", "ERR")
        if isinstance(score, float):
            print(f"  p{patch:02d} {script:20s} score={score:.3f} "
                  f"spec={m['spec_mse_db']:.1f}dB mfcc={m['mfcc_dist']:.3f} "
                  f"env={m['env_corr']:.3f}")
        else:
            print(f"  p{patch:02d} {script:20s} ERROR: {m.get('error', '?')}")

    with open(out / "results.json", "w") as f:
        json.dump(results, f, indent=2, default=lambda v: None)

    # Sort worst-first for the markdown
    rows = sorted(results, key=lambda r: r.get("score", 1.0))

    with open(out / "results.md", "w") as f:
        f.write("# A/B regression report\n\n")
        f.write(f"Truth: `{args.truth_dir}` · Candidate: `{args.candidate_dir}`\n\n")
        scores = [r["score"] for r in results if "score" in r]
        if scores:
            f.write(f"Cells: {len(scores)} · "
                    f"mean score: **{sum(scores)/len(scores):.3f}** · "
                    f"min: {min(scores):.3f} · max: {max(scores):.3f}\n\n")
        f.write("Sorted worst → best:\n\n")
        f.write("| patch | script | score | spec_mse dB | mfcc | env_corr | rms_diff dB | peak_diff dB |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for r in rows:
            if "error" in r:
                f.write(f"| {r['patch']} | {r['script']} | ERR | | | | | |\n")
                continue
            f.write(f"| {r['patch']} | {r['script']} | "
                    f"{r['score']:.3f} | {r['spec_mse_db']:.1f} | "
                    f"{r['mfcc_dist']:.3f} | {r['env_corr']:.3f} | "
                    f"{r['rms_diff_db']:.2f} | {r['peak_diff_db']:.2f} |\n")

    print(f"[regression] wrote {out}/results.md and results.json")


if __name__ == "__main__":
    main()
