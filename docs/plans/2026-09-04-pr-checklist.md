# Upstream PR checklist

Companion to `2026-09-03-upstreaming.md`, which holds the measurements and the
reasoning. THIS file is the working checklist: one section per PR, what to
split, what to claim, and **what must be measured before it is opened**.

Written 2026-09-04, before a detour to cut a Move release. Assume nothing about
the working tree when picking this up: check `git -C libs/gearmulator log` and
rebuild before trusting any state described here.

## Ground rules

**Every PR gets independently verified with its own measurement.** Not "we
measured the bundle in September" -- each PR is a separate claim to a separate
reviewer, and a bundle number cannot support an individual one. Where a PR
claims no behaviour change, the evidence is a HASH, not a listen.

**The submodule's own conventions apply, and differ from this repo's:**
`libs/gearmulator/CLAUDE.md` says do not commit without explicit approval, and
**do not add `Co-authored-by` trailers**. Tabs, `m_` members, `_` params, braces
on their own line, 120 columns.

**Branch per PR, forked from `upstream-gearmulator/main`**, not from our
`rebase-onto-upstream`. Cherry-pick or re-apply; never open a PR carrying our
other 24 commits.

**Measurement method, learned the hard way (see the traps section of the
companion doc):**
- Benchmark on an IDLE machine. Check `/proc/loadavg` first. Several numbers
  had to be retracted for this.
- Check `vcgencmd get_throttled` before AND after on a Pi.
- Time renders WITHOUT boot: a `jp8000_render` run carries ~1.0-1.4 s of
  snapshot load and JIT warm-up. Render 1 s and 9 s and take the slope.
- `ps` %CPU is a LIFETIME AVERAGE and folds boot in forever. Use
  `/proc/<tid>/stat` deltas or `top -H`.
- A72 hardware, not a dev Mac: Apple Silicon shows 6.92x vs 6.93x for a change
  worth 1.85x on A72.

## The PRs

### PR 1 -- `setFlushDenormalsToZero()` is a no-op outside MSVC
**Split from `04098db8`** (`move: FTZ on aarch64, release codegen flags,
non-spinning DSP boot wait`), which touches three files. **Take only
`source/framework/baseLib/os.cpp`.** Leave `source/cmake/base.cmake` (global
`-funroll-loops -fomit-frame-pointer` on top of `-Ofast`, unmeasured) and
`source/axel/virusLib/dspSingle.cpp` (a Move RT workaround that slows Virus boot
for everyone).

Claim: `HAVE_SSE` is never defined project-wide -- only locally inside
`rmlRendererJuce.cpp` -- so denormal flush was never enabled on any Linux or
macOS build, x86-64 or ARM, for any device, Virus included. Key the SSE path off
`__SSE__`, add the aarch64 FPCR FZ path.

**Verify before opening:** show the preprocessor never sees `HAVE_SSE` in a
normal build (`grep -rn "HAVE_SSE" source/` plus the one local definition), and
demonstrate the FPCR bit is actually set after the change on aarch64 -- read
FPCR back in a test, do not just assert it. A denormal-heavy microbenchmark
before/after would be stronger still.

Lead with this one: a real bug in their code, no JP-8000 knowledge needed,
unrelated to everything else.

### PR 2 -- remove the MIDI-out `printf`
**`574e6c10` ships as-is**, 5 insertions 2 deletions in
`source/cpu/h8s/h8sdevices.hpp`.

Claim: a `write()` syscall per MIDI byte from the emulation thread, in two
places.

**Verify:** count the syscalls. `strace -c -e trace=write` on a run that emits
MIDI, before and after. That turns "this is obviously bad" into a number.

### PR 3 -- `PerformanceControlChannel` is set with an out-of-range value
Upstream's own bug, in `jeController.cpp`, from `902d06adc` (dsp56300, "merge
JE8086") -- verified an ancestor of `dsp56300/gearmulator main`. NOT ours, so
there is nothing to split; write it fresh.

    case jeLib::SystemParameter::PerformanceControlChannel:
        sendChange(0x11);   // off   <- 17, but this parameter takes 0..16

`jemiditypes.h` documents it "1 - 16, Off" (17 values, off = 16), while
`RemoteControlChannel` next door is "1 - 16, All, Off" (18 values, off = 17).
The firmware silently rejects the out-of-range write, so the channel stays on
its factory default of 16 and keeps listening.

**Verify:** demonstrate the rejection. Write 0x11, dump the system area back,
show the value is unchanged; write 0x10, dump, show it took. `JP_SYSEX_LOG=1`
prints what we hand the firmware. A parameter read-back alone proves NOTHING --
`param_write_block` writes our image first, so a get returns what we just
stored whether or not the emulator accepted it.

**Frame as a question, not an assertion.** The parameter has no entry in
`parameterDescriptions_je.json`, so our only range evidence is the header
comment plus our own probing, and the firmware knowledge is theirs.

Worth mentioning in the same PR or a sibling: `RemoteControlChannel` is set to 0
("channel 1") for a new savestate while its own comment says "Set remote channel
to 3" and the JSON default is 2. Channel 1 collides with the Upper part, which
also defaults to MIDI CH 1, so channel 1 is consumed by the remote/keyboard path
-- it goes through KEY MODE (SPLIT) and the arpeggiator instead of addressing
Upper directly. Observed live on hardware with a Minilab3.

### PR 4 -- H8S page bitmap
**Split from `787c8dfe`**, `source/cpu/h8s/h8s.hpp` only, and only the bitmap
hunks. Every instruction fetch previously probed a 16 M-entry pointer table; an
8 KB bitmap answers first.

**Verify:** bit-exactness via `tools/ab/bitexact.sh` (SHA-256 per script, with
the UNSTABLE self-check), plus a per-change A72 timing. Do NOT quote the bundled
1.85x -- measure THIS change alone.

### PR 5 -- H8S wide-access fast path
**Split from `787c8dfe`**, same file, separate PR so each is judged alone.
16/32-bit accesses that lie in one unmapped page read `memory[]` directly.

Put this in the commit message, it is what earns the merge: **the per-byte
`clockMem()` calls are kept verbatim, so cycle counts -- and therefore the ASIC
sample cadence -- are unchanged.**

**Verify:** bit-exact hashes, and its own timing figure.

### PR 6 -- timer event horizon  [DONE: branched + measured]

Branch `pr/h8s-timer-event-horizon`, commit `5f03f84b`, forked from
`upstream-gearmulator/main`. 61 insertions across 2 files (vs `787c8dfe`'s 218).

**MEASURED 2026-09-04 and it is +6%, NOT "the largest single engine win"** --
that claim was carried in these docs unverified and is now retracted. Upstream's
own `jeTestConsole` on an idle Pi 4B, counterbalanced B,A,A,B:

| position | build | speed | wav bytes |
|----------|-------|-------|-----------|
| 1st | before | 43% | 68,135,468 |
| 2nd | after  | 45% | 71,887,916 |
| 3rd | after  | 45% | 71,518,508 |
| 4th | before | 42% | 67,905,068 |

`after` wins in both middle positions; `before` reads 43% first and 42% last,
consistent with the 50.6 -> 58.4 C drift. The wav sizes corroborate from an
independent measure: 5.4% more audio rendered in the same wall clock.

The 43% baseline independently reproduces the "stock upstream, serial 0.433x" in
the companion doc, measured with a different harness -- good cross-check.

**Bit-exact, verified properly.** Each run is killed by a wall-clock timeout, so
the faster build writes MORE audio and the SHA-256s differ for a reason that has
nothing to do with correctness. Comparing the shorter file as a PREFIX of the
longer (`cmp -n <size> -i 44:44`) shows all 68,135,424 audio bytes identical.
Do not report raw hashes from timeout-truncated runs.

**Consequence: the flagship PR is probably NOT this one.** If the timer horizon
is 6% and the whole engine bundle is 0.433 -> 0.800 (+85%), something else does
nearly all the work -- likely the dense ARM64 emitter or ESP dirty tracking.
Measure each before claiming; do not guess which.

Correctness argument, already verified once: `timers.tick()` runs AFTER
`emu.step()`, so the old code updated to end-of-instruction cycles, and
`catchUp()` uses `instrStartCycles`, which is exactly that point. Deferred
increments equal per-instruction ones because `inc` is a difference of floors,
additive across any split.

Pre-empt two questions: the skip conditions in `update()` and in the `nextEvent`
recompute must stay in lockstep, and `write()` resets `nextEvent`.

**Verify:** bit-exact hashes, its own timing, and ideally a test that a timer
IRQ lands on the identical cycle before and after.

### PR 7 -- `a64::Assembler` instead of `a64::Builder`
**Split from `787c8dfe`**: `esp/esp_jit_types.h`, `esp/esp_jit_arm64.h`. Six
lines. The ESP program is straight-line with no labels or node edits, so it is a
drop-in with identical encodings; `Builder::finalize()` was re-walking every
node.

Clean up first: `m_asm.finalize()` is guarded by `#if JIT_X64`, which keys off
the architecture rather than the builder type.

**Verify:** dump the generated code before and after and show the encodings are
byte-identical -- that is a much stronger claim than "sounds the same" and it is
available here. Then a compile-time measurement (`genProgram` calls, patch
change burst).

### PR 8 -- ESP per-core dirty tracking
**Split from `787c8dfe`**: `esp/esp_opt.hpp`, `esp/esp.hpp`,
`jeLib/je8086devices.h`. The change is ~15 lines: a `cores` mask threaded
through `setProgramDirty`/`genProgram` and the two `writeuC` call sites, where
core 0 owns `intmem` words `0..0x3ff` and core 1 the rest. **Everything else in
that hunk is instrumentation and stays here.** Comment the 0x56 path's
straddling mask.

**Verify with `patch_sweep`, not the note-only scripts.** The note scripts send
`pc 0` and nothing else, so they compile 15 ESP programs at boot and none after
-- they say NOTHING about dirty tracking, whose whole purpose is the recompile
burst at a patch change. `patch_sweep` walks eight factory patches (109 program
compiles, 762 `genProgram` calls). Report compiles avoided, and timing.

### PR 9 -- dense ARM64 emitter + ring mirror
**`bf974365`**, minus the fork-parallel ASIC split. `esp/esp_jit_arm64.cpp`,
`esp/esp_opt.hpp`, `esp/esp.hpp`. The only change that alters generated-code
semantics; expect the most review.

Lead with `coefsShifted`, which is easy to defend: `shiftAmount` is one of
{3,5,6,7}, `coef` is `int8`, so `coef << 4 <= 2032` fits `int16`, and
`(P << (7-s)) >> 7 == P >> s` exactly under arithmetic shift.

Settle before submitting: `nextIsDmac` uses the next statically emitted op,
consistent with the existing design (`esp_opt.hpp` already declares jumps
unsupported -- offer our runtime check for that, it was an `assert` compiled out
under `-Ofast`); `jitEnter` zeroes `last_mulInput{A,B}_24` where the interpreter
carries them, not a regression and measured unreachable across the corpus
(`ec8a4344` scoped that measurement to the real corpus); keep `ESP_IRAM_MIRROR`
unconditional, upstream will not want two ring layouts by architecture. The x64
emitter takes `nextIsDmac` and ignores it -- say so.

**MEASURED 2026-09-04 and it is the flagship: +65% on A72, +45% on M1, bit-exact
over 378.7 s of audio including a program change.** Branch
`pr/esp-dense-arm64-emitter`, commit `52735ea2`, 7 files. Full write-up in
`pr/esp-dense-arm64-emitter.md`. The worry that it might cost a beefy machine
something was tested directly and does not materialise -- the M1 gains 45%.

One decision is still open and belongs to the maintainer: `ESP_IRAM_MIRROR` is
aarch64-only on the branch, which this checklist elsewhere argues against. Lead
the PR description with it.

**The split is emitter-ONLY.** `bf974365` bundled the dense emitter with the
generalised ASIC split for fork-parallel; `je8086devices.h` is untouched on the
branch and the diff contains no thread, fork or pipeline code at all. Both
measured binaries are single-threaded, so this is a SERIAL engine win and has
nothing to do with PR 10.

### PR 10 -- the parallel ASIC pipeline
**An ISSUE FIRST, not a patch.** This adds threads to a device library; the
answer may be "yes, but through `clap.thread-pool`" or "yes, but block-level".
Pitch it as what it is: the difference between 0.8x and 2.26x on an SBC, off by
default, nothing changed for anyone who does not ask.

Commits: `a3e5c7fe`, `b0466c80`, `8ea696d4`, `e732641b`, `6b8a54ad`, `94bbbff4`,
`345786b7`, `9843dd92`. **`8c2d4eff` (env opt-in) and `4db85347` (RTPRIO env) are
SUPERSEDED** by the settings and schedule-inheritance commits -- do not carry
them, and delete the `else if (getenv("JE_PIPELINE"))` fallback in
`jeLib/device.cpp` and the `JE_PIPELINE_RTPRIO` override in `jePipeline.cpp`.
Those keep our Move build and benches alive and are the exact env interface this
PR argues against.

Also drop core pinning entirely: measured worthless (3 stages pinned vs unpinned
1.8/1.8 vs 1.7/1.8; 2 stages pinned was WORSE once, 1.5x) and it is what breaks
multiple instances, since `core(s)` indexes by stage and the environment is
process-wide.

Evidence we already have, all reproducible:
- Bit-exact against serial with a fixed delay; the raw ASIC3 tap is
  byte-identical and identical across runs.
- REAPER 7.39 on a Pi hosts it as VST3 and CLAP; both render the same chord to
  MIDI 60.02 at 48 kHz.
- Live from a MIDI controller, no underruns, one instance at 3 stages.
- Two instances at 2 stages each: 302% of 400%, REAPER steady at 75%.
- Stages inherit the host's schedule: SCHED_RR 68 under REAPER's mediaafx 69;
  SCHED_OTHER under clap-trap despite rtprio 95 being available.

**Disclose:** the pipeline delay is 2 samples and IS reported through
`getInternalLatencyMidiToOutput()` / `getInternalLatencyInputToOutput()`; a
host's per-plugin CPU readout understates the plugin badly (REAPER shows 3.3%
against an actual ~1.4 cores) because the work happens off the audio thread --
say it before a reviewer does, it is a fair objection to the design.

### Not upstreaming
The fork pipeline and its Move machinery, `base.cmake` codegen flags,
`dspSingle.cpp` sleep, snapshot v2 (propose before writing), and the test corpus
as-is (it drives `jp8000_render`, which depends on our snapshot support).
Porting a minimal renderer onto `jeTestConsole` would change the last one and
would be a genuine contribution -- **upstream has no JP-8000 audio regression
corpus at all**, no `add_test` and no ctest anywhere for je8086.
