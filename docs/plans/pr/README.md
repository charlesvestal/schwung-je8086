# Per-PR working notes

One file per upstream PR. **These live in the PARENT repo on purpose.**

The PR branches are in the submodule (`libs/gearmulator`, pushed to our
gearmulator fork); these notes are in `schwung-jp8000`. Two different
repositories, so the notes CANNOT end up in a gearmulator PR no matter what is
committed or when. No "remember to drop the docs commit" discipline needed.

## Layout

| where | what |
|-------|------|
| `libs/gearmulator` branch `pr/<name>` | ONLY the commits that go upstream, forked from `upstream-gearmulator/main` |
| `docs/plans/pr/<name>.md` (here) | findings, measurements, decisions, dead ends |

A PR branch must contain nothing but its own change: no notes, no
instrumentation, none of our other 24 commits. Verify with
`git diff --stat upstream-gearmulator/main..pr/<name>` before opening anything.

## Status

**2026-09-05: ROOT CAUSE FOUND for both open problems** (#293's 176 s
divergence AND the "#294↔#297 interaction"): the upstream ARM64 JIT reads
uninitialised registers — entry state depends on the CALLER, so any binary
layout change or a worker thread changes the audio. One fix repairs both, and
makes arm64 byte-identical to the x64 backend. See
`esp-arm64-jit-entry-state.md`. Fix PR is **#300**; #294 force-pushed onto it; #293 retraction and #297 resolution posted 2026-09-05.

| PR | branch | state |
|----|--------|-------|
| timer event horizon | `pr/h8s-timer-event-horizon` | **#293 — NOT BIT-EXACT.** Diverges at 176.0 s, -64 dB, no recovery. Correction staged; back to draft. — `h8s-timer-event-horizon.md` |
| denormal FTZ | — | **dropped**, no measurable benefit — `denormal-ftz-dropped.md` |
| MIDI printf | — | not started; reframed as hygiene, not performance |
| H8S page bitmap | `pr/h8s-wide-access` | **combined into DRAFT #296** — alone it is +0.01%, i.e. nothing |
| H8S wide access | `pr/h8s-wide-access` | **DRAFT PR #296 open** — +1.38% with the bitmap, bit-exact — `h8s-page-bitmap-and-wide-access.md` |
| a64::Assembler | — | not started |
| ESP dirty tracking | `pr/esp-per-core-dirty` | **#295 WITHDRAWN** — real but no user-visible benefit; revisit folded together with PR 7 — `esp-per-core-dirty.md` |
| dense ARM64 emitter | `pr/esp-dense-arm64-emitter` | **DRAFT PR #294 open. THE FLAGSHIP: +65% A72 / +45% M1, bit-exact 378.7 s** — `esp-dense-arm64-emitter.md` |
| PerformanceControlChannel | — | not started; upstream's bug, frame as a question |
| parallel pipeline | `pr/je-parallel-pipeline` | **DRAFT PR #297 open** — 2.14x on a Pi (0.42x -> 0.94x realtime), bit-exact on a fixed 2-sample delay — `je-parallel-pipeline.md` |

## Reusing a baseline between PRs

**The upstream baseline BINARY is reusable; its TIMING is not.**

- Binary: `tc_before_pr9` on the bench Pi is a clean `upstream-gearmulator/main`
  build of `JE8086TestConsole` (md5 `53fa6f0c829cd5053d373772fcc4e545`). Reuse it
  until upstream main moves; it saves the Docker rebuild, not the run.
- Bit-exactness references are deterministic and reusable outright. Audio-payload
  MD5 (`tail -c +45 x.wav | head -c N | md5sum`):
  `1c01a71ab04edf0f2a4598459fb5b953` over 200,435,712 bytes (378.7 s, Pi), and
  `2619badfaaae7642880e41c7a2c1e933` over 68,153,088 bytes (128.8 s, Pi AND M1,
  gcc AND clang). A future build can be checked against these without running
  upstream at all.
- **Timing must be re-measured in the same session as the change.** The point of
  B,A,A,B is that both sides share one thermal state, background load and
  ordering; a baseline from an hour ago reintroduces exactly the order-plus-drift
  confound that produced the retracted 2% FTZ result. Today's own data makes the
  case: the same upstream build read 42% and 43% in different sessions, and the
  same M1 binary produced 163.3 MB and 165.2 MB six minutes apart. The baseline
  run is the control, not overhead.

## Measure by rendered samples, not wall clock, when the question is bit-exactness

Equal wall clock is what makes a TIMING comparison fair, and it is why the faster
build writes a longer wav (compare the shorter as a prefix — see PR 6's note).
For a bit-exactness run it is pure waste: the Pi's 900 s "after" run produced the
same audio in half the time it was given. Bound those runs by rendered samples
instead.
