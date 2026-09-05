# PR: parallel ASIC pipeline (PR 10)

**Branch** `pr/je-parallel-pipeline`, 3 commits, 1134 insertions / 43 deletions
across 18 files, forked from `upstream-gearmulator/main` (`c5dddcd0`).
**State** DRAFT PR #297 open.

## It went as a PR, not an issue

The plan said issue-first. The reasons weakened under examination: the
`clap.thread-pool` alternative gives threading to CLAP hosts only, and the
project ships VST3 and AU too, so it cannot replace the plugin owning its
threads. What remained was cost, not design doubt -- and the maintainers had
said to send patches.

## Measured on the SHIPPING binary

Everything below was re-measured after the two fixes below, because the earlier
numbers came from a binary the branch no longer contains.

**Pi 4B @ 1.8 GHz, idle, palindrome serial,3,2,2,3,serial, 300 s each:**

| configuration | vs serial | realtime | run-to-run |
|---------------|-----------|----------|------------|
| serial | 1.000x | 0.42x | 0.32% |
| 2 stages | 1.575x | 0.64x | 0.01% |
| 3 stages | **2.143x** | **0.94x** | 0.24% |

Within noise of the pre-fix binary (+0.23%, +0.81%, -0.38%), so both fixes are
free.

**THE PIPELINE ALONE DOES NOT REACH REALTIME on a Pi.** 0.94x is borderline. The
1.8x and 2.26x figures in the older notes were measured with our engine work
underneath. Keep the two claims apart.

**Apple M1 (busy desktop), same binary:** serial 1.000x, 2 stages 1.873x,
**4 stages 0.333x** -- three times SLOWER than single-threaded. A pipeline runs at
the pace of its slowest stage and the M1's efficiency cores set that pace. 2-3
stages is the useful range; do not derive the count from `hardware_concurrency()`.

## Bit-exact, precisely

Output is the serial stream delayed by **exactly 2 samples**, byte-identical, at
2, 3 AND 4 stages. Shifts of 1, 3 and 4 fail immediately, so it is an exact
property. The delay does not grow with depth -- the parent blocks if a stage has
not reached the sample, making it a delivery contract rather than pipeline depth.

## The divergence at 223.8 s is UPSTREAM'S, not ours

Two runs of the same binary diverge at the demo's first program change. Chased it
properly:

| build | machine | mode | result |
|-------|---------|------|--------|
| **vanilla upstream** | M1 | serial | diverges at 223.8 s (6 of 6 pairs) |
| PR branch | M1 | serial | diverges at 223.8 s |
| PR branch | Pi | 3 stages | diverges at 223.8 s |
| vanilla upstream | Pi | serial | identical over 378 s |
| PR branch | Pi | serial | identical over 380 s |

The vanilla binary was verified byte-identical (md5 `06aafbc7...`) to the one
those six pairs were run on. So it is timing-jitter dependent and latent in
upstream; the pipeline amplifies it, it does not cause it.

**Three wrong hypotheses before that, all disproved by experiment:**

1. *Control-ring overflow.* Peak occupancy 14 of 8192 across 8.6 M writes, zero
   overflows.
2. *Readback refresh race.* Freezing it changed nothing.
3. *Cross-thread `read()`.* Cutting it changed nothing -- and the divergence byte
   moved by ONE, which should have been the tell that I was nowhere near it.

The lesson: the divergence byte barely moved across four different binaries. A
fixed trigger with a variable outcome pointed at a pre-existing timing
sensitivity all along, and the fast discriminator was the M1 (jittery serial),
not more instrumentation on the Pi.

## Two real bugs found and fixed on the way

Neither caused the divergence; both are genuine.

- **`read()` had no ownership guard while `write()` did.** The H8S read a
  child-owned ASIC object while another thread was running it -- a data race, and
  the first thing a reviewer with TSan would hit. The owning stage now publishes
  its readback bytes and the parent answers from that snapshot.
- **The control ring had no space check** and stored indices modulo its capacity,
  so full and empty were indistinguishable. Now modulo 2N with a blocking
  producer, matching the gram ring.

## Harness note

`jeTestConsole` has no way to select a thread count, so measuring needs a
one-line `JE_DSP_THREADS` hook in `params`. Keep it UNCOMMITTED -- the real
selector is the plugin setting.

## All four PRs stacked: 0.43x -> ~2x realtime on a Pi

Branch `integration/all-prs` in the submodule (upstream main + the four PR
branches). **All four merge cleanly**, including #293 and #296 which both touch
`h8s.hpp`, and #294 and this PR which both touch `esp.hpp`.

| build | vs vanilla | realtime |
|-------|-----------|----------|
| vanilla upstream, serial | 1.000x | 0.43x |
| + #293 #294 #296, serial | 1.83x | 0.83x |
| + pipeline, 2 stages | 3.82x | 1.75x |
| + pipeline, 3 stages | ~4.5x | ~1.96-2.03x |

The engine PRs compose as predicted: 1.06 x 1.65 x 1.014 = 1.77 predicted, 1.83
measured. Nothing was double-counted.

**Quote the 3-stage figure as a range.** Its two runs differed by 7.7% of bytes
(196% vs 203% realtime), far worse than the 0.24% seen in the dedicated run.

**COMPOSITE BIT-EXACTNESS HOLDS.** Three engine PRs stacked, serial output
byte-identical to vanilla upstream over 68,297,472 bytes. That is the strongest
correctness evidence in the whole effort -- each PR claims to change nothing, and
stacked they still change nothing.

**But pipeline vs serial on the INTEGRATED build is not byte-exact.** Identical
except a **731 ms transient at 176.0 s**: 28,018 of 8,000,000 bytes differ
(0.35%) at RMS ratio **0.0002 (-74 dB)**, then the streams re-converge exactly at
the same +2 shift. Roughly 8 counts on 24-bit samples -- inaudible, but real.

Both configurations are individually DETERMINISTIC here: integrated serial
self-identical over 235 s, integrated 3-stage self-identical over **562 s**. So
this is a reproducible difference, not the timing nondeterminism seen on the
pipeline-only branch -- and the 223.8 s divergence did not appear at all in this
build, which fits it being jitter-dependent.

Cause not chased. Disclosed in the PR body rather than left to be found.

## The 176 s transient: four causes ELIMINATED, cause still unknown

Distinct from the 223.8 s nondeterminism. On the integrated build BOTH configs
are deterministic -- 3-stage self-identical over 562 s, spanning the event -- so
this is a REPRODUCIBLE difference between pipeline and serial, not a race. That
is what made it worth chasing: a deterministic bug is tractable where the other
was not.

What it is: 731 ms starting at 175.994 s, 0.35% of bytes, RMS ratio 0.0002
(-74 dB, ~8 counts on 24-bit samples), then EXACT re-convergence at the same
shift. Sits at the demo's patch change to `[5:Feedbacker]`.

Ruled out by experiment:

| hypothesis | test | result |
|------------|------|--------|
| delivery delay too tight | delay 2 -> 8 -> 64 | same divergence byte every time |
| control write applied a sample early/late | apply-time bias -1/0/+1 | 0 provably correct; +/-1 diverges at 0.0 s |
| handoff word set too narrow | {3,6,8} -> {10,10,10} | same divergence byte |
| control-write ring overflow | instrumented occupancy | peak 14 of 8192, zero overflow |

**The bias test is the useful one.** Shifting when a forwarded write takes effect
by ONE sample breaks the stream immediately, which proves the existing stamping
is exactly right -- and kills the most attractive remaining theory.

Still open: something during a patch load differs between running ASICs in one
thread and running them staged. Not the handoff payload, not the write timing,
not the delay. Next place to look is the ESP recompile path -- `genProgramIfDirty`
counts down three samples on the OWNING stage's clock, and a patch load is
exactly when that fires.

**Iteration note:** a 120 s run reaches ~235 s of audio at 3 stages but only
~95 s at serial (0.83x). The serial reference needs ~260 s. Getting that wrong
compares against a file that never reached the event.

## 2026-09-05: the pipeline was never inexact — the JIT was

The 176.0/223.8 s divergences of the pipeline against its own serial were the
uninitialised-register bug (`esp-arm64-jit-entry-state.md`): stage threads
change the JIT's caller and with it the entry garbage. Nothing in the
handoff/ring/uc-forwarding design was wrong — traces show GRAM handoffs, uC
writes and recompile ticks identical between serial and pipeline. On the fix,
2/3/4-stage output is byte-identical to serial over 200 s on M1 (Pi run in
flight). The PR's exactness claim should note it depends on the fix PR.

Wav-offset note: the constant 2-sample delivery delay can be absorbed before
recording starts (block phase), so serial-vs-pipeline wavs align at offset 2
OR 0. Probe, don't assume.
