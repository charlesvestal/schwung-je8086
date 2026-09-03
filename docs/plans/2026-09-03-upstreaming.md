# Upstreaming to dsp56300/gearmulator

Status as of 2026-09-03. Our branch is `rebase-onto-upstream`, 15 commits on top
of upstream `c5dddcd0`, tagged `upstream-rebase-2026-09-03`.

## The PRs do not map 1:1 onto our commits

Our commits are bundled by the order we discovered things, not by what upstream
should review. `787c8dfe` alone carries FOUR separate candidates, and
`04098db8` mixes one genuine upstream bug fix with two changes that must not go
upstream. Every PR below needs the commit split, not cherry-picked.

| Change | Lives in | Files |
|---|---|---|
| FTZ never enabled outside MSVC | `04098db8` | `framework/baseLib/os.cpp` |
| MIDI-out `printf` on render path | `574e6c10` | `cpu/h8s/h8sdevices.hpp` |
| H8S page bitmap + wide accesses | `787c8dfe` | `cpu/h8s/h8s.hpp` |
| Timer event horizon | `787c8dfe` | `cpu/h8s/h8sdevices.hpp` (+ `instrStartCycles` in `h8s.hpp`) |
| ESP per-core dirty tracking | `787c8dfe` | `esp/esp_opt.hpp`, `esp/esp.hpp`, `jeLib/je8086devices.h` |
| `a64::Assembler` not `Builder` | `787c8dfe` | `esp/esp_jit_types.h`, `esp/esp_jit_arm64.h` |
| Dense ARM64 emitter + ring mirror | `bf974365` | `esp/esp_jit_arm64.cpp`, `esp/esp_opt.hpp`, `esp/esp.hpp` |

## Tier 1 — submit first, self-contained

**1. `setFlushDenormalsToZero()` is a no-op on every non-MSVC build.**
The strongest PR in the set and unrelated to the JP-8000. `HAVE_SSE` is never
defined project-wide -- `git grep HAVE_SSE` upstream finds it defined only
locally inside `rmlRendererJuce.cpp` -- so denormal flush was never enabled on
any Linux or macOS build, x86-64 or ARM, for any device including the Virus.
Fix: key the SSE path off `__SSE__`, add the aarch64 FPCR FZ path. ~10 lines.
The same dead gate appears in `framework/baseLib/filesystem.cpp` and
`framework/synthLib/os.cpp`, but there it only guards an `#include
<immintrin.h>` -- no behaviour, worth mentioning in the PR, not worth widening
it. Split this OUT of `04098db8`: the `base.cmake` codegen flags and the
`dspSingle.cpp` `usleep` in that commit are Move-specific and do not go.

**2. Remove the `printf("MIDI Out: [%02x]")` on the H8S serial write path.**
A `write()` syscall per MIDI byte from the emulation thread, in two places.
5 lines, ships as-is.

**3. H8S page-mapped access + wide-access fast path.** Split into two commits so
each is judged separately: the `mappedPages` bitmap (every instruction fetch
previously probed a 16 M-entry pointer table), then the 16/32-bit fast path.
Lead the message with the fact that earns it: the per-byte `clockMem()` calls are
kept verbatim, so cycle counts -- and the ASIC sample cadence -- are unchanged.

**4. `a64::Assembler` instead of `a64::Builder`.** Six lines. The ESP program is
straight-line with no labels or node edits, so it is a drop-in with identical
encodings. Clean up first: `m_asm.finalize()` is currently guarded by
`#if JIT_X64`, which keys off the architecture rather than the builder type.

## Tier 2 — good value, strip instrumentation first

**5. Timer event horizon.** The largest general CPU win. Contained in
`h8sdevices.hpp` plus one field in `h8s.hpp`. The correctness argument, verified
here: `timers.tick()` is called *after* `emu.step()`, so the old code updated to
end-of-instruction cycles, and `catchUp()` uses `instrStartCycles`, which is
exactly that same point. Deferred increments equal per-instruction ones because
`inc` is a difference of floors, additive across any split. Pre-empt two reviewer
questions: the skip conditions in `update()` and in the `nextEvent` recompute
loop are duplicated and must stay in lockstep, and `write()` conservatively
resets `nextEvent = 0`.

**6. ESP per-core dirty tracking.** The real change is ~15 lines: a `cores` mask
threaded through `setProgramDirty`/`genProgram` and the two `writeuC` call sites.
Core 0 owns `intmem` words `0..0x3ff`, core 1 the rest. Everything else in that
hunk is instrumentation and does not go upstream. Comment the one tricky line --
the 0x56 path's `(addr < 0x400 ? 1 : 0) | (addr + 4 >= 0x400 ? 2 : 0)`, a 5-word
write straddling the core boundary.

## Tier 3 — ARM64 codegen, one PR with numbers

**7 + 8. Dense emitter and `ESP_IRAM_MIRROR`.** Inseparable: the mirror is what
lets the emitter rebase the ring once and use immediate offsets. The only change
in the set that alters generated-code semantics, so it needs the most evidence.

Lead with `coefsShifted`, which is easy to defend: `shiftAmount` is one of
{3,5,6,7}, `coef` is `int8`, so `coef << 4 <= 2032` fits `int16`, and
`(P << (7-s)) >> 7 == P >> s` exactly under arithmetic shift.

Three things to settle before submitting:
- `nextIsDmac` uses the next *statically emitted* op. This is consistent with
  existing design, not a new assumption -- `esp_opt.hpp` already declares the
  jump ops unsupported. We made that a real runtime report (it was an `assert`,
  compiled out under `-Ofast`); offer that as part of the PR.
- `jitEnter` zeroes `last_mulInput{A,B}_24` each call while the interpreter
  carries them across samples. Not a regression (upstream left them as
  callee-saved garbage) and measured unreachable across our whole corpus -- 109
  program compiles in `patch_sweep`, 121 in `performance_select`, 15 in each
  note-only script, and the first emitted op is a MAC every time. Say so, with
  the limits.
- We made the mirror unconditional locally. Upstream will not want two ring
  layouts by architecture; keep it unconditional there too.

The `x64` emitter takes `nextIsDmac` and ignores it. Fine as a first step, but
say so -- upstream may want parity or may want the signature change deferred.

## Not upstreaming

- **Fork-parallel process split** (`d9230451`, `5bf69b80`, `6b6f8502`,
  `1abbe1e9`, `17c38c8f`, and the `je8086devices.h` half of `bf974365`).
  File-scope mutable globals encoding a process topology that exists because of
  Move's RT budget and `RLIMIT_RTPRIO 0`. If anything survives, it is the
  generalised ASIC split reframed as a threaded pipeline -- an issue to open,
  not a patch to send.
- **`base.cmake` `-funroll-loops -fomit-frame-pointer`** -- global codegen flags
  on top of `-Ofast` with no per-change measurement.
- **`dspSingle.cpp` yield -> `usleep(1000)`** -- a Move RT-starvation
  workaround that slows Virus boot for everyone. The underlying complaint (a
  busy-yield on a boot wait thread) is worth an issue.
- **Snapshot v2** (`6a63bb04`, `44f4383e`, `630980f8`) -- useful, but a feature,
  not a perf fix: bespoke `FILE*` format, no in-class versioning, most
  `fread`/`fwrite` returns unchecked, and upstream has its own persistence
  conventions. Propose before writing.

## Evidence

Every Tier 1-3 item claims to be behaviour-preserving. That claim is
bit-exactness, so the evidence is hashes, not the perceptual score in
`compare_wavs.py`.

`tools/ab/bitexact.sh <ref_build> <new_build> <rom_dir> tests/scripts` is the
harness. Result on the rebased tree vs the same tree with the three performance
commits removed: **5 identical, 0 differing, 1 excluded as unstable**
(`performance_select`; see CLAUDE.md -- it is the render path, not our changes).
Measured on Apple Silicon, so the ARM64 JIT and the mirrored ring are the paths
actually exercised.

**Upstream has no JP-8000 audio regression corpus.** `jeTestConsole` plays the
factory demo to a wav; there is no `add_test` and no ctest for je8086 anywhere.
Contributing the corpus and harness may be worth its own PR, and it is what any
bit-exactness claim rests on.

### Which numbers are measured, and where

Measured in this repo on 2026-09-03: the bit-exactness result above; the program
compile counts; `HAVE_SSE` never being defined upstream; the absence of an
upstream corpus.

**Not re-measured this session** -- carried from earlier device work, and each
needs a fresh number before it goes in a PR description: ESP compile cost
0.45 -> 0.18 ms per core (Assembler), H8S stage 10.2 -> 7.5 us/sample (timer
event horizon), ~120 compiles per patch change roughly halved (per-core dirty).

## Order

1. FTZ fix. 2. `printf` removal. Both trivially mergeable, and they establish a
careful contributor before the large one lands.
3. H8S page/wide access. 4. Assembler-not-Builder. 5. Timer event horizon.
6. Per-core dirty. 7. ARM64 codegen, with benchmarks and the bit-exact matrix.

Rebase before submitting, never after: our patches applied to upstream main with
zero source conflicts on 2026-09-03 because every file we touch was
byte-identical to our old base. That window closes as soon as upstream edits
`esp_opt.hpp` or `h8sdevices.hpp`.
