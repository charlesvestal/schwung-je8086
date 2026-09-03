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

**Measured 2026-09-03 on a Raspberry Pi 4B rev 1.5 @ 1.8 GHz** (Debian 13
arm64, PSU + wifi, `throttled=0x0` before and after, best of 3, variance below
0.01 s). Serial `jp8000_render`, i.e. ONE process and no fork -- the
configuration upstream would get:

| Script | Before opts | After opts | Speedup | RT factor |
|--------|-------------|------------|---------|-----------|
| `sustained_c4` (5 s) | 12.78 s | 7.34 s | 1.74x | 0.39x -> 0.68x |
| `patch_sweep` (10 s) | 24.31 s | 13.36 s | 1.82x | 0.41x -> 0.75x |

"Before" is the rebased tree with the three performance commits removed (branch
`ref-no-perf`); "after" is HEAD. Output is byte-identical between them, and
`sustained_c4` hashes the same on macOS/arm64 and Linux/arm64 -- the engine is
deterministic across OS and toolchain, so a reviewer can reproduce the hash.

**The serial engine is still below real-time, even at 1.8 GHz with every
optimization.** Move's CM4 runs at 1.5 GHz, ~20% slower, so serial there is
roughly 0.57-0.63x. Real-time playback on Move therefore comes from the FORK
PIPELINE spreading ASICs across cores, not from the H8S/ESP work. Keep the two
claims apart: "~1.8x faster on A72, bit-exact" is the upstream claim; "real-time
on Move" is a product claim that depends on machinery upstream is not getting.

**Definitive numbers, Pi 4B @ 1.8 GHz, measured 2026-09-03 after a reboot with
`throttled=0x0` before and after** (48.7 -> 66.7 C, well under the 80 C cap):

| Configuration | engine (`bench_je*`) | via Device (`jp8000_render`) |
|---------------|----------------------|------------------------------|
| serial | 0.85x | 0.60x |
| pipeline, 2 stages | 2.06x fork / 2.01x thread | |
| pipeline, 3 stages | 2.58x fork / 2.62x thread | |
| pipeline, 4 stages | 2.94x fork / 2.92x thread | 1.28x |

Threads match fork at every depth.

**The engine optimizations are worth 1.85x on A72, not 1.74x.** Steady-state,
boot excluded: 0.433x -> 0.800x real-time. With the four-stage pipeline on top,
through Device, 2.26x -- 5.2x from stock upstream serial in total.

**Time a render WITHOUT its boot, or the number is wrong.** A jp8000_render run
carries ~1.0-1.4 s of fixed cost (snapshot load, JIT warm-up) and timing the
whole process folds that into the rate: a 5 s render read 0.60x serial and 1.28x
pipelined, which is what first suggested a large per-sample Device overhead and
sent us looking for one. Subtracting the fixed cost by rendering 1 s and 9 s and
taking the slope gives the real figures: **serial 0.76x, pipeline 2.26x, a 2.98x
speedup** -- the same speedup the engine bench shows, so Device costs about 12%
serial and is not hiding a bottleneck.

**The audio ring is not worth batching.** `perf` on the serial render: 64% in
[JIT] (the ESP-generated ASIC code), 32.5% in the binary (H8S interpreter,
step(), runForCycles), **2.78% kernel**, 0.69% libc. Semaphores, syscalls and
I/O together cannot exceed that 2.78%. `SpscSemaphoreWithCount` is a counted
semaphore whose notify()/wait() are a single atomic unless a waiter is actually
blocked, so a ring crossing costs tens of nanoseconds, not microseconds. An
earlier note here claimed batching it was the next big win; that was inferred
from the boot-inflated numbers above and is wrong.

**In a plugin host, leave a core for the host: 3 stages beat 4.** Measured with
the JE8086 CLAP plugin in clap-trap on an idle Pi 4B, 48 kHz, 256-sample blocks
(5333 us budget), `throttled=0x0`:

| Configuration | RT factor | us/block |
|---------------|-----------|----------|
| serial (upstream today) | 0.7x | 7621 |
| pipeline, 2 stages | 1.7x | 3196 |
| **pipeline, 3 stages** | **1.8x** | **2953** |
| pipeline, 4 stages | 1.4x | 3757 |

Four stages pins every core and starves the host's own audio thread; the
standalone bench never showed this because it had no host to starve. Three
stages uses 55% of the block budget -- comfortable, not marginal.

**The Pi's built-in audio is the latency floor, not the engine.** The bcm2835
headphone driver pins the ALSA period at 884 frames (~10 ms at 88.2 kHz) --
identical through `plughw` and `hw`, and unmoved by an explicit
`snd_pcm_hw_params_set_period_size_near` request for 128. Two periods xrun
(~10 per 14 s), three run clean, so **~30 ms is the floor on the onboard DAC**.
The engine is nowhere near being the constraint: a 128-frame block renders in
0.61 ms against a 1.45 ms budget. A class-compliant USB interface takes
64-256 frame periods and is the fix for playing live.

`snd_pcm_set_params` makes this worse by hiding it: given any latency under
~20 ms it silently returns a buffer of exactly ONE period, so the card starves
whenever the writer is running -- an identical 682 xruns at 10, 6 and 4 ms,
which is what a misconfiguration looks like beside real jitter.

**Benchmark on an IDLE box.** The same plugin measured 1.1x with Ardour running
alongside and 1.8x without. Check `/proc/loadavg` first; several numbers taken
this way had to be retracted.

**Debian ships realtime audio limits DISABLED.**
`/etc/security/limits.d/audio.conf.disabled` -- until it is copied to
`audio.conf`, `ulimit -r` is 0, nothing can get SCHED_FIFO, and every tool
prints "Failed to set thread priority: Permission denied". With it enabled
(rtprio 95, memlock unlimited) the headless host went from 113 xruns to 6 at a
20 ms buffer. This was true for the whole first day of measurements here.

**The resampler is not the cost.** Normalised to CPU per second of audio, serial
is 1.44x at 88.2 kHz and 1.46x at 48 kHz -- identical, because the engine always
runs at 88.2 kHz internally whatever the host asks for.

**The pipeline is what reaches real-time, and it does so on stock hardware.**
Same Pi 4B @ 1.8 GHz, stock Debian 13, no Move-specific anything:

| Configuration | RT factor |
|---------------|-----------|
| Serial, optimized (`bench_je`) | 0.85x |
| Fork pipeline, 2 stages on 2 cores (`bench_je_fork`) | 2.01x |

Scaled to Move's 1.5 GHz CM4 that is ~1.67x, and sharing cores with Ableton's
UI process brings it to about the ~1.4x observed in play -- so the real-time
claim is a PIPELINE claim, corroborated, and nothing to do with the retracted
2026-06 figure it happens to match numerically.

The split is uneven: stage 0 (H8S + ASIC0..1) runs 5.63 us/sample against stage
1's 4.26, and stage 1 idle-waits 1.21 s of a 10 s render. Budget is 11.34
us/sample. A better-balanced split has headroom.

(`bench_je` and `jp8000_render` are different workloads -- the renderer also
resamples and writes a wav -- so their serial figures, 0.85x and 0.68-0.75x,
are not directly comparable to each other. Compare each against itself.)

**Do not benchmark this on a dev Mac.** Apple Silicon renders the optimizations
invisible -- 6.92x vs 6.93x serial, before vs after -- because a wide
out-of-order core hides the per-instruction timer loop, the pointer-table
indirection and the extra ESP loads/stores that an A72 actually pays for. The
same comparison on A72 is 1.74x. Use A72 hardware.

The pre-optimization history: serial was measured at 0.37x and fork-parallel
(2+2 ASICs, 2 cores) at 0.72x on Move in June 2026, and a "fork + resample
~1.44x" row was deleted then as a doubling error -- one second of 44.1 kHz out
needs a full second of 88.2 kHz engine audio, so decimation does not halve the
work. That retracted 1.4x nearly went into a public post; the numbers above are
the ones to quote.

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

## Loading banks

**The bank list is FLAT, and there is no folder level.** A file IS a bank here
-- you drop dumps into `banks/` and each becomes one -- so a folder step was a
screen listing directories on the way to a screen listing the files in them, and
with everything dropped in at the top it had exactly ONE row on it, titled
"Banks", above a screen titled "Bank". `bank_list` / `bank_list_count` /
`bank_list_name` is one mode-aware list over every bank. Files in sub-folders are
still found and still listed, by their own name: the folder is not in the label,
because it is not information the user asked for. What survives from the folder
scheme is `uniquify()`, now keyed on the NAME ALONE -- with the folder gone from
the label, two same-named files in different folders would otherwise draw two
identical rows.

**The browser levels carry no knobs.** They used to carry `MAIN_KNOBS`, which
made the planner emit the Main knob page in the MIDDLE of the browse sequence:
bank list, eight knobs, preset list. The pages now run in the order the task
does -- `BANK > PRESET > PATCH > sections`.

**A repeated header block is a new preset.** Not every dump advances the
destination slot: "The Usual Suspects" holds 32 distinctly named performances
and addresses every one of them to user slot 0 (AZS Eternal spans 0x00-0x3F).
Keyed by address alone they collapsed to one preset -- correct to the byte, and
useless. `classify()` splits when a program's first block arrives twice.

**`patch_count` / `performance_count` answer -1 until the table is current.**
They used to answer out of the sizes array while the child was still filling it,
so the previous bank's number was reported as this one's: a confident "1/1" over
a bank of 32. The `:N` name form already had the guard; the counts beside it did
not, so the list drew a row the name lookup then refused to fill.

**A file that parses to nothing gets a row, not silence.** 39 of the 97 files in
one real library contain no sysex at all -- ordinary MIDI songs beside the dumps
-- so the parser is right about them and the user still needs telling. The bank
list ends with `! N files ignored`; the name is the whole message, and the empty
preset list under it is the honest answer. `bank_scan` audits a corpus without
booting the emulator.

**The browser name comes from the PARSED BANK, not from the synth.**
`patch_name` / `performance_name`, bare or `:N`, is the name of a ROW -- read
out of the table the child filled from the file it parsed, with no emulator
involvement. It used to answer "what is LOADED" from the temp image, on the
reasoning that a factory performance carries its own patches so what SOUNDS is
"Chariots U" rather than whatever the list points at. True, and the wrong
question for a browser to ask: the image is an ANSWER FROM THE FIRMWARE, so the
name appeared "after a moment", and after a bank switch it kept showing the
previously loaded preset until the jog moved. Both complaints were one sentence
-- the browser was displaying the synth's state where the user was reading the
list's. `loaded_patch_name` / `loaded_performance_name` still ask the image.

**A restored state is not restored until the FIRMWARE says so.** `state_apply`
queues ~560 bytes of DT1 and writes our image optimistically; the emulator only
parses those bytes while it STEPS, and a freshly loaded slot is silent, so the
shim idles it and nothing drives the child. The image then reads as the restored
performance while the previous one keeps sounding -- and switching a SET reloads
every module, so that is the path a set switch takes. The boot thread now asks
the firmware to dump its temp back and WAITS for it before setting `ui_ready`
(SCHED_OTHER, nobody listening to the slot yet, so waiting is free; bounded at
4 s so a silent firmware still yields a usable slot). Verified byte-exact:
528/528 with the slot never rendered.

**Two probes lied about this before it was found**, both worth remembering.
`temp_refresh` forces the child to step, so using it to ask "did the restore
land" SUPPLIES the stepping whose absence is the bug -- it passed identically
with the fix disabled. And a spectral-similarity score on a chorused pad cannot
separate "right patch, different LFO phase" from "wrong patch": it read 0.86 vs
0.93 where the byte comparison read 0/528. Compare the bytes.

**Arrival is tracked apart from validity.** `temp_rx_mask` / `temp_pending`
answer "has the dump I asked for landed" without clearing `img_valid`. Clearing
it to detect that -- the obvious way -- blanks the WHOLE UI: `param_read`
returns -1 on a clear bit, so every knob on every page reads as unavailable for
as long as the dump is in flight, and `state_get` refuses too, which is a
silently skipped autosave. The image stays readable and shows the last known
values until the new ones arrive.

**Selecting a bank resets the row to 0.** The index is a position in THIS bank;
carrying it across landed at 40 in a bank of 32, an index with no row, so the
name read as -1 and the browser showed nothing. It loads nothing -- only setting
`patch`/`performance` does that.

**Loading a PATCH obeys Edit Part** (`load_part`): Upper, Lower or Both, so one
sound can be taken from a bank into one half of a split. A PERFORMANCE always
replaces both, because a performance *is* both.

## The System area

A third mode, beside Patch and Performance. It is not a page under Performance:
on the hardware you leave the performance to reach it (SHIFT/EXIT), and what you
set there outlives the patch. Thirteen keyboard parameters — the pattern/motion
sequencer settings and the rack-only tail are skipped.

**It is the one group whose writes do not carry the temp base.** Every other
parameter lives inside the temp performance at `0x01000000`; the system area is
`0x00000000` (`jeLib::AddressArea::System`, with `SystemArea::SystemParameter`
adding nothing), so `param_write_block` picks the base off `p->area`. The image
was already being filled from the firmware's DT1 replies (`img_system`,
`IMG_SYSTEM`) long before anything could address it — reads worked, writes had
nowhere to go.

**A parameter read-back proves nothing about the firmware.** `param_write_block`
writes our image first and the DT1 second, so a get returns what we just stored
whether or not the emulator accepted it. `JP_SYSEX_LOG=1` prints the bytes we
actually hand the firmware (in the child, which is not the SPI callback) — that
plus a `sysreq` in `jp8000_render` is the only way to see both halves.

**Every range here was measured, not read off a header.** The firmware silently
REJECTS an out-of-range write and keeps what it had, so an accepted write is the
only proof of a range. `PerformanceControlChannel` is the one that matters:
it accepts 0..16 (16 = Off) and rejects 17, so jeController's
`sendChange(0x11); // off` never turned it off — it left the factory default 15,
i.e. channel 16, still listening. `RemoteControlChannel` next door genuinely does
go to 17, which is what makes the two look interchangeable and neither of them
is. Upstream bug; worth a PR.

**Three entries govern hardware that cannot exist here, and are not exposed.**
Power-Up Mode decides what the keyboard loads at power-on — the child boots from
a snapshot and the slot's own state is applied over it, so the host has already
answered. Local switches the internal keyboard into the engine, and
**`KeyScanner::read()` returns 0 unconditionally with an empty `write()`** — the
key matrix is a STUB, so that keyboard reports no keys pressed, forever.
Keyboard Shift transposes the same stub (measured: a note on the remote channel
is identical at -2, 0 and +2).

**The ribbon is NOT in that category.** `Faders` is fully emulated,
`setFader(which, value)` exists, and `kFader_Ribbon1/2` sit in its table beside
pitch bend and the mod wheel — nothing calls it yet, which makes Ribbon
Relative/Hold, and the forty `ctl_*` patch depths they scale, **latent rather
than impossible**. Gate Time Ratio is the same shape, and it took two attempts to
say so honestly: measured against a **verified-running arpeggiator** (mode UP,
beat NORMAL 1/16, 11 voice starts at 0.405 s) all six of its values render
byte-identical audio. It belongs to the pattern/RPS block, not the arpeggiator.
The first attempt used a sustained pad, never established the arp was running at
all, and was reported as if it had proved something.

**RPS is selectable and silent, and that is the whole pattern story.** The 84
non-NORMAL beat patterns DO play — `arp_beat` = SEQUENCE A1 in mode UP gives an
uneven rhythm (IOI sd 0.142 s) against NORMAL 1/16's metronomic 0.405 s — but
`arp_mode` = RPS gives **zero voice starts** on every pattern family, across the
whole keyboard, with and without a running clock. RPS plays back *recorded*
phrases; `PatternSetup` (SystemArea `0x1000`) holds only 48 loop lengths, not
phrase data, and phrases are recorded with the panel REC plus the keyboard —
which is the stub again. So the seven pattern/motion system parameters have
nothing to act on, and the module offers an `arp_mode` option that produces
silence. Loading phrases over sysex is the path that would change all of this.

Being unable to observe a parameter is not the same as knowing it does nothing —
**a stub device is knowing.**

**One setup page.** Ten parameters do not fit eight cells, so the split is where
it costs least: the eight a slot can reach today stay together (channels and
tuning on the top row, TxRx and gate on the bottom) and the two ribbon settings
are the single dive. Three pages of two, three and four cells was the shape this
replaced.

**System settings are not in the temp performance, so they need their own state
field.** `state_get` writes `"sys":"<hex>"` (version 2) holding one byte per
EXPOSED system parameter in table order, and `state_apply` replays them as
individual DT1s. Not the firmware's own 23-byte dump: that starts with
`PerformanceBank` and `PerformanceNumber`, so replaying it wholesale would also
reload the preset. A blob whose `sys` length disagrees with the current parameter
count is skipped rather than decoded positionally into the wrong addresses.

**`jp8000_render` fires every scripted sysex at t=0**, whatever time the script
line names. Three separate readings of "did this write land" came out of that,
because a value written at 3.0 s appeared in a dump logged at 0.247 s. Note-level
events are scheduled; sysex is not.

**The System pages are a knob grid, not a menu.** They were built expecting to
need `LAYOUT_LIST` — set-once settings with long words — and rendering them
(`preview.mjs jp8000 --mode system --layout movy`) says otherwise: four pages,
nothing paginated, nothing wrapped, two-option settings drawn as switches. A
per-level layout pin is a host feature and is still not built; nothing here needs
it.

## Verifying a change is behaviour-preserving

`tools/ab/bitexact.sh <ref_build> <new_build> <rom_dir> tests/scripts` renders
every script on both builds and compares SHA-256. The perceptual score in
`compare_wavs.py` is the wrong instrument for an optimization claiming to change
nothing: that claim is bit-exactness, so compare hashes.

**A script is only evidence if it is reproducible.** The harness renders each
script several times per build and reports one whose own output varies as
UNSTABLE, excluding it -- otherwise the harness's own jitter gets read as a
regression from the change under test. That happened: `performance_select` was
reported as DIFFERS before the self-check existed.

**A parallel pipeline is exact and reproducible only if its delay is FIXED.**
Bounding how far the H8S may lead the stages is not enough: how far it actually
gets still depends on thread timing, so each run covers a different span of
emulated time. Three runs of the threaded pipeline at window 4 gave three
different hashes. Delivering audio on a constant delay instead -- one sample
handed over for every sample rendered, taken a fixed number of samples late --
ties the output to counters rather than to timing. Measured on a Pi 4B: the raw
ASIC3 tap is then byte-identical to serial and identical across runs, the wav is
the serial stream shifted by that fixed delay, and it still runs at 1.27x
against serial's 0.64x. Two samples of delay is enough; it only has to cover the
samples in flight.

**The parallel pipeline is very likely why.** The stages let the H8S run ahead of
the audio, and HOW FAR it gets before the stages catch up depends on thread
timing, so the emulator covers a slightly different span of emulated time each
run. Measured directly on a Pi with the threaded pipeline in `Device`: window 1
(no run-ahead) renders byte-identically every time, window 4 gives three
different sizes and hashes over three runs. The Move plugin forks with
1024-deep rings, i.e. an effectively unbounded window, which is the same
mechanism. Not proven to be the whole story for the case below, but it is the
same class and it now has a knob.

**Performance SWITCHING is intermittently nondeterministic; the performances are
not.** Each of performances 4, 9, 17 and 31 held alone renders bit-identical
across runs, so this is not a random LFO or S&H in the sound. Selecting one
performance and then another, with notes played near the switch, gives 2-3
distinct hashes over 6 runs of the SAME binary; the runs diverge at an identical
sample index with 0.1-1% of samples differing. Switching with notes kept well
clear of the switch is deterministic. Root cause not established -- it is in the
render path, not in our optimizations.

**The note-only scripts do not exercise patch changes.** They send `pc 0` and
nothing else, so they compile 15 ESP programs at boot and none after -- which
means they say nothing about per-core dirty tracking, whose whole purpose is the
recompile burst at a patch change. `patch_sweep` walks eight factory patches
(109 program compiles, 762 `genProgram` calls) and is the script that covers it.
`pc`/`cc` take an optional channel so a performance select on channel 16 is
expressible at all.

The plan for what goes upstream, in what order, and which of our commits has to
be split to get there is `docs/plans/2026-09-03-upstreaming.md`.

**Upstream has no JP-8000 audio regression corpus.** `jeTestConsole` plays the
factory demo to a wav; there is no `add_test` and no ctest anywhere for je8086.
Any bit-exactness evidence for an upstream PR has to come from here.

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
