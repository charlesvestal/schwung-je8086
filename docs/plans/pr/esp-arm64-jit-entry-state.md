# PR (new): esp: ARM64 JIT must load and store the ESP's persistent state

Branch: `pr/esp-arm64-jit-entry-state` (one commit, `34fe5a73`, on
`upstream-gearmulator/main`). NOT PUSHED YET.

## The bug

The ESP's ERAM latch chain (`eramReadLatch`, `eramWriteLatch`,
`eramWriteLatchNext`, `eramVarOffset`, `eramEffectiveAddr`), the DMAC inputs
(`last_mulInputA/B_24`) and the six accumulators are state that persists
across samples. The interpreter keeps them in the ESP object; the x64 JIT
backend loads and stores them through the `JitInputData` pointers; the ARM64
backend kept them purely in registers (x8–x15, x19–x24) and NEVER INITIALISED
them — on entry to the generated program each held whatever the calling code
left there.

With a fixed calling sequence that garbage is reproducible, so every run of
one binary is byte-identical and even different binaries can happen to agree.
It is still an accident: any change to the code around `callOptimized` —
another compiler, an added trace, a different PR in the tree, running the
ASIC from a worker thread — changes the entry garbage and with it the audio.

## What it explained (both open problems of 2026-09-05)

1. **#293 "diverges at 176.0 s"** — the timer-horizon PR's audio divergence
   was THIS, not its timer logic. Any binary-layout change shifts the garbage;
   the demo's patch change to `[5:Feedbacker]` at 176 s is where a stray ERAM
   write surfaces. M1 evidence: `vanilla vs #293` DIFFERS (176.7 s, −155 dB);
   `fix vs fix+#293` IDENTICAL over 200 s.
2. **"#294 ↔ #297 interaction"** — there was no interaction. #297's pipeline
   runs ASICs on stage threads, which changes the caller and therefore the
   entry garbage; #294 merely changed the flavor. With the fix, the pipeline
   is byte-identical to its own serial at 2, 3 and 4 stages.

## The chase (M1, one afternoon, all sample-bounded runs)

Divergence at 175.994 s → delivered stream identical at offset 2 →
asic3's RAW output differs → but its GRAM handoff inputs, uC writes and
recompile ticks are all IDENTICAL → per-sample state hash: asic3's **ERAM**
diverges at raw sample 277375, during BOOT, 172 s before anything is audible
→ full ERAM dump at that sample: exactly ONE word differs (`0x41581`:
serial 0, pipeline 0xe068c) → an ERAM write whose latch came from an
uninitialised register.

Traps that bit on the way (recorded in the handoff doc): trace windows
anchored to emulation start vs wav start (boot offset), and per-thread trace
files (a shared filename produced a bogus "everything differs").

## The fix

`jitEnter` loads the persistent state from the `JitInputData` pointers,
`jitExit` stores it back — exactly the x64 backend's contract. Registers with
no backing state (condition, temps) are zeroed so entry state is
deterministic. Cost: ~30 instructions per call, once per 384 emulated steps — measured
unmeasurable: B,A,A,B on the Pi (2026-09-05, 300 s wall each, temps 48–50 C,
load < 0.3) reads upstream serial 42%/43% and the all-PRs-fixed serial
80%/80% of realtime. 0.80x is exactly the pre-fix steady-state figure for the
optimization stack, so the persistence costs nothing visible at this scale.

## Validation so far (M1, clang, 200 s = past the 176 s trigger)

| comparison | result |
|---|---|
| fix serial run1 vs run2 | identical |
| fix+#297: serial vs 2/3-stage pipeline | identical |
| fix vs fix+#293 serial | **identical** (exoneration) |
| fix vs fix+#294(rebased) serial | identical |
| all-PRs (fix+293+294+296+297): serial vs fixed vanilla | identical |
| all-PRs: serial vs 2/3/4-stage pipeline | identical (offset 0 — see note) |
| **fix arm64 vs fix x86_64 (Rosetta)** | **identical** — the ARM64 JIT now matches the x64 backend byte-for-byte |
| vanilla arm64 vs vanilla x86_64 | **DIFFERS at 175.994 s** (33339 bytes, −124 dB) — the fix is what creates cross-backend parity |

Offset note: the pipeline delivers on a constant 2-sample delay, but the wav
starts at a block boundary after the "ROM PLAY" LCD event, so the wav-level
offset between serial and pipeline is 2 OR 0 depending on where boot's block
phase lands (# 293 shifts it). `cmp` must probe for the offset, not assume it.

## Pi validation (Pi 4B, gcc, Debian 13, 380 s of audio each, sample-bounded)

Run 2026-09-05 09:00–10:00 via `~/tcrun/pxrun.sh`; all five wavs exactly
201,096,236 bytes; compared with `pxcmp.py`:

| comparison | result |
|---|---|
| fix serial run1 vs run2 | IDENTICAL over 380 s |
| fix vs fix+#293 serial | **IDENTICAL over 380 s** — #293 exonerated on A72/gcc |
| fix vs all-PRs (293+294+296+297) serial | IDENTICAL over 380 s |
| all-PRs serial vs 3-stage pipeline | IDENTICAL over 380 s (offset 0, absorbed at recording start) |
| vanilla ref `L_b1.wav` vs fix serial | differs first at **175.994 s** — the bug's signature; the fix changes vanilla's output only where it was garbage-defined |

Combined with the M1 table above, every PR is bit-exact in isolation and in
combination on both machines once the entry-state fix is present.

## Upstream framing

Lead with the x64 parity: "the ARM64 backend now produces byte-identical
output to the x64 backend; before this fix its output depended on
uninitialised register contents". #293's earlier "not bit-exact" correction
comment should be retracted once this lands — the divergence was this bug,
not the timer change.
