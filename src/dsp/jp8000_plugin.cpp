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
#define MIDI_FIFO_SIZE      4096
#define OUTPUT_GAIN         0.5f
#define JE_BLOCK_SIZE       128

/* GRAM handoff ring (same as je_fork_shm.h but embedded in shm) */
#define JE_GRAM_RING_CAP    1024
#define JE_GRAM_RING_MASK   (JE_GRAM_RING_CAP - 1)
#define JE_GRAM_COUNT       6

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

static FILE *g_vlog = nullptr;
static void vlog(const char *fmt, ...) {
    if (!g_vlog) {
        g_vlog = fopen("/data/UserData/jp8000_debug.log", "a");
        if (!g_vlog) g_vlog = fopen("/tmp/jp8000_debug.log", "a");
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

struct jp8000_shm_t {
    /* Audio ring: child writes int16_t stereo pairs, parent reads */
    int16_t audio_ring[AUDIO_RING_SIZE * 2];
    volatile int ring_read;
    volatile int ring_write;

    /* GRAM ring: parent writes, child reads */
    je_gram_entry_t gram_ring[JE_GRAM_RING_CAP];
    volatile int gram_write;
    volatile int gram_read;

    /* MIDI FIFO: parent writes, child reads */
    uint8_t midi_buf[MIDI_FIFO_SIZE];
    volatile int midi_read;
    volatile int midi_write;

    /* uC write ring: parent forwards ASIC2/3 writes, child applies them.
     * Each entry: [asic(1 byte), addr_hi, addr_lo, value] = 4 bytes */
    #define UC_WRITE_RING_CAP 4096
    uint8_t uc_write_ring[UC_WRITE_RING_CAP * 4];
    volatile int uc_write_read;
    volatile int uc_write_write;

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
    volatile int64_t child_samples_produced;
    volatile int64_t parent_samples_produced;

    /* ASIC2/3 readback register forwarding: child → parent.
     * The H8S in the parent reads these instead of stale local copies. */
    volatile uint8_t asic2_readback[4];
    volatile uint8_t asic3_readback[4];
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

static int gram_ring_available(jp8000_shm_t *shm) {
    int avail = shm->gram_write - shm->gram_read;
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

static int uc_write_ring_available(jp8000_shm_t *shm) {
    int avail = shm->uc_write_write - shm->uc_write_read;
    if (avail < 0) avail += UC_WRITE_RING_CAP;
    return avail;
}

static void uc_write_ring_push(jp8000_shm_t *shm, int asic, uint32_t addr, uint8_t val) {
    if (uc_write_ring_available(shm) >= UC_WRITE_RING_CAP - 1) return; /* drop if full */
    int wi = shm->uc_write_write;
    int off = wi * 4;
    shm->uc_write_ring[off + 0] = (uint8_t)asic;
    shm->uc_write_ring[off + 1] = (uint8_t)(addr >> 8);
    shm->uc_write_ring[off + 2] = (uint8_t)(addr & 0xFF);
    shm->uc_write_ring[off + 3] = val;
    SHM_STORE_FENCE();
    shm->uc_write_write = (wi + 1) % UC_WRITE_RING_CAP;
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

struct jp8000_instance_t {
    jp8000_shm_t *shm;
    pid_t child_pid;
    pthread_t boot_thread;
    volatile int boot_thread_running;
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

static void child_main(jp8000_shm_t *shm) {
    g_child_shm = shm;
    signal(SIGSEGV, child_crash_handler);
    signal(SIGBUS, child_crash_handler);
    signal(SIGABRT, child_crash_handler);
    g_vlog = nullptr; /* reopen in child */

    vlog("[child] started, pid=%d", (int)getpid());

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

    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Forking ASIC child...");

    /* Fork ASIC child — mode stays 0 until after fork */
    pid_t asic_pid = fork();
    if (asic_pid < 0) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error),
                 "ASIC fork failed: %s", strerror(errno));
        shm->initialized = 1; shm->loading_complete = 1;
        return;
    }

    if (asic_pid == 0) {
        /* === ASIC CHILD: runs ASIC2+3 === */
        g_vlog = nullptr;
        pin_to_core(3);
        baseLib::setFlushDenormalsToZero();

        /* GRAM consume */
        jeLib::devices::g_je_gram_consume = [shm](int32_t *gram) -> bool {
            while (gram_ring_available(shm) < 1) {
                if (shm->child_shutdown) return false;
                usleep(10);
            }
            SHM_LOAD_FENCE();
            int ri = shm->gram_read & JE_GRAM_RING_MASK;
            for (int k = 0; k < JE_GRAM_COUNT; k++)
                gram[k] = shm->gram_ring[ri].gram[k];
            shm->gram_read = (shm->gram_read + 1) % (JE_GRAM_RING_CAP * 2);
            return true;
        };

        /* Audio output to ring.
         * asic3 emits 24-bit signed samples in an int32 container
         * (range ±2^23). The Device/JUCE path does `dsp2sample<float>(s)
         * * masterVolume` with a default master volume of 12.0:
         *   int16 = s / 2^23 * 12 * 2^15 = s * 12/256 ≈ (s*3)>>6.
         * The synth's own output level provides the headroom (the DAC
         * output never approaches 24-bit full scale), same as on the
         * JUCE plugin; clamp guards the rest. */
        je->getAsics().setPostSample([shm](int32_t left, int32_t right) {
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
            shm->child_samples_produced++;
        });

        /* ASIC2+3 loop */
        auto& asics = je->getAsics();
        while (!shm->child_shutdown) {
            while (uc_write_ring_available(shm) > 0) {
                SHM_LOAD_FENCE();
                int ri = shm->uc_write_read;
                int off = ri * 4;
                int asic = shm->uc_write_ring[off + 0];
                uint32_t addr = ((uint32_t)shm->uc_write_ring[off + 1] << 8) |
                                shm->uc_write_ring[off + 2];
                uint8_t val = shm->uc_write_ring[off + 3];
                shm->uc_write_read = (ri + 1) % UC_WRITE_RING_CAP;
                asics.applyUcWrite(asic, addr, val);
            }

            if (!asics.processSampleAsic23()) break;

            asics.getAsic23Readback(
                (uint8_t*)shm->asic2_readback,
                (uint8_t*)shm->asic3_readback);
        }
        _exit(0);
    }

    /* === PARENT DSP: H8S + ASIC0+1 === */
    vlog("[child] ASIC fork done, asic_pid=%d", (int)asic_pid);
    pin_to_core(2);

    usleep(50000);

    jeLib::devices::g_je_parallel_mode = 1;
    jeLib::devices::g_je_gram_produce = [shm](const int32_t *gram) {
        while (gram_ring_available(shm) >= JE_GRAM_RING_CAP - 1) {
            if (shm->child_shutdown) return;
            usleep(10);
        }
        int wi = shm->gram_write & JE_GRAM_RING_MASK;
        for (int k = 0; k < JE_GRAM_COUNT; k++)
            shm->gram_ring[wi].gram[k] = gram[k];
        SHM_STORE_FENCE();
        shm->gram_write = (shm->gram_write + 1) % (JE_GRAM_RING_CAP * 2);
        shm->parent_samples_produced++;
    };
    static int uc_write_count = 0;
    jeLib::devices::g_je_uc_write_forward = [shm](int asic, uint32_t addr, uint8_t val) {
        uc_write_ring_push(shm, asic, addr, val);
        uc_write_count++;
        if (uc_write_count <= 20 || uc_write_count % 10000 == 0)
            vlog("[uc-fwd] #%d asic=%d addr=0x%04x val=0x%02x", uc_write_count, asic, addr, val);
    };
    vlog("[child] parallel mode 1 active");

    /* Pre-fill */
    int prefill = 0;
    while (audio_ring_available(shm) < 512 && prefill < 5000000) {
        je->getAsics().setAsic23Readback(
            (const uint8_t*)shm->asic2_readback,
            (const uint8_t*)shm->asic3_readback);
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
        if (audio_ring_available(shm) >= 6000) {
            usleep(1000);
            continue;
        }

        child_process_midi(shm, *je);

        je->getAsics().setAsic23Readback(
            (const uint8_t*)shm->asic2_readback,
            (const uint8_t*)shm->asic3_readback);

        je->step();
        if (!je->getSampleBuffer().empty()) je->clearSampleBuffer();
        shm->child_alive++;
    }

    kill(asic_pid, SIGTERM);
    for (int i = 0; i < 30; i++) {
        int status;
        if (waitpid(asic_pid, &status, WNOHANG) == asic_pid) break;
        usleep(100000);
    }
    kill(asic_pid, SIGKILL);
    waitpid(asic_pid, nullptr, 0);

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

    vlog("[midi] status=0x%02X d1=%d d2=%d len=%d",
         msg[0], len > 1 ? msg[1] : 0, len > 2 ? msg[2] : 0, len);

    uint8_t modified[8];
    int n = len > 8 ? 8 : len;
    memcpy(modified, msg, n);

    midi_fifo_push(inst->shm, modified, n);
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
        return snprintf(buf, buf_len,
            "{\"status\":\"ready\",\"message\":\"JP-8000 Ready\","
            "\"ring\":%d,\"underruns\":%d}",
            audio_ring_available(shm), shm->underrun_count);
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

    /* Log first 5 calls then every ~10 seconds */
    if (shm->render_count < 5 || shm->render_count % 3450 == 0) {
        /* Sample a few values from the ring to check for non-zero audio */
        int peek_avail = audio_ring_available(shm);
        int16_t peek_l = 0, peek_r = 0;
        if (peek_avail > 0) {
            int ri = shm->ring_read;
            peek_l = shm->audio_ring[ri * 2 + 0];
            peek_r = shm->audio_ring[ri * 2 + 1];
        }
        vlog("[render] #%d frames=%d ring=%d ur=%d alive=%d prod=%lld peek=%d/%d",
             shm->render_count, frames, audio_ring_available(shm), shm->underrun_count,
             shm->child_alive, (long long)shm->child_samples_produced, peek_l, peek_r);
    }

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
    for (int i = 0; i < to_read; i += 2) {
        if (out_idx >= frames) break;
        /* Average two consecutive samples for 2:1 decimation */
        int32_t l = (int32_t)shm->audio_ring[rd * 2 + 0];
        int32_t r = (int32_t)shm->audio_ring[rd * 2 + 1];
        rd = (rd + 1) % AUDIO_RING_SIZE;
        l += (int32_t)shm->audio_ring[rd * 2 + 0];
        r += (int32_t)shm->audio_ring[rd * 2 + 1];
        rd = (rd + 1) % AUDIO_RING_SIZE;
        out[out_idx * 2 + 0] = (int16_t)(l / 2);
        out[out_idx * 2 + 1] = (int16_t)(r / 2);
        out_idx++;
    }
    shm->ring_read = rd;

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
