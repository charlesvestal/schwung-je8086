/*
 * JP-8000 DSP Plugin for Schwung
 *
 * Runs the JE-8086 (Roland JP-8000) emulator using fork-parallel processing:
 *   Parent process: H8S microcontroller + ASIC0+1 on core 2
 *   Child process:  ASIC2+3 direct sample loop on core 3 (no H8S)
 *
 * Communication via mmap'd shared memory:
 *   - GRAM ring: parent → child (ASIC1→ASIC2 handoff, 6×int32_t per sample)
 *   - Audio ring: child → parent (ASIC3 stereo output, int16_t interleaved)
 *   - MIDI FIFO: parent → child (raw MIDI bytes)
 *
 * Internal rate: 88200 Hz → resampled to 44100 Hz output (2:1 linear interp)
 *
 * GPL-3.0 License
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <sched.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <algorithm>
#include <vector>
#include <string>
#include <array>
#include <functional>

/* Gearmulator headers */
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"
#include "synthLib/audioTypes.h"
#include "baseLib/os.h"

/* Plugin API v2 */
extern "C" {

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

} /* extern "C" */

/* =====================================================================
 * Constants
 * ===================================================================== */

#define JE_INTERNAL_RATE    88200
#define AUDIO_RING_SIZE     8192   /* int16_t stereo pairs */
/* Producer sleeps above this (~68 ms at 88.2 kHz) and boots to it: this is
 * both the stall the pipeline can absorb and the MIDI-to-audio latency. */
#define AUDIO_RING_THROTTLE 6000
#define MIDI_FIFO_SIZE      4096
#define OUTPUT_GAIN         0.5f
#define JE_BLOCK_SIZE       128

/* GRAM handoff ring (same as je_fork_shm.h but embedded in shm) */
#define JE_GRAM_RING_CAP    1024
#define JE_GRAM_RING_MASK   (JE_GRAM_RING_CAP - 1)
#define JE_GRAM_COUNT       10  /* widest boundary payload (2->3 + 0xa0/0xa2); see JE_HANDOFF_MAX */

/* Pipeline shape. The emulator is an N-stage fork pipeline: stage 0 (the DSP
 * parent) runs the H8S plus ASICs [0, b1); each child stage s owns the
 * contiguous ASIC run [b_s, b_{s+1}) and hands its GRAM boundary to the next
 * stage through a per-stage ring; the last stage writes audio. Every stage
 * lags the one before it by one sample, and H8S register writes travel
 * stamped with the parent's sample index so each stage applies them before
 * rendering the same index (bit-exact with the serial emulator on all
 * presets tested; see src/benchmark/dsp_bench.cpp).
 *
 * Measured on the Move with the dense emitter, budget 11.34 us/sample:
 *   2-stage {H8S+A0}/{A1..3}:      parent 9.4 / child 9.1   -> child-bound
 *   3-stage {H8S+A0}/{A1}/{A2,3}:  stage0 ~8.5 / 6.0 / 5.1  -> H8S-bound
 * so the default is the 3-stage shape and the H8S interpreter sets the ceiling.
 *
 * Boundaries can be overridden without a rebuild by writing e.g. "1,2" or "2"
 * to JE_PIPELINE_FILE (read once at load); cores likewise via a second line
 * "2,1,0" (stage0, stage1, ...). Core 3 is the SPI core and is never used
 * by default. */
#define JE_MAX_STAGES       4
#define JE_PIPELINE_FILE    "/data/UserData/schwung/jp8000_pipeline"
static const int JE_DEFAULT_BOUNDS[JE_MAX_STAGES - 1] = {1, 2, 0};
static const int JE_DEFAULT_CORES[JE_MAX_STAGES]      = {2, 1, 0, 0};

/* ARM memory barriers for cross-process SPSC ring buffers.
 * Without these, ARM's weak memory ordering lets the index update
 * become visible before the data writes, causing torn reads. */
#ifdef __aarch64__
#define SHM_STORE_FENCE() __asm__ volatile("dmb ishst" ::: "memory")
#define SHM_LOAD_FENCE()  __asm__ volatile("dmb ishld" ::: "memory")
#else
#define SHM_STORE_FENCE() __asm__ volatile("" ::: "memory")
#define SHM_LOAD_FENCE()  __asm__ volatile("" ::: "memory")
#endif

static const host_api_v1_t *g_host = nullptr;

static int64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void plugin_log(const char *fmt, ...) {
    if (!g_host || !g_host->log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_host->log(buf);
}

/* Pin the calling process to a core. Linux-only; no-op elsewhere so the
 * plugin can be built and exercised natively on macOS for testing. */
static void pin_to_core(int core) {
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    sched_setaffinity(0, sizeof(cs), &cs);
#else
    (void)core;
#endif
}

/*
 * Debug log. Armed by `touch /data/UserData/schwung/jp8000_debug_on`, checked
 * once per process; off by default. NEVER call from on_midi / render_block /
 * the stage loops: those run on the SPI callback (host side) or in the audio
 * hot path (child side), and an fflush to eMMC blocks ~100 ms when writeback
 * kicks in. A `% 10000` uc-write log in stage 0 was exactly that — a 60-100 ms
 * all-stage freeze every few seconds that looked like an emulator stall.
 */
#define JP8000_DEBUG_ARM_FILE "/data/UserData/schwung/jp8000_debug_on"
static FILE *g_vlog = nullptr;
static int g_vlog_armed = -1;
static void vlog(const char *fmt, ...) {
    if (g_vlog_armed < 0) g_vlog_armed = access(JP8000_DEBUG_ARM_FILE, F_OK) == 0 ? 1 : 0;
    if (!g_vlog_armed) return;
    if (!g_vlog) {
        g_vlog = fopen("/data/UserData/jp8000_debug.log", "a");
        if (!g_vlog) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_vlog, fmt, ap);
    va_end(ap);
    fprintf(g_vlog, "\n");
    fflush(g_vlog);
}

/* =====================================================================
 * Shared memory structure (parent ↔ child)
 * ===================================================================== */

struct je_gram_entry_t {
    int32_t gram[JE_GRAM_COUNT];
};

/* A forwarded H8S register write. `sample` is the parent's sample index when
 * the write happened; the write precedes that sample's render, so the owning
 * stage applies it before rendering the SAME index — never on arrival, which
 * is up to a gram ring (~11.6 ms) early when the parent runs ahead. Applying
 * on arrival cost a 9e-3 deviation from the serial emulator; stamped, the
 * split is bit-exact. */
#define UC_WRITE_RING_CAP 8192
struct uc_write_t { uint8_t asic; uint8_t val; uint16_t addr; uint32_t sample; };

/* One pipeline stage. Stage s owns ASICs [lo, hi); its INPUT rings are written
 * by stage s-1 (gram) and by the parent (uc writes, only for ASICs it owns).
 * Stage 0 is the DSP parent itself and uses only samples_produced. */
struct je_stage_t {
    int lo, hi;
    volatile int ready;
    volatile int alive;
    uc_write_t uc_ring[UC_WRITE_RING_CAP];
    volatile int uc_write;
    volatile int uc_read;
    je_gram_entry_t gram_ring[JE_GRAM_RING_CAP];
    volatile int gram_write;
    volatile int gram_read;
    /* Samples this stage has published to the next stage (or to audio) */
    volatile int64_t samples_produced;
};

struct jp8000_shm_t {
    /* Audio ring: last stage writes int16_t stereo pairs, plugin reads */
    int16_t audio_ring[AUDIO_RING_SIZE * 2];
    volatile int ring_read;
    volatile int ring_write;

    /* Pipeline */
    int num_stages;
    int cores[JE_MAX_STAGES];
    je_stage_t stage[JE_MAX_STAGES];

    /* MIDI FIFO: parent writes, child reads */
    uint8_t midi_buf[MIDI_FIFO_SIZE];
    volatile int midi_read;
    volatile int midi_write;

    /* Control */
    volatile int child_ready;
    volatile int child_shutdown;
    volatile int child_alive;

    /* Status */
    volatile int initialized;
    volatile int loading_complete;
    char loading_status[128];
    char load_error[256];

    /* Module directory */
    char module_dir[256];

    /* Profiling */
    volatile int underrun_count;
    volatile int render_count;
    volatile int midi_count;      /* on_midi calls, reported in __status */
    volatile int out_peak;        /* max |sample| of the last rendered block, L/R */
    volatile int out_min;         /* min signed sample of the last block (a DC pedestal shows here) */

    /* Child-owned ASIC readback registers, stage → parent, so the H8S in
     * the parent reads live values instead of its stale local copies. Async
     * by construction; the firmware never reads ASIC1's and reads ASIC2/3's
     * <0.06 times per sample, and the result is bit-exact with serial. */
    volatile uint8_t readback[4][4];
};

/* =====================================================================
 * Ring buffer helpers
 * ===================================================================== */

static int audio_ring_available(jp8000_shm_t *shm) {
    int avail = shm->ring_write - shm->ring_read;
    if (avail < 0) avail += AUDIO_RING_SIZE;
    return avail;
}

static int audio_ring_free(jp8000_shm_t *shm) {
    return AUDIO_RING_SIZE - 1 - audio_ring_available(shm);
}

static int gram_ring_available(const je_stage_t *st) {
    int avail = st->gram_write - st->gram_read;
    if (avail < 0) avail += JE_GRAM_RING_CAP * 2;
    return avail;
}

static int midi_fifo_available(jp8000_shm_t *shm) {
    int avail = shm->midi_write - shm->midi_read;
    if (avail < 0) avail += MIDI_FIFO_SIZE;
    return avail;
}

static int midi_fifo_free(jp8000_shm_t *shm) {
    return MIDI_FIFO_SIZE - 1 - midi_fifo_available(shm);
}

static int uc_write_ring_available(const je_stage_t *st) {
    int avail = st->uc_write - st->uc_read;
    if (avail < 0) avail += UC_WRITE_RING_CAP;
    return avail;
}

static void uc_write_ring_push(je_stage_t *st, int asic, uint32_t addr, uint8_t val,
                               uint32_t sample) {
    if (uc_write_ring_available(st) >= UC_WRITE_RING_CAP - 1) return; /* drop if full */
    int wi = st->uc_write;
    st->uc_ring[wi] = uc_write_t{ (uint8_t)asic, val, (uint16_t)addr, sample };
    SHM_STORE_FENCE();
    st->uc_write = (wi + 1) % UC_WRITE_RING_CAP;
}

/*
 * Scheduling policy of the DSP processes. The child is forked from whatever
 * thread called create_instance, and inherits its class: the SPI callback
 * (FIFO 70) on today's chain host, a FIFO 20 loader once schwung#303 lands,
 * SCHED_OTHER under plugin_drive run as `ableton`.
 *
 * Measured 2026-09-02 on the device at 1.15x with Move running: SCHED_OTHER
 * stages get preempted by MoveOriginal's main thread (~45-90% of a core) for
 * ~100 ms windows and underrun 4 runs in 6; at FIFO 20 the ring never leaves
 * the throttle, 6 runs in 6. FIFO 70 is worse than either: it is above Move's
 * `Link Main` (35) and starves Link Audio delivery — the "fork" entry in the
 * 2026-08-22 RT thread audit was this module.
 *
 * So: an inherited realtime priority is CLAMPED to JP8000_RT_PRIO, never
 * raised and never dropped to SCHED_OTHER. MoveOriginal runs as `ableton`
 * with RLIMIT_RTPRIO 0, so a process that leaves the realtime class cannot
 * come back; and the stages fork from this process later and inherit
 * whatever this leaves in place.
 */
#define JP8000_RT_PRIO 20
static void child_clamp_realtime(jp8000_shm_t *shm) {
    (void)shm;
#ifdef __linux__
    const int pol = sched_getscheduler(0);
    if (pol != SCHED_FIFO && pol != SCHED_RR) {
        vlog("[child] sched: inherited SCHED_OTHER, leaving it");
        return;
    }
    struct sched_param sp{};
    sched_getparam(0, &sp);
    if (sp.sched_priority <= JP8000_RT_PRIO) {
        vlog("[child] sched: inherited FIFO %d, leaving it", sp.sched_priority);
        return;
    }
    const int inherited = sp.sched_priority;
    sp.sched_priority = JP8000_RT_PRIO;
    const int rc = sched_setscheduler(0, SCHED_FIFO, &sp);
    sched_getparam(0, &sp);  /* verify: sched_setscheduler has failed silently on this device before */
    vlog("[child] sched: inherited FIFO %d -> FIFO %d (rc=%d, now %d)", inherited, JP8000_RT_PRIO, rc, sp.sched_priority);
#endif
}

/* Wait for a cross-process condition: spin briefly (the other side is one
 * sample away most of the time), then back off to usleep so an idle core is
 * not burned while the parent throttles on the audio ring. */
template <class Cond>
static bool shm_wait(jp8000_shm_t *shm, Cond cond) {
    for (int i = 0; i < 4000; i++) {
        if (cond()) return true;
        if (shm->child_shutdown) return false;
#ifdef __aarch64__
        __asm__ volatile("yield");
#endif
    }
    while (!cond()) {
        if (shm->child_shutdown) return false;
        usleep(10);
    }
    return true;
}

/* Push one GRAM handoff into stage `to`'s input ring, waiting for space. */
static void gram_ring_push(jp8000_shm_t *shm, je_stage_t *to, const int32_t *gram) {
    if (!shm_wait(shm, [to]{ return gram_ring_available(to) < JE_GRAM_RING_CAP - 1; }))
        return;
    int wi = to->gram_write & JE_GRAM_RING_MASK;
    for (int k = 0; k < JE_GRAM_COUNT; k++)
        to->gram_ring[wi].gram[k] = gram[k];
    SHM_STORE_FENCE();
    to->gram_write = (to->gram_write + 1) % (JE_GRAM_RING_CAP * 2);
}

/* Parse a comma list of ints; returns the count. */
static int parse_int_list(const char *s, int *out, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (*s < '0' || *s > '9') break;
        out[n++] = atoi(s);
        while (*s && *s != ',' && *s != '\n') s++;
        if (*s == ',') s++;
        else break;
    }
    return n;
}

/* Pipeline shape: defaults, or JE_PIPELINE_FILE ("b1,b2\ncore0,core1,...").
 * Fills shm->num_stages, stage[].lo/hi and cores. Bounds must be ascending
 * in 1..3; anything malformed falls back to the defaults. */
static void configure_pipeline(jp8000_shm_t *shm) {
    int bounds[JE_MAX_STAGES - 1]; int nbounds = 0;
    int cores[JE_MAX_STAGES]; int ncores = 0;

    if (FILE *f = fopen(JE_PIPELINE_FILE, "r")) {
        char line[128];
        if (fgets(line, sizeof(line), f)) nbounds = parse_int_list(line, bounds, JE_MAX_STAGES - 1);
        if (fgets(line, sizeof(line), f)) ncores = parse_int_list(line, cores, JE_MAX_STAGES);
        fclose(f);
        for (int i = 0; i < nbounds; i++)
            if (bounds[i] < 1 || bounds[i] > 3 || (i > 0 && bounds[i] <= bounds[i - 1])) { nbounds = 0; break; }
        for (int i = 0; i < ncores; i++)
            if (cores[i] < 0 || cores[i] > 3) { ncores = 0; break; }
    }
    if (nbounds == 0) {
        for (int i = 0; i < JE_MAX_STAGES - 1 && JE_DEFAULT_BOUNDS[i]; i++) bounds[nbounds++] = JE_DEFAULT_BOUNDS[i];
    }
    for (int i = 0; i < JE_MAX_STAGES; i++)
        shm->cores[i] = (i < ncores) ? cores[i] : JE_DEFAULT_CORES[i];

    shm->num_stages = nbounds + 1;
    shm->stage[0].lo = 0; shm->stage[0].hi = bounds[0];
    for (int s = 1; s < shm->num_stages; s++) {
        shm->stage[s].lo = bounds[s - 1];
        shm->stage[s].hi = (s < nbounds) ? bounds[s] : 4;
    }
}

static void midi_fifo_push(jp8000_shm_t *shm, const uint8_t *msg, int len) {
    if (len < 1 || len > 8) return;
    if (midi_fifo_free(shm) < len + 1) return;
    int wr = shm->midi_write;
    shm->midi_buf[wr] = (uint8_t)len;
    wr = (wr + 1) % MIDI_FIFO_SIZE;
    for (int i = 0; i < len; i++) {
        shm->midi_buf[wr] = msg[i];
        wr = (wr + 1) % MIDI_FIFO_SIZE;
    }
    SHM_STORE_FENCE();
    shm->midi_write = wr;
}

/* =====================================================================
 * Instance structure
 * ===================================================================== */

/* Output DC blocker, one per channel. The ESP's DAC word carries a DC
 * pedestal (about -85 in the 24-bit word, -4 LSB after the plugin gain on the
 * left channel, -1 on the right) and the patch's chorus/delay LFOs ride a
 * +-2 LSB ripple on it. The emulation is faithful; the problem is the host:
 * the shim only idles a slot whose output stays within +-4 LSB for a second,
 * so with the pedestal the three stages never sleep and cost ~1.5 cores for
 * the rest of the session even with nothing playing (measured 2026-09-02:
 * 48-52% of a core per stage at rest). A one-pole HPF at ~3 Hz removes the
 * pedestal and is inaudible. */
struct dc_block_t { float x1, y1; };
static inline int16_t dc_block(dc_block_t *d, int32_t x) {
    float y = (float)x - d->x1 + 0.9995f * d->y1;   /* fc ~ 3.5 Hz at 44.1 kHz */
    if (y < 1e-6f && y > -1e-6f) y = 0.0f;           /* no denormal tail */
    d->x1 = (float)x; d->y1 = y;
    int v = (int)lrintf(y);
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    return (int16_t)v;
}

struct jp8000_instance_t {
    jp8000_shm_t *shm;
    pid_t child_pid;
    pthread_t boot_thread;
    volatile int boot_thread_running;
    dc_block_t dc[2];
};

/* =====================================================================
 * Child process — DSP work happens here
 * ===================================================================== */

static jp8000_shm_t *g_child_shm = nullptr;

static void child_crash_handler(int sig) {
    if (g_child_shm) {
        snprintf((char*)g_child_shm->load_error, sizeof(g_child_shm->load_error),
                 "DSP crashed (signal %d)", sig);
        g_child_shm->initialized = 1;
        g_child_shm->loading_complete = 1;
    }
    _exit(1);
}

static void child_process_midi(jp8000_shm_t *shm, jeLib::Je8086 &je) {
    while (midi_fifo_available(shm) > 0) {
        int rd = shm->midi_read;
        int len = shm->midi_buf[rd];
        rd = (rd + 1) % MIDI_FIFO_SIZE;
        if (len < 1 || len > 8) { shm->midi_read = rd; continue; }

        uint8_t msg[8];
        for (int i = 0; i < len; i++) {
            msg[i] = shm->midi_buf[rd];
            rd = (rd + 1) % MIDI_FIFO_SIZE;
        }
        shm->midi_read = rd;

        /* Send MIDI via addMidiEvent (goes through rate limiter → Serial) */
        synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host,
                                msg[0],
                                len > 1 ? msg[1] : 0,
                                len > 2 ? msg[2] : 0);
        je.addMidiEvent(ev);
    }
}

/* Also try: send MIDI note directly via Serial for immediate testing.
 * Call this once after boot to verify audio output works at all. */
static void child_send_test_note(jeLib::Je8086 &je) {
    /* Note-on C4 velocity 100 on channel 0 */
    synthLib::SMidiEvent noteOn(synthLib::MidiEventSource::Host, 0x90, 60, 100);
    je.addMidiEvent(noteOn);
}

/* One forked pipeline stage: ASICs [lo, hi) of stage s, one iteration per
 * sample index. Consumes the lo-1 -> lo handoff from stage s-1, produces the
 * hi-1 -> hi handoff to stage s+1 (or audio when it is the last stage). */
static void stage_main(jp8000_shm_t *shm, int s, jeLib::Je8086 &je) {
    je_stage_t *st = &shm->stage[s];
    je_stage_t *prev = &shm->stage[s - 1];
    je_stage_t *next = (s + 1 < shm->num_stages) ? &shm->stage[s + 1] : nullptr;
    const int lo = st->lo, hi = st->hi;

    g_vlog = nullptr;
    pin_to_core(shm->cores[s]);
    baseLib::setFlushDenormalsToZero();
    jeLib::devices::g_je_parallel_mode = 2;
    jeLib::devices::g_je_stage_lo = lo;
    jeLib::devices::g_je_stage_hi = hi;

    /* GRAM consume. The main loop has already waited for the entry. */
    jeLib::devices::g_je_gram_consume = [shm, st](int32_t *gram) -> bool {
        if (!shm_wait(shm, [st]{ return gram_ring_available(st) >= 1; })) return false;
        SHM_LOAD_FENCE();
        int ri = st->gram_read & JE_GRAM_RING_MASK;
        for (int k = 0; k < JE_GRAM_COUNT; k++)
            gram[k] = st->gram_ring[ri].gram[k];
        st->gram_read = (st->gram_read + 1) % (JE_GRAM_RING_CAP * 2);
        return true;
    };

    if (next) {
        jeLib::devices::g_je_gram_produce = [shm, st, next](const int32_t *gram) {
            gram_ring_push(shm, next, gram);
            st->samples_produced++;
        };
    } else {
        /* Audio output to ring.
         * asic3 emits 24-bit signed samples in an int32 container
         * (range ±2^23). The Device/JUCE path does `dsp2sample<float>(s)
         * * masterVolume` with a default master volume of 12.0:
         *   int16 = s / 2^23 * 12 * 2^15 = s * 12/256 ≈ (s*3)>>6.
         * The synth's own output level provides the headroom (the DAC
         * output never approaches 24-bit full scale), same as on the
         * JUCE plugin; clamp guards the rest. */
        je.getAsics().setPostSample([shm, st](int32_t left, int32_t right) {
            if (audio_ring_free(shm) < 1) return;
            constexpr int32_t GAIN_NUM = 3;  /* *3 >>6 ≈ JUCE master vol 12.0 */
            constexpr int32_t SHIFT = 6;
            int32_t l = (left * GAIN_NUM) >> SHIFT;
            int32_t r = (right * GAIN_NUM) >> SHIFT;
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            int wr = shm->ring_write;
            shm->audio_ring[wr * 2 + 0] = (int16_t)l;
            shm->audio_ring[wr * 2 + 1] = (int16_t)r;
            SHM_STORE_FENCE();
            shm->ring_write = (wr + 1) % AUDIO_RING_SIZE;
            st->samples_produced++;
        });
    }

    st->ready = 1;
    vlog("[stage%d] ready on core %d, pid=%d, ASIC%d..%d", s, shm->cores[s], (int)getpid(), lo, hi - 1);

    auto& asics = je.getAsics();
    uint32_t sample = 0;
    while (!shm->child_shutdown) {
        /* Every H8S write that precedes sample N is in our uc ring once the
         * PARENT has published sample N; the previous stage only publishes N
         * after seeing that, so its counter implies the parent's. gram N itself
         * is consumed inside processSampleChild (it feeds N+1). */
        if (!shm_wait(shm, [st, prev, sample]{
                return gram_ring_available(st) >= 1 &&
                       prev->samples_produced > (int64_t)sample; }))
            break;
        SHM_LOAD_FENCE();
        while (uc_write_ring_available(st) > 0) {
            int ri = st->uc_read;
            const uc_write_t w = st->uc_ring[ri];
            if (w.sample > sample) break;
            st->uc_read = (ri + 1) % UC_WRITE_RING_CAP;
            asics.applyUcWrite(w.asic, w.addr, w.val);
        }

        if (!asics.processSampleChild()) break;
        sample++;
        for (int a = lo; a < hi; a++)
            asics.getReadback(a, (uint8_t*)shm->readback[a]);
        st->alive++;
    }
    vlog("[stage%d] shutdown after %lld samples", s, (long long)st->samples_produced);
}

static void child_main(jp8000_shm_t *shm) {
    g_child_shm = shm;
    signal(SIGSEGV, child_crash_handler);
    signal(SIGBUS, child_crash_handler);
    signal(SIGABRT, child_crash_handler);
    g_vlog = nullptr; /* reopen in child */

    vlog("[child] started, pid=%d", (int)getpid());
    child_clamp_realtime(shm);

    /* Don't pin cores during boot — let OS schedule freely.
     * Core pinning happens after boot, before real-time DSP work. */
    baseLib::setFlushDenormalsToZero();

    /* 1. Load ROM */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Loading ROM...");
    char roms_dir[512];
    snprintf(roms_dir, sizeof(roms_dir), "%s/roms", shm->module_dir);

    (void)chdir(roms_dir);
    synthLib::RomLoader::addSearchPath(std::string(roms_dir));

    auto rom = jeLib::RomLoader::findROM();
    if (!rom.isValid()) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error),
                 "No JP-8000 ROM found in roms/ directory.");
        shm->initialized = 1; shm->loading_complete = 1;
        vlog("[child] no ROM found");
        return;
    }
    vlog("[child] ROM: %s", rom.getName().c_str());

    /* 2. Create Je8086 and try to load snapshot for instant boot */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Creating emulator...");
    std::string ramFile = std::string(roms_dir) + "/ram_dump.bin";
    auto *je = new jeLib::Je8086(rom.getData(), ramFile);

    if (je->hasDoneFactoryReset()) {
        delete je;
        je = new jeLib::Je8086(rom.getData(), ramFile);
    }

    /* Try snapshot first — instant boot */
    char snapPath[512];
    snprintf(snapPath, sizeof(snapPath), "%s/roms/boot.snap", shm->module_dir);
    if (je->loadSnapshot(snapPath)) {
        vlog("[child] snapshot loaded from %s — instant boot!", snapPath);
    } else {
        /* Fall back to step-by-step boot (~30s) */
        vlog("[child] no snapshot, booting from scratch...");
        snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Booting DSP (first run)...");

        jeLib::SysexRemoteControl sysexRemote;
        bool bootFinished = false;
        sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& _lcdContent) {
            char lcd[41]{0};
            for (size_t i = 0; i < _lcdContent.size(); ++i)
                lcd[i] = _lcdContent[i] >= ' ' ? static_cast<char>(_lcdContent[i]) : ' ';
            if (std::string(lcd).find("PERFORM") != std::string::npos)
                bootFinished = true;
        });

        nice(19);
        int bootSteps = 0;
        while (!bootFinished && bootSteps < 50000000) {
            je->step();
            bootSteps++;
            if (!je->getSampleBuffer().empty()) je->clearSampleBuffer();
            std::vector<synthLib::SMidiEvent> midiOut;
            je->readMidiOut(midiOut);
            for (const auto& e : midiOut) sysexRemote.receive(e);
            if (bootSteps % 1000 == 0) sched_yield();
            if (bootSteps % 500000 == 0) {
                vlog("[child] boot: step %dk", bootSteps / 1000);
                snprintf((char*)shm->loading_status, sizeof(shm->loading_status),
                         "Booting DSP... (%dk)", bootSteps / 1000);
            }
        }
        nice(-19);
        vlog("[child] boot complete after %d steps", bootSteps);
    }
    /* Warm-up: run serial mode 0 for ~2s after snapshot load so H8S/DSP
     * state stabilizes before switching to fork-parallel mode. */
    {
        int warmup_samples = 0;
        vlog("[child] warming up DSP (serial mode 0) for ~2s...");
        for (int i = 0; i < 50000000 && warmup_samples < 88200 * 2; i++) {
            je->step();
            auto& buf = je->getSampleBuffer();
            warmup_samples += (int)buf.size();
            if (!buf.empty()) je->clearSampleBuffer();
        }
        vlog("[child] warm-up done: %d samples (~%.1fs)", warmup_samples,
             warmup_samples / 88200.0);
    }

    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Forking ASIC stages...");

    /* Fork one child per stage — mode stays 0 until after the forks. The
     * first boundary is set before forking so every process agrees on the
     * handoff width; each child sets its own [lo,hi). */
    configure_pipeline(shm);
    vlog("[child] pipeline: stage0 = H8S + ASIC0..%d (core %d)", shm->stage[0].hi - 1, shm->cores[0]);
    for (int s = 1; s < shm->num_stages; s++)
        vlog("[child]           stage%d = ASIC%d..%d (core %d)", s, shm->stage[s].lo, shm->stage[s].hi - 1, shm->cores[s]);
    jeLib::devices::g_je_split_asic = shm->stage[0].hi;
    jeLib::devices::g_je_stage_lo = 0;
    jeLib::devices::g_je_stage_hi = shm->stage[0].hi;

    pid_t stage_pid[JE_MAX_STAGES] = {0};
    for (int s = 1; s < shm->num_stages; s++) {
        pid_t pid = fork();
        if (pid < 0) {
            snprintf((char*)shm->load_error, sizeof(shm->load_error),
                     "ASIC fork failed: %s", strerror(errno));
            shm->initialized = 1; shm->loading_complete = 1;
            shm->child_shutdown = 1;
            return;
        }
        if (pid == 0) {
            stage_main(shm, s, *je);
            _exit(0);
        }
        stage_pid[s] = pid;
    }

    /* === STAGE 0 (this process): H8S + ASICs [0, b1) === */
    for (int s = 1; s < shm->num_stages; s++) vlog("[child] stage%d pid=%d", s, (int)stage_pid[s]);
    pin_to_core(shm->cores[0]);

    /* Wait for every stage to be up */
    for (int s = 1; s < shm->num_stages; s++) {
        for (int i = 0; i < 100 && !shm->stage[s].ready; i++) usleep(10000);
        if (!shm->stage[s].ready)
            vlog("[child] WARNING: stage%d not ready after 1s", s);
    }

    jeLib::devices::g_je_parallel_mode = 1;
    /* H8S writes to an ASIC we do not own go to the ring of the stage that
     * does, stamped with our sample index. */
    static int uc_write_count = 0;
    jeLib::devices::g_je_uc_write_forward = [shm](int asic, uint32_t addr, uint8_t val) {
        for (int s = 1; s < shm->num_stages; s++) {
            je_stage_t *st = &shm->stage[s];
            if (asic >= st->lo && asic < st->hi) {
                uc_write_ring_push(st, asic, addr, val, (uint32_t)shm->stage[0].samples_produced);
                break;
            }
        }
        /* Hot path (stage 0's sample loop): no logging here, see vlog(). */
        uc_write_count++;
    };
    /* GRAM produce: the b1-1 -> b1 handoff into stage 1 */
    jeLib::devices::g_je_gram_produce = [shm](const int32_t *gram) {
        gram_ring_push(shm, &shm->stage[1], gram);
        shm->stage[0].samples_produced++;
    };
    vlog("[child] parallel mode 1 active, %d stages", shm->num_stages);

    const int split = shm->stage[0].hi;

    /* Pre-fill to the throttle level. The host starts rendering the moment
     * it sees "ready", so a ring declared ready at 512 samples (6 ms) was
     * being consumed while it filled, and underran on the first blocks. */
    int prefill = 0;
    while (audio_ring_available(shm) < AUDIO_RING_THROTTLE && prefill < 5000000) {
        for (int a = split; a < 4; a++)
            je->getAsics().setReadback(a, (const uint8_t*)shm->readback[a]);
        je->step();
        if (!je->getSampleBuffer().empty()) je->clearSampleBuffer();
        prefill++;
    }
    vlog("[child] pre-fill done, audio_ring=%d, steps=%d", audio_ring_available(shm), prefill);

    shm->initialized = 1;
    shm->loading_complete = 1;
    shm->child_ready = 1;
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Ready");
    vlog("[child] READY");

    /* Main DSP loop */
    while (!shm->child_shutdown) {
        if (audio_ring_available(shm) >= AUDIO_RING_THROTTLE) {
            usleep(1000);
            continue;
        }

        child_process_midi(shm, *je);

        for (int a = split; a < 4; a++)
            je->getAsics().setReadback(a, (const uint8_t*)shm->readback[a]);

        je->step();
        if (!je->getSampleBuffer().empty()) je->clearSampleBuffer();
        shm->child_alive++;
    }

    for (int s = 1; s < shm->num_stages; s++) kill(stage_pid[s], SIGTERM);
    for (int s = 1; s < shm->num_stages; s++) {
        for (int i = 0; i < 30; i++) {
            int status;
            if (waitpid(stage_pid[s], &status, WNOHANG) == stage_pid[s]) { stage_pid[s] = 0; break; }
            usleep(100000);
        }
        if (stage_pid[s]) { kill(stage_pid[s], SIGKILL); waitpid(stage_pid[s], nullptr, 0); }
    }

    vlog("[child] exiting");
}

/* =====================================================================
 * Boot / fork helpers (parent plugin process)
 * ===================================================================== */

static int fork_and_wait_child(jp8000_instance_t *inst) {
    jp8000_shm_t *shm = inst->shm;
    fprintf(stderr, "JP-8000: fork_and_wait starting...\n");
    vlog("fork_and_wait: forking child for DSP...");

    pid_t pid = fork();
    if (pid < 0) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error),
                 "fork() failed: %s", strerror(errno));
        shm->initialized = 1; shm->loading_complete = 1;
        return -1;
    }

    if (pid == 0) {
        g_vlog = nullptr;
        child_main(shm);
        _exit(0);
    }

    inst->child_pid = pid;
    vlog("fork_and_wait: child pid=%d", (int)pid);

    /* Wait up to 120s for boot (JE device boot is slow) */
    for (int i = 0; i < 1200 && !shm->child_ready; i++) {
        int status;
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid) {
            snprintf((char*)shm->load_error, sizeof(shm->load_error),
                     "DSP process exited (status=%d)", status);
            shm->initialized = 1; shm->loading_complete = 1;
            inst->child_pid = 0;
            return -1;
        }
        usleep(100000);
    }

    if (!shm->child_ready) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error), "DSP boot timed out (120s)");
        shm->initialized = 1; shm->loading_complete = 1;
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        inst->child_pid = 0;
        return -1;
    }

    vlog("fork_and_wait: child ready");
    return 0;
}

static void* boot_thread_func(void *arg) {
    jp8000_instance_t *inst = (jp8000_instance_t*)arg;
    fprintf(stderr, "JP-8000: boot thread starting, module_dir=%s\n", inst->shm->module_dir);
    fork_and_wait_child(inst);
    fprintf(stderr, "JP-8000: boot thread done, ready=%d error=%s\n",
            inst->shm->child_ready, inst->shm->load_error);
    inst->boot_thread_running = 0;
    return nullptr;
}

/* =====================================================================
 * Plugin API v2
 * ===================================================================== */

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)json_defaults;

    jp8000_instance_t *inst = (jp8000_instance_t*)calloc(1, sizeof(jp8000_instance_t));
    if (!inst) return nullptr;

    inst->shm = (jp8000_shm_t*)mmap(nullptr, sizeof(jp8000_shm_t),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (inst->shm == MAP_FAILED) {
        free(inst);
        return nullptr;
    }
    memset(inst->shm, 0, sizeof(jp8000_shm_t));
    strncpy((char*)inst->shm->module_dir, module_dir, sizeof(inst->shm->module_dir) - 1);

    fprintf(stderr, "JP-8000: creating instance from %s\n", module_dir);
    snprintf((char*)inst->shm->loading_status, sizeof(inst->shm->loading_status), "Initializing...");

    inst->boot_thread_running = 1;
    pthread_create(&inst->boot_thread, nullptr, boot_thread_func, inst);
    return inst;
}

static void v2_destroy_instance(void *instance) {
    jp8000_instance_t *inst = (jp8000_instance_t*)instance;
    if (!inst) return;
    fprintf(stderr, "JP-8000: destroying\n");

    if (inst->shm) inst->shm->child_shutdown = 1;

    if (inst->boot_thread_running)
        pthread_join(inst->boot_thread, nullptr);

    if (inst->child_pid > 0) {
        kill(inst->child_pid, SIGTERM);
        for (int i = 0; i < 30; i++) {
            int status;
            if (waitpid(inst->child_pid, &status, WNOHANG) == inst->child_pid) break;
            usleep(100000);
        }
        kill(inst->child_pid, SIGKILL);
        waitpid(inst->child_pid, nullptr, 0);
    }

    if (inst->shm && inst->shm != MAP_FAILED)
        munmap(inst->shm, sizeof(jp8000_shm_t));

    free(inst);
    fprintf(stderr, "JP-8000: destroyed\n");
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    jp8000_instance_t *inst = (jp8000_instance_t*)instance;
    if (!inst || !inst->shm || !inst->shm->initialized || len < 1) return;
    (void)source;

    /* SPI callback: no logging here, see vlog(). */
    uint8_t modified[8];
    int n = len > 8 ? 8 : len;
    memcpy(modified, msg, n);

    midi_fifo_push(inst->shm, modified, n);
    inst->shm->midi_count++;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    jp8000_instance_t *inst = (jp8000_instance_t*)instance;
    if (!inst || !inst->shm) return;
    /* TODO: parameter mapping for JP-8000 */
    (void)key; (void)val;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    jp8000_instance_t *inst = (jp8000_instance_t*)instance;
    if (!inst || !inst->shm) return 0;

    if (strcmp(key, "__ui") == 0) {
        return snprintf(buf, buf_len, "{\"pages\":[]}");
    }

    if (strcmp(key, "__status") == 0) {
        jp8000_shm_t *shm = inst->shm;
        if (shm->load_error[0]) {
            return snprintf(buf, buf_len,
                "{\"status\":\"error\",\"message\":\"%s\"}", shm->load_error);
        }
        if (!shm->loading_complete) {
            return snprintf(buf, buf_len,
                "{\"status\":\"loading\",\"message\":\"%s\"}", shm->loading_status);
        }
        /* Per-stage sample counters, so a stall can be pinned on the stage
         * that stopped rather than read off the ring alone. */
        int n = snprintf(buf, buf_len,
            "{\"status\":\"ready\",\"message\":\"JP-8000 Ready\","
            "\"ring\":%d,\"underruns\":%d,\"midi\":%d,\"peak\":%d,\"min\":%d,\"produced\":[",
            audio_ring_available(shm), shm->underrun_count, shm->midi_count,
            shm->out_peak, shm->out_min);
        for (int s = 0; s < shm->num_stages && n < buf_len; s++)
            n += snprintf(buf + n, buf_len - n, "%s%lld", s ? "," : "",
                          (long long)shm->stage[s].samples_produced);
        if (n < buf_len) n += snprintf(buf + n, buf_len - n, "]}");
        return n;
    }

    return snprintf(buf, buf_len, "0");
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    jp8000_instance_t *inst = (jp8000_instance_t*)instance;
    if (!inst || !inst->shm) return 0;
    if (inst->shm->load_error[0])
        return snprintf(buf, buf_len, "%s", inst->shm->load_error);
    return 0;
}

static void v2_render_block(void *instance, int16_t *out, int frames) {
    jp8000_instance_t *inst = (jp8000_instance_t*)instance;
    if (!inst || !inst->shm || !inst->shm->child_ready) {
        memset(out, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    jp8000_shm_t *shm = inst->shm;

    /* SPI callback: no logging here, see vlog(). Ring / underrun / per-stage
     * counters are exposed through the __status param instead. */

    /* The audio ring contains samples at 88200 Hz.
     * Output is 44100 Hz. Decimate 2:1 with simple averaging. */
    SHM_LOAD_FENCE();  /* ensure we see audio data written before ring_write */
    int avail = audio_ring_available(shm);
    int needed = frames * 2; /* 2 input samples per 1 output sample */
    int to_read = (avail < needed) ? avail : needed;
    /* Round down to even */
    to_read &= ~1;

    int rd = shm->ring_read;
    int out_idx = 0;
    int peak = 0, mn = 0;
    for (int i = 0; i < to_read; i += 2) {
        if (out_idx >= frames) break;
        /* Average two consecutive samples for 2:1 decimation */
        int32_t l = (int32_t)shm->audio_ring[rd * 2 + 0];
        int32_t r = (int32_t)shm->audio_ring[rd * 2 + 1];
        rd = (rd + 1) % AUDIO_RING_SIZE;
        l += (int32_t)shm->audio_ring[rd * 2 + 0];
        r += (int32_t)shm->audio_ring[rd * 2 + 1];
        rd = (rd + 1) % AUDIO_RING_SIZE;
        l = dc_block(&inst->dc[0], l / 2);
        r = dc_block(&inst->dc[1], r / 2);
        out[out_idx * 2 + 0] = (int16_t)l;
        out[out_idx * 2 + 1] = (int16_t)r;
        /* Block peak / min for __status: the shim only idles a slot whose
         * output stays within +-4 LSB, and the ESP's DAC word carries a small
         * DC pedestal, so "is this slot ever silent" is a question worth
         * being able to answer from the device. */
        int al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > peak) peak = al;
        if (ar > peak) peak = ar;
        if (l < mn) mn = l;
        if (r < mn) mn = r;
        out_idx++;
    }
    shm->ring_read = rd;
    shm->out_peak = peak;
    shm->out_min = mn;

    if (out_idx < frames) {
        shm->underrun_count++;
        memset(out + out_idx * 2, 0, (frames - out_idx) * 2 * sizeof(int16_t));
    }
    shm->render_count++;
}

/* =====================================================================
 * Entry point
 * ===================================================================== */

static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;
    return &g_plugin_api_v2;
}
