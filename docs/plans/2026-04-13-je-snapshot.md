# JE-8086 Boot Snapshot Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate the 30-second boot freeze by pre-computing a snapshot of the booted JE-8086 state and loading it instantly in the plugin.

**Architecture:** A standalone `je_snapshot` tool boots the Je8086 offline (6M steps), then writes all mutable state to a binary `.snap` file (~19MB). The plugin reads this file via mmap on startup, copies state into a fresh Je8086 object, and skips the boot loop entirely. Boot goes from 30s to <100ms.

**Tech Stack:** C++ fwrite/fread for snapshot generation, mmap for fast loading in plugin.

---

## State to serialize (~19MB total)

| Component | Type | Size | Serialize method |
|-----------|------|------|-----------------|
| h8state::memory | uint8[16M] | 16 MB | fwrite blob |
| h8state::regs | h8reg[8] | 64 B | fwrite blob |
| h8state::pc | offset into memory | 4 B | pcoff(pc) |
| h8state::ccr, exr | uint8 | 2 B | fwrite |
| h8state::cycles | uint64 | 8 B | fwrite |
| h8state::pending_irqs | uint64 | 8 B | fwrite |
| ESP<17> asic0 (intmem+shared+cores) | mixed | ~530 KB | fwrite blobs |
| ESP<0> asic1, asic2 | mixed | ~17 KB each | fwrite blobs |
| ESP<19> asic3 | mixed | ~2.1 MB | fwrite blobs |
| MultiAsic lastCycles/residual | uint64×2 | 16 B | fwrite |
| Timers | struct | ~150 B | fwrite blob |
| Faders | int[64]+regs | ~260 B | fwrite blob |
| Port | int8[32]×2+regs | ~70 B | fwrite blob |
| LCD | char[40]+regs | ~130 B | fwrite blob |
| HWRegs | int8[256] | 256 B | fwrite blob |
| Serial | regs only | ~30 B | fwrite blob |

**Skip (not needed):**
- h8state::maps[] — reconstructed via memmap() calls
- ESPOptimizer — regenerated on first genProgramIfDirty()
- Callbacks — re-bound after restore
- MIDI queues/vectors — empty after boot
- SampleBuffer — empty after boot

## Not serialized (reconstructed):

- `h8state::maps[]` — rebuild by calling same `emu.memmap(...)` sequence as constructor
- `h8state::pc` — restore from saved offset: `emu.pc = emu.makepc(saved_offset)`
- `ESPCore::pram` — points into ESP::intmem, restore by calling `core.setup()`
- `ESPCore::shared` — points to ESP::shared, restore by calling `core.setup()`
- `postSample` callback — re-bind after restore
- `ESPOptimizer` — mark dirty, regenerated on first use

---

### Task 1: Add snapshot save/restore methods to Je8086

**Files:**
- Modify: `libs/gearmulator/source/ronaldo/je8086/jeLib/je8086.h`
- Modify: `libs/gearmulator/source/ronaldo/je8086/jeLib/je8086.cpp`
- Modify: `libs/gearmulator/source/ronaldo/esp/esp.hpp` (add save/restore to ESP)
- Modify: `libs/gearmulator/source/ronaldo/h8s/h8sdevices.hpp` (add save/restore to Timers, Serial)
- Modify: `libs/gearmulator/source/ronaldo/je8086/jeLib/je8086devices.h` (add save/restore to MultiAsic, Port, Faders)

Add `bool saveSnapshot(FILE* f)` and `bool loadSnapshot(FILE* f)` to Je8086 that serialize/deserialize all mutable state. Each sub-component (ESP, Timers, etc.) gets its own save/load pair.

### Task 2: Create je_snapshot tool

**Files:**
- Create: `src/tools/je_snapshot.cpp`
- Modify: `CMakeLists.txt` (add je_snapshot target)

Standalone binary that:
1. Loads ROM
2. Creates Je8086
3. Boots (6M steps until PERFORM)
4. Calls `je.saveSnapshot(file)`
5. Writes to `roms/boot.snap`

### Task 3: Plugin loads snapshot instead of booting

**Files:**
- Modify: `src/dsp/jp8000_plugin.cpp`

In `child_main()`, replace the 6M-step boot loop with:
1. Check for `roms/boot.snap`
2. If exists: create Je8086, call `je.loadSnapshot(file)` — instant boot
3. If not: fall back to step-by-step boot (first run)

### Task 4: Generate snapshot on Move, verify plugin loads instantly

Run `je_snapshot` on Move to generate `boot.snap`, then load the plugin and verify <1s boot time.
