# Where this is and what comes next

## State

Everything from the iOS session is on named branches, all pushed.

| repo | branch | contains |
|---|---|---|
| schwung-jp8000 | (current) | build_ios.sh, docs, `pgo/` profile |
| gearmulator | `ios-sleep-window` | 14 commits: interpreter, no-JIT, iOS/AUv3, fixes, scheduling, latency |
| dsp56300 | `ios-asmjit-bump` | submodule pointer for the asmjit fix |
| asmjit | `ios-oscachecontrol-fix` | 4-line iOS compile fix (fork: charlesvestal/asmjit) |

Result: 0.37x -> 0.70x interpreter, 1.22x -> 2.07x pipelined, ~190 ms -> ~18 ms
latency, two AUv3 instances at 98-99% clean. Output bit-exact throughout
(sha256 of the raw ASIC3 tap: 6cff97377c657a23...).

Tag `ios-working-2026-09-06` marks the working configuration.

## 1. Don't lose it

- [x] Push all four branches to their remotes. asmjit had no fork and its only
      remote was upstream, so the one commit the whole iOS build depends on
      existed in exactly one working copy; `dsp56300/.gitmodules` now points at
      the fork, without which the recorded submodule commit cannot be fetched
      and the tree does not clone.
- [ ] Regenerate `pgo/je8086.profdata` whenever `esp.hpp`'s step loop changes;
      a stale profile degrades quietly. Better: make it a build step.
- [ ] Fold `docs/IOS_AUV3.md` findings into CLAUDE.md, or link it from there.

## 2. Contributing upstream

Split into three tiers; the first stands alone without anyone caring about iOS.

**Bug fixes, platform-neutral** (send individually, each defensible alone):

| fix | size | note |
|---|---|---|
| `setDspThreads()` clamps against a placeholder pre-device | 6 lines | value silently discarded |
| asmjit `OSCacheControl.h` on all Apple targets | 4 lines | asmjit repo, not gearmulator |
| `GL_R` -> `GL_RED` | 1 line | not a valid format token anywhere |
| uc-write ring: no space check; full ring reads as empty | ~25 lines | drops patch programming |
| `UcWrite.sample` uint32 wraps in 13.5 h | ~5 lines | |
| two blocking waits reachable from the audio thread | ~85 lines | can freeze the host's graph; carries a policy choice |

**Neutral improvement:** ESP interpreter (131 lines, bit-exact, +35%). Value to
upstream is contingent -- they JIT everywhere, so this is dead code unless a
non-JIT target interests them.

**iOS support** (~420 lines): a feature with ongoing maintenance cost. ASK
before building the PR; it may be declined on scope rather than merit.

Caveat for all of it: upstream has no JP-8000 audio regression corpus. The
bit-exactness harness used here is the only evidence of that kind and lives
only in this repo.

## 3. Other gearmulator synths on iOS

**The enablement is generic.** Of the ~420 lines of iOS support, almost none is
JP-8000 specific: AUv3/Standalone formats in `juce.cmake`, the Lua `LUA_USE_IOS`
fix, `JUCE_MAC` under `if(APPLE)`, the software-renderer path for iOS (no
desktop GL, no AppKit Metal), the cross-compile changelog skip, and the signing
and ROM-embedding in `scripts/build_ios.sh`. Any gearmulator synth inherits it.

**And the DSP56300 synths already have a non-JIT path**: `dsp56kEmu` has
`DSP::execInterpreter()` with its own unit tests (`interpreterunittests.cpp`)
beside the JIT ones. So Osirus/OsTIrus, Vavra, Xenia and NodalRed2x are not
architecturally blocked on iOS. Only speed is unknown.

**Measured.** `virusBench` (new, in the gearmulator tree) renders through
`virusLib::Device` with the tree built twice. M1, Virus C, before any PGO:

| voices | JIT | interpreter |
|---|---|---|
| idle | 7.57x | 1.11x |
| 1 | 6.66x | 0.92x |
| 2 | 6.59x | 0.88x |
| 4 | 6.31x | 0.81x |

**The interpreter costs 6.5-8x the JIT, not the ~20x the ESP pays**, and it is
near real time on an M1 at light load -- so an iPad P-core has room. That is the
encouraging half.

**The discouraging half is that there is nothing to parallelise.** JE-8086
reaches real time on iOS because four ESP ASICs are independent and go on four
threads; a Virus is ONE DSP56300 executing one serial instruction stream, so a
0.81x engine stays 0.81x however many cores are free. Voice-splitting across
DSP instances is not the same emulator any more.

- [ ] Establish what the polyphonic cost actually is before promising anything.
      Above four voices the number stops being reproducible: 8 voices read
      0.19x, 0.33x and 0.62x in three sittings while 0 and 4 voices repeat to
      within 2%. The DSP runs on its own thread that `Device::process` waits on,
      so wall time may have stopped measuring emulation throughput -- rule that
      out (instrument the thread's own MIPS counter) before treating the spread
      as real emulation cost.
**Underclocking is the lever, and it already ships.** `canModifyDspClock()` is
true for the Virus and both Waldorfs, wired to the DSP/Audio settings page, and
cost is near-linear in emulated cycles. Interpreter, 4 held voices:

| DSP clock | 100% | 80% | 60% | 50% | 40% |
|---|---|---|---|---|---|
| real-time | 0.82x | 0.95x | 1.13x | 1.25x | 1.35x |

The audio is IDENTICAL at every one of those -- at four voices the firmware fits
inside 40% of its budget, so the clock is free headroom until voices need the
cycles. Half clock still holds six voices untouched on this preset and starts
dropping them at eight. A reduced-polyphony Virus is a real instrument, so this
is a shipping configuration, not a compromise measurement.

**The other DSP56300 synths, structurally** (no ROMs on hand, so nothing here is
measured):

| synth | DSPs | clock control |
|---|---|---|
| Osirus / OsTIrus | 1 (ABC), 2 (TI) | yes |
| Vavra (microQ) | 1 | yes |
| Xenia (MW II/XT) | **3** with `XT_VOICE_EXPANSION`, on by default | yes |
| NodalRed2x | 2 | **no** -- `canModifyDspClock()` not overridden |

Xenia and NodalRed2x are the interesting ones: multiple DSPs are independent and
threadable, which is exactly the arrangement that got JE-8086 to real time, and
is precisely what a single-DSP Virus cannot do. Xenia's three DSPs are three
times the work but also three threads. Note Xenia is the ESSI user, so the
release-build logging fix above is not incidental for it.

**ROMs, and where they come from.** dbwbp.com/synthbin has the Microwave II
EPROMs (two 128k halves; `xt::RomLoader` interleaves them into one 256k image)
and Waldorf Micro Q (`microQ223.BIN`). Two snags worth recording:

- The **microQ dump is byte-swapped within each 16-bit word** relative to what
  `mqLib` expects -- it begins `2e 32 33 32` where `ROM::verifyRom()` looks for
  the ASCII `2.23`, i.e. `32 2e 32 33`. Swap pairwise and it boots. Nothing says
  so; the loader just reports no ROM found.
- **Nord Lead 2 v1.04 is not Nord Lead 2x firmware** and `n2xLib` rejects it.
  The 2x ROM is not on that page, so NodalRed2x stays unmeasured.

Measured core clocks, which is the thing that could not be predicted from the
tree: **Virus C 136 MHz, microQ 118.5 MHz, MW II/XT 81.9 MHz** (times three
DSPs -- voice expansion is on by default).

**All three, M1, best-of-4, every row verified AUDIO:**

| synth | DSPs | clock | JIT 4v | interp 1v | interp 4v | interp 8v |
|---|---|---|---|---|---|---|
| Virus C | 1 | 136 MHz | 6.23x | 0.92x | **0.81x** | 0.69x |
| microQ | 1 | 118.5 MHz | 9.14x | 0.63x | **0.40x** | 0.37x |
| MW II/XT | 3 | 81.9 MHz | 9.62x | 0.25x | **0.64x** | 0.37x |

**The Virus is the best interpreted candidate, not the worst**, which inverts
the ordering the JIT column gives -- microQ and XT are the CHEAPER two under the
JIT and the more expensive two interpreted. Cost is not simply proportional to
emulated MHz (the microQ clocks lower than the Virus and interprets at half its
speed), so a synth's iOS viability cannot be predicted from its clock either;
it has to be measured per synth.

The XT column is not self-consistent -- one voice reads slower than four -- so
its shape is not yet trustworthy. Retake it on an idle machine.

An earlier version of this table reported the microQ at 1.28x and called it the
best candidate. That was the note-decay artifact described in the bench's own
history: a 3.5x error in the flattering direction, on the one measurement the
whole iOS question rests on.
- [ ] Give NodalRed2x the clock control the others have, if it is to be a
      candidate.
- [ ] Apply PGO to the interpreter before judging it. It was worth +45% here and
      the shape (big switch, hot loop) is the same. At 4 voices that is roughly
      the difference between 0.81x and playable.

Two things found on the way, both fixed and pushed: `essi.cpp` logs every
peripheral register access in RELEASE builds (`#if 1`), which an idle Virus
turns into 4.5 GB in 40 seconds; and `DeviceCreateParams::customData` carries
the device model but defaults to 0 = Virus A, under which a Virus C ROM boots
at 36 MHz into a DEBUG instruction and `process()` never returns.
- [ ] Expect to re-derive the pipeline/scheduling work per synth: the sleep
      backoff and pipeline window are tuning, not architecture.

## 4. Speed for the Move modules

**PGO is the transferable win, and it is not blocked by their using the JIT.**
Move's JP-8000 profile is 64% JIT-generated code and **32.5% compiled binary**
(H8S interpreter, `step()`, `runForCycles`) -- PGO cannot touch the former but
covers the latter. +45% on our interpreter suggests a mid-single-digit to ~8%
overall win on Move, from a build flag, bit-exact.

- [ ] PGO the Move JP-8000 build; verify with `tools/ab/bitexact.sh`.
- [ ] Same for schwung-virus, whose ratio is probably similar.
- [ ] Audit Move's own fork rings (`je_fork_shm.h`, "same but embedded in shm")
      for the two defects found in `jePipeline`: a producer with no space check,
      and blocking waits reachable from the audio path. Both are silent until
      they fire.

**Are any of these adaptable to Schwung on the Pi?** The interpreter itself is
not -- the Pi allows JIT, so Move already runs the fast path and interpreting
would be a ~7x regression for nothing. Three other things do transfer:

- **The DSP clock control is the real one, and it is not iOS-specific.**
  `setDspClockPercent()` already ships on the Virus, and on Move it is a direct
  CPU-for-polyphony trade available to schwung-virus today. On the Virus at four
  voices it was bit-identical audio down to 40% clock; half clock still held six
  voices untouched. Nothing about that argument depends on the platform.
- **PGO**, as below: 32.5% of Move's JP-8000 time is in compiled binary code.
- **New module candidates, and this now has A72 evidence.** On the JIT path --
  which is what Move uses -- the microQ and the Microwave XT both measure
  CHEAPER than the Virus already shipped there. Pi 4B @ 1.8 GHz, 62 C, full
  clock, best-of-3, every row AUDIO:

  | synth | DSPs | clock | JIT 1v | JIT 4v | JIT 8v |
  |---|---|---|---|---|---|
  | Virus C | 1 | 136 MHz | 1.45x | **1.38x** | 1.17x |
  | microQ | 1 | 118.5 MHz | 2.32x | **1.88x** | 1.49x |
  | MW II/XT | 3 | 81.9 MHz | 1.82x | **1.80x** | 1.80x |

  Scaled to Move's 1.5 GHz CM4: Virus ~1.15x, microQ ~1.57x, XT ~1.50x. That
  says the Virus is TIGHT on Move, which matches how it behaves, and that both
  Waldorfs would have materially more headroom.

  The XT is flat across 1/4/8 voices -- its cost is three DSPs' fixed cycle
  budget and barely moves with polyphony, which is a nicer profile for a live
  instrument than the Virus degrading as you play more notes. Check how many
  cores it actually occupies before believing it is free: 1.80x on a 4-core Pi
  may be spending more than one of them.

Not transferable: the interpreter, and the Apple scheduling work. Linux SCHED_FIFO behaves nothing
like QoS/DVFS, and CLAUDE.md already covers that ground.
