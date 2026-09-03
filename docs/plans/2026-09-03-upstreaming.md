# Upstreaming to dsp56300/gearmulator

Status 2026-09-03. Branch `rebase-onto-upstream`, on upstream `c5dddcd0`, tagged
`upstream-rebase-2026-09-03`. Everything below was measured on a Raspberry Pi 4B
rev 1.5 @ 1.8 GHz, Debian 13 arm64, PSU + wifi, `throttled=0x0` verified before
and after each run.

## What we have

Two independent pieces of work.

**A. Engine optimizations** — always on, bit-exact, help any host. Timer event
horizon, H8S page-mapped memory access, ESP per-core dirty tracking, asmjit
Assembler instead of Builder, dense ARM64 emitter.

**B. A parallel ASIC pipeline** — opt-in, off by default. The JP-8000 chain is
four ASICs in series, so it does not parallelise by splitting a sample across
cores; it PIPELINES, one stage per core, each handing its neighbour the GRAM
words that cross the boundary. Throughput is bounded by the slowest stage rather
than their sum.

## The numbers

Steady-state, boot excluded (see *Measurement traps* below):

| Configuration | RT factor | vs previous |
|---------------|-----------|-------------|
| Stock upstream, serial | 0.433x | — |
| + engine optimizations | 0.800x | **1.85x** |
| + 4-stage pipeline, through `Device` | **2.26x** | **2.98x** |
| **Total** | | **5.2x** |

Engine-level (`bench_je*`, excludes Device):

| | serial | 2 stages | 3 stages | 4 stages |
|---|--------|----------|----------|----------|
| fork | 0.85x | 2.06x | 2.58x | 2.94x |
| threads | — | 2.01x | 2.62x | 2.92x |

Threads match fork at every depth. The pipeline ceiling is set by stage 0
(H8S + ASIC0) at 3.76 us/sample against an 11.34 us budget: 11.34/3.76 = 3.02x,
which is what four stages deliver. Splitting the H8S from ASIC0 is the only way
past it.

**The gains are architecture-specific.** The same engine optimizations measured
on Apple Silicon: 6.92x -> 6.93x, i.e. nothing. A narrow in-order-ish core with
slow memory pays for the per-instruction timer loop, the pointer-table
indirection and the extra ESP loads/stores; a wide out-of-order core absorbs
them. Do not quote 1.85x to a desktop reviewer. x86 desktop is UNMEASURED.

## Correctness evidence

- Engine optimizations: 5 of 6 corpus scripts byte-identical before/after
  (`tools/ab/bitexact.sh`). The sixth, `performance_select`, is excluded as
  nondeterministic in the render path, independent of our changes.
- Pipeline: the raw ASIC3 tap is byte-identical to serial and identical across
  runs; the wav is the serial stream delayed by a fixed 3 frames (0.034 ms).
- Determinism required a FIXED delay, not a bounded one. See below.
- Cross-platform: `sustained_c4` hashes the same on macOS/arm64 and Linux/arm64,
  so a reviewer can reproduce a hash rather than take our word.

## The PRs

Ours are bundled by discovery order, not by what upstream should review, so most
of these need a commit split rather than a cherry-pick. `787c8dfe` alone carries
four separate candidates.

### 1. `setFlushDenormalsToZero()` is a no-op outside MSVC
`framework/baseLib/os.cpp`, ~10 lines. `HAVE_SSE` is never defined project-wide
(only locally inside `rmlRendererJuce.cpp`), so denormal flush was never enabled
on any Linux or macOS build, x86-64 or ARM, for any device — Virus included.
Key the SSE path off `__SSE__`, add the aarch64 FPCR FZ path.

Lead with this one: it is a real bug in their code, needs no JP-8000 knowledge,
and is unrelated to everything else. Split it out of `04098db8` — the
`base.cmake` flags and `dspSingle.cpp` sleep beside it are Move-specific.
The same dead gate in `filesystem.cpp` and `synthLib/os.cpp` only guards an
`#include`; mention it, do not widen the PR.

### 2. Remove the MIDI-out `printf`
`cpu/h8s/h8sdevices.hpp`, 5 lines. A `write()` syscall per MIDI byte from the
emulation thread, in two places. Ships as-is.

### 3. H8S page bitmap
`cpu/h8s/h8s.hpp`. Every instruction fetch previously probed a 16 M-entry
pointer table; an 8 KB bitmap answers first. Bit-exact.

### 4. H8S wide-access fast path
Same file, separate PR so each is judged alone. 16/32-bit accesses that lie in
one unmapped page read `memory[]` directly. **The per-byte `clockMem()` calls are
kept verbatim, so cycle counts — and the ASIC sample cadence — are unchanged.**
Put that sentence in the commit message; it is what earns the merge.

### 5. Timer event horizon
`cpu/h8s/h8sdevices.hpp` + `instrStartCycles` in `h8s.hpp`. The largest single
engine win. Nothing observable happens between compare-match/overflow/IRQ
events, so skip to the next one; register accesses catch up first.

Correctness argument, verified: `timers.tick()` runs AFTER `emu.step()`, so the
old code updated to end-of-instruction cycles, and `catchUp()` uses
`instrStartCycles`, which is exactly that point. Deferred increments equal
per-instruction ones because `inc` is a difference of floors, additive across
any split. Pre-empt two questions: the skip conditions in `update()` and in the
`nextEvent` recompute must stay in lockstep, and `write()` resets `nextEvent`.

### 6. `a64::Assembler` instead of `a64::Builder`
`esp/esp_jit_types.h`, `esp/esp_jit_arm64.h`. Six lines. The ESP program is
straight-line with no labels or node edits, so it is a drop-in with identical
encodings; `Builder::finalize()` was re-walking every node. Clean up first:
`m_asm.finalize()` is guarded by `#if JIT_X64`, which keys off the architecture
rather than the builder type.

### 7. ESP per-core dirty tracking
`esp/esp_opt.hpp`, `esp/esp.hpp`, `jeLib/je8086devices.h`. The change is ~15
lines: a `cores` mask threaded through `setProgramDirty`/`genProgram` and the two
`writeuC` call sites, where core 0 owns `intmem` words `0..0x3ff` and core 1 the
rest. Everything else in that hunk is instrumentation and stays here. Comment the
0x56 path's straddling mask.

### 8. Dense ARM64 emitter + ring mirror
`esp/esp_jit_arm64.cpp`, `esp/esp_opt.hpp`, `esp/esp.hpp`. The only change that
alters generated-code semantics; expect the most review.

Lead with `coefsShifted`, which is easy to defend: `shiftAmount` is one of
{3,5,6,7}, `coef` is `int8`, so `coef << 4 <= 2032` fits `int16`, and
`(P << (7-s)) >> 7 == P >> s` exactly under arithmetic shift.

Settle before submitting: `nextIsDmac` uses the next statically emitted op,
consistent with the existing design (`esp_opt.hpp` already declares jumps
unsupported — offer our runtime check for that, it was an `assert` compiled out
under `-Ofast`); `jitEnter` zeroes `last_mulInput{A,B}_24` where the interpreter
carries them, not a regression and measured unreachable across the corpus; keep
`ESP_IRAM_MIRROR` unconditional, upstream will not want two ring layouts by
architecture. The x64 emitter takes `nextIsDmac` and ignores it — say so.

### 9. Parallel ASIC pipeline
`jeLib/jePipeline.{h,cpp}`, `jeLib/je8086.{h,cpp}`, plus the stage-scoped
`thread_local` state and hooks in `jeLib/je8086devices.h`.

**Open an issue first.** This is a design conversation, not a drive-by patch: it
adds threads to a device library, and the answer may be "yes, but through
`clap.thread-pool`" or "yes, but block-level". Pitch it as what it is — the
difference between 0.8x and 2.26x on an SBC, off by default, nothing changed for
anyone who does not ask.

Three things reviewers will and should ask:
- **Why not fork?** A plugin cannot fork. Threads also removed the ragged
  shutdown fork had.
- **Is it exact?** Yes, with a FIXED delay. A bounded window is not enough:
  how far the H8S actually leads still depends on thread timing, so runs differ
  (three runs at window 4 gave three hashes). Delivering one sample per sample
  rendered, taken a constant number of samples late, ties output to counters
  alone. Two samples of delay suffices — it only has to cover samples in flight.
- **Multiple instances?** The stage state is `thread_local`, which is correct
  per stage but has never been tested with two JP-8000s in one process. Be
  honest about that.

## Not upstreaming

- **The fork pipeline** and its Move-specific machinery: `g_je_parallel_mode`
  file-scope globals, the shm rings, the `flock`, FIFO 20 clamping, core pinning
  for Move's layout. The threaded pipeline is the upstreamable form.
- **`base.cmake` `-funroll-loops -fomit-frame-pointer`** — global codegen flags
  on top of `-Ofast` with no per-change measurement.
- **`dspSingle.cpp` yield -> `usleep`** — a Move RT workaround that slows Virus
  boot for everyone. The busy-yield on a boot wait thread is worth an issue.
- **Snapshot v2** — a feature, not a perf fix: bespoke `FILE*` format, no
  in-class versioning, unchecked `fread`/`fwrite`. Propose before writing.
- **The test corpus and `bitexact.sh`** cannot go as-is: they drive
  `jp8000_render`, which depends on our snapshot support. The evidence comes
  from us running it, not from them. Porting a minimal renderer onto
  `jeTestConsole` would change that, and would be a genuine contribution.

## Known gaps — disclose these

- **`getExtraLatencySamples()` is not wired to the pipeline delay.** The delay is
  fixed and tiny but a host should be told. This is the one functional gap.
- **x86 desktop is unmeasured** for the engine optimizations. Apple Silicon says
  ~0; a desktop is architecturally closer to that than to an A72.
- **Two instances in one process** is untested with the pipeline.
- `performance_select` remains nondeterministic in the render path; the fixed
  delay work suggests why (unbounded run-ahead) but it has not been retested.

## Measurement traps

Three real mistakes made while producing the numbers above. Anyone continuing
this should know them.

**Time renders WITHOUT boot.** A `jp8000_render` run carries ~1.0-1.4 s of
snapshot load and JIT warm-up. Timing whole processes folded that into the rate
and understated everything: the engine optimizations read 1.74x when they are
1.85x, and the pipeline read 1.28x when it is 2.26x. Render 1 s and 9 s and take
the slope.

**Profile before believing an arithmetic story.** A plausible per-sample cost
model pointed at `JeThread`'s semaphore ring and produced a confident "batching
it is the next big win". `perf` said 64% `[JIT]`, 32.5% binary, **2.78% kernel** —
the ring cannot cost what the model claimed, and `SpscSemaphoreWithCount` is a
single atomic unless a waiter is actually blocked. The whole premise was the boot
artifact above.

**A dev Mac cannot see these gains.** 6.92x vs 6.93x on Apple Silicon for a
change worth 1.85x on A72. Benchmark on the target class.
