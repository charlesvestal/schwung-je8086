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

static FILE *g_vlog = nullptr;
static void vlog(const char *fmt, ...) {
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

static void child_process_midi(jp8000_shm_t *shm, jeLib::Device &device) {
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

        /* Send MIDI to JE device */
        std::vector<synthLib::SMidiEvent> midiIn;
        if (len == 1)
            midiIn.emplace_back(synthLib::MidiEventSource::Host, msg[0], 0, 0);
        else if (len == 2)
            midiIn.emplace_back(synthLib::MidiEventSource::Host, msg[0], msg[1], 0);
        else
            midiIn.emplace_back(synthLib::MidiEventSource::Host, msg[0], msg[1], msg[2]);
        std::vector<synthLib::SMidiEvent> midiOut;
        device.process({}, {}, 0, midiIn, midiOut);
    }
}

static void child_main(jp8000_shm_t *shm) {
    g_child_shm = shm;
    signal(SIGSEGV, child_crash_handler);
    signal(SIGBUS, child_crash_handler);
    signal(SIGABRT, child_crash_handler);
    g_vlog = nullptr; /* reopen in child */

    vlog("[child] started, pid=%d", (int)getpid());

    /* Pin to core 3 */
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
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

    /* 2. Create JE device */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Creating device...");
    synthLib::DeviceCreateParams params;
    params.romData = rom.getData();
    params.romName = rom.getName();
    params.hostSamplerate = JE_INTERNAL_RATE;
    params.preferredSamplerate = JE_INTERNAL_RATE;
    params.homePath = std::string(roms_dir);

    jeLib::Device device(params);
    if (!device.isValid()) {
        snprintf((char*)shm->load_error, sizeof(shm->load_error), "Device creation failed");
        shm->initialized = 1; shm->loading_complete = 1;
        vlog("[child] device creation failed");
        return;
    }
    device.setMasterVolume(7.0f);
    vlog("[child] device created, clock=%llu Hz", (unsigned long long)device.getDspClockHz());

    /* 3. Boot: run until LCD shows PERFORM */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Booting DSP...");
    constexpr size_t blocksize = JE_BLOCK_SIZE;
    std::array<std::vector<float>, 2> outBufs;
    synthLib::TAudioInputs inputs;
    synthLib::TAudioOutputs outputs;
    for (size_t i = 0; i < outBufs.size(); ++i) {
        outBufs[i].resize(blocksize);
        outputs[i] = outBufs[i].data();
    }

    std::vector<synthLib::SMidiEvent> midiIn, midiOut;
    jeLib::SysexRemoteControl sysexRemote;
    bool bootFinished = false;

    sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& _lcdContent) {
        char lcd[41]{0};
        for (size_t i = 0; i < _lcdContent.size(); ++i)
            lcd[i] = _lcdContent[i] >= ' ' ? static_cast<char>(_lcdContent[i]) : ' ';
        if (std::string(lcd).find("PERFORM") != std::string::npos)
            bootFinished = true;
    });

    int bootBlocks = 0;
    while (!bootFinished && bootBlocks < 100000) {
        device.process(inputs, outputs, blocksize, midiIn, midiOut);
        for (const auto& e : midiOut)
            sysexRemote.receive(e);
        midiOut.clear();
        bootBlocks++;
    }
    vlog("[child] boot complete after %d blocks", bootBlocks);
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Forking ASIC child...");

    /* 4. Set up fork-parallel mode */
    /* Install GRAM produce callback BEFORE fork so both processes have it */
    jeLib::devices::g_je_parallel_mode = 1;
    jeLib::devices::g_je_gram_produce = [shm](const int32_t *gram) {
        /* Spin-wait if ring full */
        while (gram_ring_available(shm) >= JE_GRAM_RING_CAP - 1) {
            if (shm->child_shutdown) return;
            __asm__ volatile("yield" ::: "memory");
        }
        int wi = shm->gram_write & JE_GRAM_RING_MASK;
        for (int k = 0; k < JE_GRAM_COUNT; k++)
            shm->gram_ring[wi].gram[k] = gram[k];
        __asm__ volatile("dmb ishst" ::: "memory");
        shm->gram_write = (shm->gram_write + 1) % (JE_GRAM_RING_CAP * 2);
        shm->parent_samples_produced++;
    };

    /* Fork ASIC child process */
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

        /* Set up GRAM consume callback */
        jeLib::devices::g_je_gram_consume = [shm](int32_t *gram) -> bool {
            while (gram_ring_available(shm) < 1) {
                if (shm->child_shutdown) return false;
                __asm__ volatile("yield" ::: "memory");
            }
            int ri = shm->gram_read & JE_GRAM_RING_MASK;
            __asm__ volatile("dmb ishld" ::: "memory");
            for (int k = 0; k < JE_GRAM_COUNT; k++)
                gram[k] = shm->gram_ring[ri].gram[k];
            shm->gram_read = (shm->gram_read + 1) % (JE_GRAM_RING_CAP * 2);
            return true;
        };

        /* Audio output → write int16_t stereo to audio ring */
        device.getJe8086().getAsics().setPostSample([shm](int32_t left, int32_t right) {
            if (audio_ring_free(shm) < 1) return; /* drop if full */
            /* JE output is 24-bit signed in int32_t, scale to int16_t */
            int32_t l = (left >> 8);
            int32_t r = (right >> 8);
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            int wr = shm->ring_write;
            shm->audio_ring[wr * 2 + 0] = (int16_t)l;
            shm->audio_ring[wr * 2 + 1] = (int16_t)r;
            shm->ring_write = (wr + 1) % AUDIO_RING_SIZE;
            shm->child_samples_produced++;
        });

        /* Drive ASIC2+3 directly */
        auto& asics = device.getJe8086().getAsics();
        while (!shm->child_shutdown) {
            if (!asics.processSampleAsic23()) break;
        }
        _exit(0);
    }

    /* === PARENT DSP: runs H8S + ASIC0+1 === */
    vlog("[child] ASIC fork done, asic_pid=%d", (int)asic_pid);

    /* Pre-fill: run a few blocks to populate the audio ring */
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Pre-filling audio...");
    for (int i = 0; i < 16; i++) {
        device.process(inputs, outputs, blocksize, midiIn, midiOut);
        midiOut.clear();
    }
    vlog("[child] pre-fill done, audio_ring=%d", audio_ring_available(shm));

    /* 5. Signal ready */
    shm->initialized = 1;
    shm->loading_complete = 1;
    shm->child_ready = 1;
    snprintf((char*)shm->loading_status, sizeof(shm->loading_status), "Ready");
    vlog("[child] READY");

    /* 6. Main DSP loop — parent runs H8S + ASIC0+1 */
    /* The JE device runs at 88200 Hz internally. We produce 88200 samples/sec
     * of GRAM data. The ASIC child runs at the same rate and produces 88200
     * int16_t stereo pairs per second. The parent's render_block reads these
     * at 44100 Hz, decimating 2:1. */
    while (!shm->child_shutdown) {
        child_process_midi(shm, device);

        /* Throttle: don't overfill */
        if (audio_ring_available(shm) >= AUDIO_RING_SIZE / 2) {
            usleep(500);
            continue;
        }

        /* Run one block of audio through H8S + ASIC0+1 */
        device.process(inputs, outputs, blocksize, midiIn, midiOut);
        midiOut.clear();
        shm->child_alive++;
    }

    /* Cleanup: kill ASIC child */
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
    fork_and_wait_child(inst);
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

    /* The audio ring contains samples at 88200 Hz.
     * Output is 44100 Hz. Decimate 2:1 with simple averaging. */
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
