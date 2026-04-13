# schwung-jp8000

Roland JP-8000 emulator for Schwung/Move, based on gearmulator's JE-8086 engine.

## Architecture

Uses **fork-parallel processing** to achieve real-time on Move's A72:
- Parent process: H8S microcontroller + ASIC0+1 on core 2
- Child process: ASIC2+3 direct loop on core 3 (no H8S needed)
- GRAM handoff: 6×int32_t per sample via SPSC ring buffer
- Audio output: 2×int32_t per sample via SPSC ring buffer

Internal sample rate: 88,200 Hz → resampled to 44,100 Hz output.

## Performance

| Config | RT Factor |
|--------|-----------|
| Serial (4 ASICs, 1 core) | 0.37x |
| Fork parallel (2+2 ASICs, 2 cores) | 0.72x |
| Fork + 44.1 kHz resample | ~1.44x |

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
