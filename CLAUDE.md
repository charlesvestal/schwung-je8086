# schwung-jp8000

Roland JP-8000 emulator for Schwung/Move, based on gearmulator's JE-8086 engine.

## Architecture

Uses **fork-parallel processing** to approach real-time on Move's A72:
- Three-stage fork pipeline: stage 0 (H8S + ASIC0) on **core 3**, stage 1 (ASIC1) on core 1, stage 2 (ASIC2+3) on core 2 (two DIFFERENT Move cores; stage 0 fits only on core 3 — RT budget, see the plugin header); per-stage GRAM handoff rings, audio out of the last stage
- Stages run at **FIFO 20** (`child_clamp_realtime`) — SCHED_OTHER stages underrun 4 runs in 6 in the chain host with Move idle; FIFO 20 is 0 in 6. No RR and no way back from OTHER: `ableton` has RLIMIT_RTPRIO 0
- **One pipeline per device**: an `flock` on `/data/UserData/schwung/jp8000.pipeline.lock`. Two pipelines starve everything SCHED_OTHER (Move's UI thread, sshd, mDNS) while the pads keep working — a frozen LCD, twice on 2026-09-02
- **Core 3 is not "the SPI core"**: Move pins its whole process to cores 0–2, SPI thread included; core 3 holds only the SPI DMA IRQ thread. Stages on 2/1/0 left Move's UI thread 5% of a core during a chord; stage 0 on core 3 gives it the 40% it has with the module silent (`JE_DEFAULT_CORES`)
- **A FIFO stage that polls is charged to the RT budget like work, and the budget is shared with Move.** Linux RT throttling here is 950 ms per core per second, no sharing; past it, EVERY RT task on that core is parked for the remainder — Move's FIFO 70 audio worker included. `shm_wait` used to `usleep(10)`-poll (~40k wakeups/s, ~30% of a core) and put cores 0/1 at 933/869 ms next to Move's workers: audible glitches in MOVE's output, none in ours. Now 200 yields then exponential back-off to 250 µs (the ring holds ~136 ms). Measure with `rt_time` in `/sys/kernel/debug/sched/debug` (root), not with top
- **Performances select on the performance control channel (16).** Bank 80 + PC on ch 1 picks a patch. `plugin_drive` handles this for bank 80 sweeps and chords both parts (ch 1+2); in the chain host set receive=All / forward=THRU for the select. Trancer = 80/0 program 4
- The jp8000 harness (`plugin_drive`) beside a loaded slot is refused by the lock; run it as root with `chrt -f 20` (as `ableton` it is SCHED_OTHER and underruns for that reason alone)

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
