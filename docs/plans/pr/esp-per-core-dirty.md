# PR: ESP per-core dirty tracking

**Branch** `pr/esp-per-core-dirty` in `libs/gearmulator`, commit `77f021a5`,
forked from `upstream-gearmulator/main` (`c5dddcd0`).
**Size** 25 insertions / 12 deletions across 2 files.
**State** measured, bit-exact. NOT opened.

## The change

A patch load is ~60 program write bursts per ASIC and every one recompiled BOTH
ESP cores. A core's generated code depends only on its own program words -- core
0 owns intmem words `0..0x3ff`, core 1 the rest, and the ERAM decode reads core
1's -- so `setProgramDirty` carries a core mask and `genProgram` compiles only
what changed.

Both write paths supply the mask. `0x54` writes one word, so it names one core.
`0x56` writes `pmem[addr] .. pmem[addr + 4]`, so a burst starting below the
`0x400` boundary and ending at or above it names both.

## Measured: half the compiles, half the compile time, same audio

`patch_sweep` -- eight factory patch changes across a 10 s render -- is the right
harness, and `jeTestConsole`'s demo is NOT. The demo does change patches (the LCD
advances tracks), but only a handful of times in 300 s, so the effect on total
throughput is milliseconds in five minutes: below the noise floor that made two
identical runs of the same binary differ by 1.2%. **Count the compiles instead
of timing the render.**

Measured by toggling `m_dirtyCores |= cores` to `m_dirtyCores = 3` -- a one-line
revert to the pre-change behaviour, so nothing but the mask differs.

**Pi 4B @ 1.8 GHz, idle (load 0.08), 46.7 -> 52.5 C, counterbalanced
off,on,off,on:**

| | compiles | total compile time | per compile |
|---|----------|--------------------|-------------|
| without | 1542 | 167.6 / 169.1 ms | 0.109 / 0.110 ms |
| with    |  775 |  83.4 /  83.3 ms | 0.108 ms |

**767 of 1542 core compiles avoided, 49.7%**, and 85 ms of the 168 ms.

**The per-compile cost is unchanged (0.108 ms both ways.)** That is the isolation
that matters: this change removes compiles, it does not make them cheaper, so it
does not overlap with the `a64::Assembler` change (PR 7) which does the opposite.

On the M1 the same script gives 1524 -> 766 compiles and 30.7 -> 18.1 ms, i.e.
the same ~50%.

**The recompile EVENTS are identical either way** -- 762 `genProgram` calls in
both builds -- so the change alters only how much each one does, never when.

**The mask distribution validates the premise directly.** Of 762 bursts, 470
touched core 0 alone, 288 touched core 1 alone, and **4** straddled the `0x400`
boundary and needed both. The design assumption that a burst almost always hits
one core is not an argument here, it is a measurement.

## Bit-exact

`patch_sweep` renders identically with and without: same size, same SHA-256, on
both machines (Pi `3e2fca8c...`, M1 `af8261f5...`).

## Pitch it as a stall fix, not a speedup

167.6 ms of compile inside a 10 s render is 1.7% of wall clock, and it is
CONCENTRATED at the eight patch changes rather than spread out. Halving it halves
the pause when a player changes patch; it does not make sustained playback
faster, and claiming otherwise would not survive a throughput measurement.

## How it was built

Split from `787c8dfe` (`esp.hpp`, `esp_opt.hpp`), dropping everything in that
commit that is ours: the `JE_GENLOG` timing probes, `g_esp_genprogram_count` /
`g_esp_dirty_count`, `m_sampleClock` / `m_firstDirty` / `m_dirtyWrites`, and the
`#if JIT_X64` guard around `finalize()` -- that last one belongs to PR 7.

**The measurement used our own tree, not the PR branch.** `patch_sweep` runs
through `jp8000_render`, which needs our snapshot support and cannot build on an
upstream base. The change measured is the same change, isolated by the one-line
toggle. Say so in the PR rather than implying the branch itself was benchmarked.

## Trap: build-fix does NOT build our submodule

`build-fix/CMakeCache.txt` carries
`GEARMULATOR_SOURCE=/private/tmp/.../scratchpad/gm/source` -- another session's
copy of gearmulator. Editing `libs/gearmulator/...` and rebuilding there changes
nothing, and ninja says "no work to do" while producing a byte-identical binary.
Use `build-ref` (no override, defaults to `libs/gearmulator/source`) or check the
cache first. Confirm with:

    ninja -C <dir> -t deps | grep esp_opt.hpp

**Always verify the two A/B binaries differ before trusting the run.** Two
identical hashes caught this one.
