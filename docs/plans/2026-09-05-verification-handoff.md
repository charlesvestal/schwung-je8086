# Handoff — upstream PR verification, 2026-09-05

Read `docs/plans/pr/README.md` for the status table and the per-PR notes. This
file is the state of the VERIFICATION work and, more importantly, the ways these
measurements have already lied.

## Where the four PRs stand

| PR | branch | serial vs vanilla | state |
|----|--------|-------------------|-------|
| #293 timer horizon | `pr/h8s-timer-event-horizon` | **DIFFERS at 176.0 s** | draft, correction posted, cause OPEN |
| #294 dense emitter | `pr/esp-dense-arm64-emitter` | identical 378.7 s | READY |
| #296 h8s bitmap+wide | `pr/h8s-wide-access` | identical 378.8 s | draft, verified, could go ready |
| #297 pipeline | `pr/je-parallel-pipeline` | identical 378.8 s | draft, BLOCKED on the interaction below |

`#294 + #296 + #297` together in serial are byte-identical to vanilla over
378.8 s. The three compose cleanly. #293 breaks on its own.

## Two open problems

### 1. #293 diverges from vanilla at 176.0 s

Serial, no pipeline. 739 ms from 175.994 s, 1.19% of bytes, RMS ratio 0.00066
(-64 dB), NO recovery by 245 s. Starts at the demo's patch change to
`[5:Feedbacker]`. Two vanilla runs are byte-identical over 378.8 s, so it is the
change.

Ruled out analytically: the floor-difference arithmetic IS additive; the skip
conditions in `update()` and the `nextEvent` recompute ARE in lockstep; `write()`
does reset `nextEvent`; externally clocked channels are excluded from both loops;
`inc` cannot overflow `tcnt` on the `d >= inc` path.

Measured on the M1 and NOT TRUSTED (see traps): timer event stream and IRQ raise
cycles are identical between deferred and non-deferred over 294 s, yet audio
differs -- the two cannot both be right, and the M1 bisect contradicted itself.
**Redo on the Pi with a self-check per binary.**

### 2. #294 + #297 interact: pipeline is not bit-exact once the emitter is in

Pipeline alone vs its own serial: clean through 223.8 s. Add #294 and it differs
at 176.0 s: 737 ms, 0.77% of bytes, RMS ratio 0.000452 (**-67 dB**), and it does
**NOT** re-converge (still differing at 227 s). Both changes are individually
bit-exact in serial over ~378 s.

Lead: #294 makes GRAM a 512-entry mirrored ring reconciled by
`sync()`/`syncShared()`; #297 writes GRAM ACROSS STAGES via `writeHandoff()` ->
`writeGRAM()` -> `ramIdx()`. A handoff write landing while the receiving ASIC's
mirror halves are unreconciled fits the signature. NOT confirmed.

This is what blocks #297: its headline claim ("byte-identical, delayed exactly 2
samples") is false in a tree that also contains #294, and #294 is going upstream.

## THE TRAPS — read before measuring anything

**A bit-exactness window is only as long as the SHORTEST run in it.** #293 was
"verified" over 128.7 s and diverges at 176.0 s. The composite check that night
covered 129 s for the same reason: vanilla is the SLOWEST build, so its file ends
first and silently truncates every comparison against it. Always check the audio
length of both files, in seconds, before believing an "identical" result.

**Check the reference before instrumenting the suspect.** Four hypotheses about
the pipeline were eliminated before anyone checked whether the serial reference
itself contained the defect. It did (#293). Every bisect run that evening
compared against contaminated output.

**A wrong shift looks like "differs at 0.0 s".** Comparing a pipelined wav
against serial requires an offset equal to the delivery delay (2 frames = 12
bytes by default). A divergence at byte ~22,650 means the offset is wrong, not
that the build is broken.

**The M1 is fast but its serial is jittery at 223.8 s**, and the pipeline-branch
M1 bisect produced mutually contradictory results in one sitting. Use it to
REPRODUCE, never to conclude. The Pi is deterministic in serial over 378.8 s.

**Measure the magnitude on a build that contains ONLY the change under test.**
The interaction above was first reported as -74 dB and re-converging; on a build
without #293 it is -67 dB and persistent. The first figure was two defects added
together.

**Verify the two A/B binaries differ before trusting a run.** `build-fix` has
`GEARMULATOR_SOURCE` pointing at another session's scratchpad copy, so edits to
`libs/gearmulator` do nothing there and ninja reports "no work to do" while
emitting an identical binary. Use `build-ref` or check the cache.

## Harness facts

- `jeTestConsole` selects thread count only via a one-line `JE_DSP_THREADS` hook
  in `params` — keep it UNCOMMITTED, the real selector is the plugin setting.
- Boot is a few seconds, not the 60-90 s older notes claim.
- Audio is 24-bit/88.2 kHz stereo = **529,200 bytes per second**. Divide by this
  to convert any byte offset to seconds; do it for every result.
- Run lengths needed to pass 176 s: vanilla/serial ~410 s wall, #294-class ~250 s,
  3-stage pipeline ~120 s. Getting this wrong is trap #1.
- Reference files on the Pi in `~/tcrun/`: `L_b1.wav`/`L_b2.wav` are vanilla
  serial at 378.8 s -- the good long reference.

## Suggested order

1. #294 <-> #297 interaction. It blocks a PR; the mirror-ring lead is concrete;
   both builds are deterministic so it is tractable.
2. #293's cause, on the Pi.
3. Then decide ready/withdraw for #297 and #293.
