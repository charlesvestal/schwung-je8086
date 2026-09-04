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
