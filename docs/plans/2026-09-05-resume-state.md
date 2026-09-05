# State snapshot — 2026-09-05, end of the root-cause session

Supersedes the open-problems half of `2026-09-05-verification-handoff.md`.
Both of its problems are RESOLVED; the traps in it remain valid, plus two new
ones recorded below.

## The one finding

The upstream ESP **ARM64 JIT enters generated programs with uninitialised
registers** for state that persists across samples (ERAM latch chain x8–x12,
DMAC inputs x14/x15, accumulators x19–x24). The x64 backend persists all of it
via `JitInputData`; arm64 ignored those pointers. Output is therefore
deterministic per binary but changes with ANY calling-context change — another
build of the same source, an added trace, or #297's stage threads. The audible
symptom is always "diverges at the demo's 176 s patch change (Feedbacker), a
second-ish of ~−115 dB noise": one stray ERAM write from an uninitialised
latch during BOOT, buried ~172 s until that patch reads it out.

This one bug was: #293's "not bit-exact" (retracted — timer logic fine), the
"#294↔#297 interaction" (never existed), and the reason vanilla arm64 ≠
vanilla x64.

## Where every PR stands (all on dsp56300/gearmulator)

| PR | branch (submodule) | state |
|---|---|---|
| **#300** JIT entry-state fix | `pr/esp-arm64-jit-entry-state` = `681ffb04`, 1 commit | **ready for review** — the gate |
| **#296** h8s bitmap+wide | `pr/h8s-wide-access` | **ready for review** (undrafted; independent of #300) |
| **#294** dense emitter | `pr/esp-dense-arm64-emitter` = `a7d63a52` ON TOP of `681ffb04` | open; force-pushed 2026-09-05 with review fixes; shows 2 commits until #300 lands |
| **#293** timer horizon | `pr/h8s-timer-event-horizon` (unchanged) | draft; retraction posted; **undraft when #300 merges** (promised publicly) |
| **#297** pipeline | `pr/je-parallel-pipeline` (unchanged) | draft; body's −67 dB qualification replaced, resolution comment posted; undraft when #300 merges |

Old #294 head preserved at `backup/pr294-pre-fix` (52735ea2).

Folded into #294 during the force-push: reviewer's `ESP_IRAM_MIRROR` →
`JIT_ARM64` derivation (MSVC ARM64 hole), reviewer's UB shift → multiply, our
wrap-aware `nextIsDmac` scan (last op's successor is the next call's first op
once state persists), and the persistence itself merged into the dense
emitter's jitEnter/jitExit.

## When #300 merges (the queued follow-ups)

1. Rebase `pr/esp-dense-arm64-emitter` onto new main → collapses to 1 commit;
   force-push #294.
2. Undraft #293 (as promised in its retraction comment).
3. Undraft #297.
4. If #294's own "+65%" number gets re-quoted, re-measure #294 ALONE on the Pi
   (only the STACK was re-measured after the fix: B,A,A,B = 42/80/80/43%,
   i.e. ~1.9x, unchanged — the persistence costs nothing visible).

## Validation record (what proves what)

`docs/plans/pr/esp-arm64-jit-entry-state.md` holds both tables. Short form:
- M1/clang, 200 s each, sample-bounded: fix serial deterministic; fix+297
  pipeline == serial (2/3 stages); all-PRs stack serial == fixed vanilla and
  pipeline == serial (2/3/4 stages); fix vs fix+293 identical; **fixed arm64
  == x64 backend (Rosetta)**; vanilla arm64 ≠ vanilla x64 at 175.994 s.
- Pi 4B/gcc, 380 s each: fix self-check identical; fix == fix+293; fix ==
  all-PRs serial; all-PRs serial == 3-stage; fix vs vanilla ref L_b1 diverges
  exactly at 175.994 s (the bug's signature, expected).
- Drafts/posted texts: `docs/plans/pr/drafts-2026-09-05.md` (all posted).

## Harness & environment (for resuming measurement work)

- **M1**: run dir `../jp8000-m1-verify` (run.sh, cmp.py, diff_traces.py,
  tc_* binaries, wavs). Build: `cmake --build build-mac-tc --target
  JE8086TestConsole -j8` (~12 s). Harness hook (UNCOMMITTED by design):
  `python3 tools/harness/apply_tc_harness.py libs/gearmulator` adds
  `JE_TC_SECONDS` (bound by RENDERED SAMPLES — this is what makes the M1
  deterministic and usable) and `JE_DSP_THREADS` (auto-skipped on trees
  without the pipeline).
- **Diagnostic patchers** (uncommitted, idempotent): `tools/harness/
  apply_trace.py` (per-boundary GRAM/uC/delivery traces), `apply_hash.py`
  (periodic per-ASIC state hashes + ERAM dump). Both apply on top of the
  harness patch.
- **Pi**: `ssh jpbench@jp-bench.local`, runs in `~/tcrun` (pxrun.sh, pxcmp.py,
  pxperf.sh, P_*.wav results, L_b1.wav vanilla 378.8 s reference). Cross-build:
  docker `schwung-jp8000-builder`, cmake dir `build-pi-tc`, target
  JE8086TestConsole.
- **x64 A/B on the Mac**: the tc build is fat; `arch -x86_64 ./tc_… .` runs
  the x64 JIT backend under Rosetta. Never run two renders in one directory —
  they share `je8086_out.wav` (this ate one run).

## New traps (beyond the handoff's list)

- **Anchor trace windows to the FIRST RECORDED SAMPLE**, not emulation start:
  boot renders seconds of audio before the wav opens, and an unanchored window
  lands that far early (produced a false "everything identical" pass).
- **One trace file per thread**: stages sharing a filename clobber each other
  (produced a false "every record differs").
- **The serial↔pipeline wav offset is 2 OR 0 samples**: the constant 2-sample
  delivery delay can be absorbed before recording starts depending on boot's
  128-sample block phase (and #293 shifts that phase). Probe; never assume.

## Local branch/tree state

- Submodule `libs/gearmulator` checked out on `verify/allfixed` (= #300+293+
  294+296+297 merged) with the harness patch applied (uncommitted, fine).
- Verify branches: `verify/allfixed`, `verify/fix293`, `verify/fix297`,
  `verify/no293`, `verify/no294` — scratch, rebuildable, some predate the
  amended fix SHA.
- Parent repo submodule POINTER deliberately untouched.
- Pushed to `charlesvestal/gearmulator`: `pr/esp-arm64-jit-entry-state`,
  `pr/esp-dense-arm64-emitter` (forced). Everything else local.
