# PR: ESP dense ARM64 emitter

**Branch** `pr/esp-dense-arm64-emitter` in `libs/gearmulator`, commit `52735ea2`,
forked from `upstream-gearmulator/main` (`c5dddcd0`).
**Size** 243 insertions / 191 deletions across 7 files, all under `source/ronaldo/esp/`.
**State** measured on two machines, bit-exact, x86-64 compile-checked. NOT opened.

## The change

Each ESP program is emitted as one straight-line block instead of a sequence of
per-op calls, with two things removed from the inner loop:

- **Pre-shifted coefficients.** `shiftAmount` is one of {3,5,6,7} and `coef` is
  an `int8`, so `coef << (7 - shiftAmount)` is at most 2032 and fits `int16`;
  `(P * coef) >> shiftAmount` and `(P * coefShifted) >> 7` are then equal
  *exactly* under arithmetic shift. A plain MAC becomes one load and a fixed
  immediate shift rather than a variable one.
- **Mirrored iram/gram ring** (aarch64 only, `ESP_IRAM_MIRROR`). The ring becomes
  a 512-entry buffer addressed as `iramPos + mem` with no wrap, so the JIT
  rebases the pointer once per call and reaches every slot with an immediate
  offset instead of masking each access. `sync()` moves the single slot whose
  home changes when `iramPos` decrements; `syncShared()` does the same for the
  shared gram. `ramIdx()` keeps the interpreter and the host-side accessors on
  the same index, so the two never disagree.

`emitOp()` gains `nextIsDmac`: `last_mulInputA_24` / `last_mulInputB_24` have
exactly one reader, a `kDMAC`, so when the next emitted op is not one, those
writes are skipped. The x64 emitter takes the argument and ignores it, and
`ESP_IRAM_MIRROR` is 0 there — buffers and addressing are exactly as before.

## Measured: +65% on A72, +45% on M1

Upstream's own `jeTestConsole`, counterbalanced B,A,A,B at equal wall clock.
Byte totals are the figure to trust: "last interval speed" is a one-second
sample and reads 75% and 66% for two runs whose totals are 0.1% apart.

**Pi 4B @ 1.8 GHz, idle (load 0.05-0.2), 52 -> 58 C per run, throttle bit never
moved.** 300 s runs:

| position | build | speed | wav bytes |
|----------|-------|-------|-----------|
| 1st | before | 43% | 68,153,132 |
| 2nd | after  | 75% | 112,570,412 |
| 3rd | after  | 66% | 112,447,532 |
| 4th | before | 43% | 68,008,748 |

Mean 112,508,972 / 68,080,940 = **1.653**. `before` reads 43% in both positions
1 and 4 with totals 0.2% apart, so there is no thermal-drift artifact.

Corroborated at a second run length: the 900 s runs gave 337,946,156 / 200,435,756
= **1.686**, i.e. +69% against +65%, on the same machine on a different afternoon
hour and a different thermal ramp.

**Apple M1, 90 s runs.** Asked for specifically — "check if this negatively
influences performance on a beefy machine":

| position | build | speed | wav bytes |
|----------|-------|-------|-----------|
| 1st | before | 390% | 163,292,972 |
| 2nd | after  | 579% | 239,722,796 |
| 3rd | after  | 595% | 235,930,412 |
| 4th | before | 376% | 165,196,844 |

Mean 237,826,604 / 164,244,908 = **1.448**. **It does not regress on a wide
out-of-order core; it gains 45%.** Smaller than the A72's 65%, which is the
expected direction, and unlike the interpreter-level work this change alters the
code the CPU actually executes, so it shows up everywhere.

Disclose with the M1 figure: that machine was *not* idle. It is an actively used
desktop and carried about one core of steady background load (`coreaudiod`,
WindowServer, a live screen-sharing session) across all four runs. Counterbalancing
at equal wall clock tolerates a steady load; it does not make the box idle.

## Bit-exact over 378.7 s, and the M1's divergence is that machine, not this change

**378.7 s of audio byte-identical between upstream and this branch on the Pi**,
with a clean same-build self-check over the same window and a program change
inside it. 900 s runs:

    L_b1 vs L_b2 (upstream vs upstream): IDENTICAL over 200,435,712 bytes
    L_b1 vs L_a1 (upstream vs PR 9)    : IDENTICAL over 200,435,712 bytes

The M1 is a different story and worth recording, because it is what a reviewer
running this on a busy laptop will see: there, runs of the SAME binary diverge at
223.8 s. That is the machine, not the engine -- the Pi renders the identical
passage deterministically. Six same-build (upstream vs upstream) M1 pairs:

| pair | diverges at |
|------|-------------|
| r1 vs r2 | 118,437,001 |
| r1 vs r3 | 118,437,001 |
| r1 vs r4 | 118,437,019 |
| r2 vs r3 | 118,437,022 |
| r2 vs r4 | 118,437,001 |
| r3 vs r4 | 118,437,001 |
| **upstream vs PR 9** | **118,437,001** |

The cross-build divergence point is not merely inside the same-build spread, it
is the *modal* value of it -- so even on the machine that IS nondeterministic,
this change is indistinguishable from the harness's own noise.

**What is at that point is a program change.** The wav is 24-bit/88.2 kHz stereo
= 529,200 bytes/s, so 118,437,001 is 223.8 s in, and the log at 222 s shows the
LCD advancing to the next demo track, spelled out character by character as the
firmware rewrites it:

    222s | LCD: [=== ROM PLAY ===]
    222s | LCD: [6:Feedbacker    ]
    ...
    222s | LCD: [6:Volcano       ]

That matches the standing finding that performance *switching* is intermittently
nondeterministic while the performances themselves are not. A plausible mechanism
is in `jeTestConsole` itself: it pokes `setButton(kSwitch_Rec/Hold)` from the main
thread inside the LCD callback while `JeThread` is concurrently stepping the
emulator. MIDI events are offset-scheduled and deterministic; a direct button
poke is not.

**The first Pi runs were clean for the wrong reason.** 300 s of wall clock got
them to 128.8 s (before) and 212.7 s (after) of audio, both short of 223.8 s --
they never reached the switch at all. That was luck, not rigour, and it is
exactly the kind of thing that reads as evidence and is not. The 900 s runs above
are what actually establish the claim, and they cross it.

## Hashes a reviewer can reproduce

MD5 of the audio payload (`tail -c +45 x.wav | head -c N | md5sum`):

| window | md5 |
|--------|-----|
| 200,435,712 bytes (378.7 s), Pi, upstream AND PR 9 | `1c01a71ab04edf0f2a4598459fb5b953` |

and over the shorter 68,153,088-byte window (128.8 s), across machines:

| build | md5 |
|-------|-----|
| Pi 4B, gcc, Linux, Cortex-A72, **upstream** | `2619badfaaae7642880e41c7a2c1e933` |
| M1, clang, macOS, **upstream** | `2619badfaaae7642880e41c7a2c1e933` |
| M1, clang, macOS, **PR 9** | `2619badfaaae7642880e41c7a2c1e933` |

Two architectures, two toolchains, two operating systems, both builds: one hash.

## x86-64

Compile-checked for `x86_64` and `arm64` (`clang -fsyntax-only -target ...`) on
both the x64 emitter and a TU including `esp.hpp`. `ESP_IRAM_MIRROR` is 0 on
x86-64, where the buffers and the addressing are unchanged from upstream.
**Not measured on x86 hardware** — we have none, and Docker x86_64 on Apple
Silicon is translation whose timings mean nothing. Say so in the PR.

## How it was built

`bf974365` cannot be cherry-picked: its `esp.hpp` hunks are interleaved with our
snapshot code (`espRingSave`/`espRingLoad`, `saveState`/`loadState`), and
**upstream's `esp.hpp` has none of that** — verified, not assumed. Same recipe as
PR 6:

    git checkout -b pr/<name> upstream-gearmulator/main
    git show <commit> -- <file> | git apply -3 -
    # resolve every conflict by KEEPING upstream's side and re-adding only the
    # emitter change; here that meant keeping syncShared() and dropping
    # getIramPos(), which only saveState needed

Also dropped by hand: every `#ifdef JE_PROFILE` block, and the whole
fork-parallel ASIC split — `je8086devices.h` is untouched on this branch.

## The arch gate is LOAD-BEARING -- our own checklist was wrong

The checklist said "keep `ESP_IRAM_MIRROR` unconditional, upstream will not want
two ring layouts by architecture." **Following that would ship broken x86-64.**

The ring layout has to match the emitter, and the two emitters differ:

- `esp_jit_arm64.cpp` rebases the ring once and addresses every slot with an
  immediate offset. There is no `#if` in it -- it REQUIRES the mirrored layout.
- `esp_jit_x64.cpp` emits `and tempB, 0xff` for every access (lines 159, 280).
  `bf974365` never taught it the mirror, so it REQUIRES the masked layout.

Turn the mirror on for x86-64 without porting the mirrored addressing into the
x64 emitter and the JIT indexes `(mem + iramPos) & 0xff` while the interpreter
and host accessors index `(mem & 0xff) + iramPos` -- different slots whenever
`iramPos > 0`, which is nearly always. That is incorrect by construction, not
untested.

Since the emitter is chosen by architecture, so is the layout, and
`#if defined(__aarch64__)` is the correct expression of the coupling rather than
a placeholder. With the mirror off, `ramIdx()` reduces to
`(offset + iramPos) & IRAM_MASK` -- upstream's expression character for
character -- `mirrorFixup()` returns immediately, and the buffers are 256 entries
again. **x86-64 behaviour is unchanged, not merely untested.**

Porting the mirror into the x64 emitter is a fair follow-up, as its own change
with its own measurement, on hardware we do not have.

## Consequence for the other PRs

The bundle is +85% (0.433x -> 0.800x). PR 6 is +6% and this is +65%; compounded,
1.06 x 1.65 = 1.75. **These two account for very nearly the whole bundle**, which
leaves only a few percent to divide between PRs 4, 5, 7 and 8. Expect ESP dirty
tracking (PR 8) to be small in a throughput harness — it removes compile stalls,
not steady-state work — and measure it with `patch_sweep`, which actually changes
patches, rather than with this demo.

## 2026-09-05: rebased onto the entry-state fix; review comments folded in

`pr/esp-dense-arm64-emitter` now sits on `pr/esp-arm64-jit-entry-state`
(backup of the old head: `backup/pr294-pre-fix`). Amended in:
- reviewer: `ESP_IRAM_MIRROR` now derives from `JIT_ARM64` (MSVC ARM64 got
  256-entry buffers under a 512-entry addressing scheme);
- reviewer: `coef << n` UB replaced with `coef * (1 << n)`;
- ours: the `nextIsDmac` elision scan wraps to the program's first op (with
  entry/exit persistence the last op's successor is the next call's first op);
- ours: jitEnter/jitExit persistence merged into the dense emitter.
M1: serial byte-identical to fixed vanilla over 200 s. NOT pushed yet;
perf on the Pi must be re-measured before updating the PR (persistence adds
~30 instr per call, expected noise).

The "#294 ↔ #297 interaction" is retracted — it was the entry-state bug.
