# Where this is and what comes next

## State

Everything from the iOS session is on named branches; nothing is pushed yet.

| repo | branch | contains |
|---|---|---|
| schwung-jp8000 | (current) | build_ios.sh, docs, `pgo/` profile |
| gearmulator | `ios-sleep-window` | 14 commits: interpreter, no-JIT, iOS/AUv3, fixes, scheduling, latency |
| dsp56300 | `ios-asmjit-bump` | submodule pointer for the asmjit fix |
| asmjit | `ios-oscachecontrol-fix` | 4-line iOS compile fix |

Result: 0.37x -> 0.70x interpreter, 1.22x -> 2.07x pipelined, ~190 ms -> ~18 ms
latency, two AUv3 instances at 98-99% clean. Output bit-exact throughout
(sha256 of the raw ASIC3 tap: 6cff97377c657a23...).

Tag `ios-working-2026-09-06` marks the working configuration.

## 1. Don't lose it

- [ ] Push all four branches to their remotes (nothing is pushed).
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

- [ ] Measure the DSP56300 interpreter vs its JIT on a Mac -- one number decides
      feasibility. The JE-8086 ratio was ~20x; the Virus DSP is a busier target.
- [ ] Apply PGO to the interpreter before judging it. It was worth +45% here and
      the shape (big switch, hot loop) is the same.
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

Not transferable: the Apple scheduling work. Linux SCHED_FIFO behaves nothing
like QoS/DVFS, and CLAUDE.md already covers that ground.
