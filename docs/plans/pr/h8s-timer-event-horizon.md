# PR: h8s timer event horizon

**Branch** `pr/h8s-timer-event-horizon` in `libs/gearmulator`, commit `5f03f84b`,
forked from `upstream-gearmulator/main` (`c5dddcd0`).
**Size** 61 insertions, 2 files — `h8s.hpp` (+6), `h8sdevices.hpp` (+58/-3).
**State** measured, bit-exact, ready to open. NOT opened.

## The change

`Timers::tick()` ran once per instruction and stepped every elapsed cycle of
every running channel — ~180 iterations per sample per channel, almost all of it
`tcnt++`. Only cycles where a channel reaches `gra`, `grb` or `0` do anything
else. So compute the first cycle at which that can happen (`nextEvent`) and
until then `tick()` is one compare; the inner loop jumps to the next event.

`instrStartCycles` records `cycles` at instruction start, and `catchUp()` brings
the counters there on a register access, so the CPU never reads a stale `tcnt`.

## Measured — +6%, and NOT the biggest win

Upstream's own `jeTestConsole`, idle Pi 4B, counterbalanced B,A,A,B:

| position | build | speed | wav bytes |
|----------|-------|-------|-----------|
| 1st | before | 43% | 68,135,468 |
| 2nd | after  | 45% | 71,887,916 |
| 3rd | after  | 45% | 71,518,508 |
| 4th | before | 42% | 67,905,068 |

`after` wins in both middle positions; `before` reads 43% first and 42% last,
consistent with 50.6 -> 58.4 C drift. Wav sizes corroborate independently: 5.4%
more audio in the same wall clock.

The 43% baseline reproduces the "stock upstream serial 0.433x" figure measured
earlier with a different harness — a useful cross-check on both.

**An earlier claim in our plan docs that this was "the largest single engine
win" was never measured and is retracted.** At 6% against a bundle worth +85%,
something else does nearly all the work.

## Bit-exact — but not by comparing hashes

All 68,135,424 audio bytes identical.

**The raw SHA-256s DIFFER, and reporting that would be a false alarm.** Each run
is killed on a wall clock, so the faster build writes MORE audio. Compare the
shorter file as a prefix of the longer:

    cmp -n $(stat -c %s before.wav) -i 44:44 before.wav after.wav

This will recur on every PR measured this way.

## How it was built (the recipe for PRs 4, 5, 7, 8)

`787c8dfe` **cannot be cherry-picked.** Its timer hunks modify `syncCycles()`,
which upstream does not have — it is ours, from the snapshot work we are not
upstreaming, as are `saveState`/`loadState` in the same file.

    git checkout -b pr/<name> upstream-gearmulator/main
    git show <commit> -- <file> | git apply -3 -
    # resolve: keep the change, drop anything touching snapshot code
    # hand-apply any small hunks from files with mixed concerns (h8s.hpp)

## Reproducing the measurement

`jeTestConsole` is the right tool and better than ours: it self-triggers (waits
for LCD `PERFORM`, presses Rec+Hold, waits for `=== ROM PLAY ===`) and its `t0`
starts AFTER the demo begins, so **boot is excluded by construction**.

- Build: `ninja JE8086TestConsole` in `build-plugin` — NOT `build`, which only
  configures our own targets.
- It finds ROMs via CWD; copy the eight `.mid` files into the run directory. The
  path argument is ignored.
- Boot takes 60-90 s on a Pi before anything prints. A 75 s timeout sees nothing
  and looks like a hang — that mistake cost an hour here.
- **`bench_je` does NOT build on an upstream base**: `dsp_bench.cpp` needs
  `g_je_uc_read_capture`, `g_je_uc_write_capture` and `g_je_audio_tap`, all ours.

## For the PR description

Lead with the correctness argument, it is what earns the merge:

> `timers.tick()` runs AFTER `emu.step()`, so the old code updated to
> end-of-instruction cycles, and `catchUp()` uses `instrStartCycles`, which is
> exactly that point. Deferred increments equal per-instruction ones because
> `inc` is a difference of floors, additive across any split.

Pre-empt two questions: the skip conditions in `update()` and in the `nextEvent`
recompute must stay in lockstep, and `write()` resets `nextEvent`.

Disclose: measured on A72 only; x86 untested (see the companion doc's wording).

## RETRACTED 2026-09-05: this is NOT bit-exact

**The output diverges from vanilla at 176.0 s.** The original check covered
68,135,424 bytes = 128.7 s and stopped 47 s short of the first difference. The
measurement was not wrong; its SCOPE was.

Measured serial, idle Pi, against a vanilla `main` build, both runs long enough:

```
vanilla vs this branch:  DIFFERS at 93,135,918 = 176.0 s
  span      739 ms
  magnitude 1.19% of bytes, RMS ratio 0.00066  (about -64 dB)
  recovery  NONE, still differing at 245 s
```

Two vanilla runs are byte-identical over 378.8 s, so it is the change, not noise.
It begins exactly at the demo's patch change to `[5:Feedbacker]`.

**How it was found, and the lesson.** It surfaced while chasing what looked like a
pipeline-vs-serial transient in the all-PRs integration build. Four hypotheses
about the PIPELINE were eliminated first (delivery delay, control-write apply
time, handoff width, ring overflow) before checking the reference itself -- at
which point vanilla-vs-integration SERIAL showed the same divergence byte. The
pipeline was never involved.

**Check the reference before instrumenting the suspect.** Every bisect run that
evening compared against a serial file that already contained the defect.

**And: a bit-exactness window is only as good as the shortest run in it.** The
composite check that night covered 129 s for the same reason -- the vanilla
reference file ended there, because vanilla is the slowest build. A fast build
paired with a slow reference silently truncates the comparison.

## What is ruled out so far

- The floor-difference arithmetic IS additive across splits, as the commit claims.
- The skip conditions in `update()` and the `nextEvent` recompute ARE in lockstep.
- `write()` does reset `nextEvent`.
- Externally clocked channels are excluded from both loops.
- `inc` cannot overflow `tcnt` on the `d >= inc` fast path.

Open lead: upstream raises timer IRQs only from `tick()` after `step()`;
`catchUp()` can raise one MID-INSTRUCTION from inside a register read.
`pending_irqs` is sampled at instruction start, so it ought to be equivalent --
which is exactly the kind of reasoning that produced the retracted claim.

## Status

Correction staged at `scratchpad/pr293_correction.md`; the network was down when
it was found. #293 to go back to draft, then fix or withdraw. The other two
engine PRs are CLEAN over the same long comparison (#294 378.7 s, #296 204.7 s),
and #297's own serial output is identical to vanilla over 378.8 s.
