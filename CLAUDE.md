# schwung-jp8000

Roland JP-8000 emulator for Schwung/Move, based on gearmulator's JE-8086 engine.

## Architecture

Uses **fork-parallel processing** to approach real-time on Move's A72:
- Parent process: H8S microcontroller + ASIC0+1 on core 2
- Child process: ASIC2+3 direct loop on core 3 (no H8S needed)
- GRAM handoff: 6×int32_t per sample via SPSC ring buffer
- Audio output: 2×int32_t per sample via SPSC ring buffer

Internal sample rate: 88,200 Hz → resampled to 44,100 Hz output.

## Performance

RT factor = internal 88.2 kHz samples produced per wall second / 88200.
One second of 44.1 kHz output requires one full second of 88.2 kHz engine
audio (2:1 decimation), so decimation does NOT halve the work — an earlier
"fork + resample ≈ 1.44x" claim was a doubling error.

| Config | RT Factor | Notes |
|--------|-----------|-------|
| Serial (H8S + 4 ASICs, 1 core) | 0.37x | 0.30–0.33x after ARM64 JIT revert |
| Fork parallel (2+2 ASICs, 2 cores) | 0.72x | ~0.58x after ARM64 JIT revert |

**The full emulator is below real-time on Move.** Audio works end-to-end
(boot, snapshot, MIDI, notes) but the ring underruns continuously →
dropouts. Cost is the same idle vs. playing: the ASICs always run their
full programs, and the ESP JIT does not recompile during notes (verified
with snap_bisect counters).

## Snapshots

`boot.snap` (v2 format) captures H8S memory/registers, ASIC state, AND
memory-mapped peripheral state (timers, MIDI SCI, interrupt enables,
ADC/ports). v1 snapshots restored a synth whose timers and MIDI receive
were dead — it idled fine but ignored all notes forever (the old "working"
snapshot only seemed fine because it had a test note baked in). Regenerate
with `je_snapshot <rom_dir> [out.snap]` (builds for ARM and Mac). v1 files
are rejected at load; the plugin then falls back to scratch boot (~30–60 s).

## Testing without the device UI

- `plugin_host_test <dsp.so> <module_dir> [secs] [out.wav]` — minimal
  Move v2 host: dlopens the plugin, boots it, injects a C4 note via
  on_midi, reports per-second peak/RMS and a final AUDIO/SILENT verdict.
  The plugin also builds natively on Mac, so the full snapshot+fork+MIDI
  path can be exercised locally before any deploy.
- `snap_bisect <rom_dir> snap|scratch [warmup] [out.wav]` — serial-mode
  raw-Je8086 note test with per-phase throughput + ESP JIT counters.
- On device, put all test artifacts in `/data/UserData/jp8000_test/`
  (NEVER /tmp — the root fs is ~474 MB and nearly full).

## Building

```bash
./scripts/build.sh              # Build plugin (Docker cross-compilation)
./scripts/install.sh            # Install to Move
```

### Benchmarks only

```bash
docker run --rm -v "$(pwd):/build" -w /build schwung-jp8000-builder \
  bash -c 'cmake --build build --target bench_je_fork -j$(nproc)'
```

## ROMs

Requires JP-8000 v1.5 firmware ROM files (.mid). Place all 8 files in the `roms/` directory on device:
```
/data/UserData/schwung/modules/sound_generators/jp8000/roms/
```

## Key Files

- `src/dsp/jp8000_plugin.cpp` — Main plugin (stub, WIP)
- `src/benchmark/dsp_bench.cpp` — Performance benchmarks
- `src/benchmark/je_fork_shm.h` — Shared memory structs for fork parallelism
- `libs/gearmulator/` — JE-8086 emulator (submodule)

## Dependencies

From gearmulator (via submodule):
- `ronaldo/je8086/jeLib` — JP-8000 H8S + ESP emulation
- `ronaldo/esp` — ESP ASIC JIT compiler
- `ronaldo/common` — Ronaldo shared library
- `synthLib` — Synth device abstraction
- `baseLib` — Platform utilities
- `dsp56300` — asmjit JIT backend (used by ESP JIT)
