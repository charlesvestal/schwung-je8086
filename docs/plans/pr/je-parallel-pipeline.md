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
