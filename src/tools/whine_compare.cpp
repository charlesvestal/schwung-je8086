/*
 * whine_compare — JP-8000 serial vs fork-parallel diagnostic harness.
 *
 * Usage:
 *   whine_compare serial <rom_dir> <snap_file> <out_dir> [samples=441000]
 *   whine_compare fork   <rom_dir> <snap_file> <out_dir> [samples=441000]
 *   whine_compare diff   <out_dir>
 *
 * Each phase is a separate invocation for clean process state.
 *
 * serial: loads snapshot, plays sustained C4, runs mode 0 for N samples,
 *         dumps WAV + per-sample tap binaries (audio, asic1→asic2 GRAM,
 *         H8S writes/reads to ASIC2/3).
 *
 * fork:   same setup, but uses the plugin's fork-parallel topology
 *         (parent runs H8S + asic0/1 via Je8086::step in mode 1;
 *          child runs asic2/3 via processSampleAsic23, no H8S).
 *         Both processes record taps; outputs include parent and
 *         child sides separately so we can disambiguate ring transport
 *         issues from compute divergences.
 *
 * diff:   reads both phases' outputs, computes:
 *           - first audio sample where serial and fork diverge
 *           - peak and RMS audio diff
 *           - GRAM tap diff: serial[N] vs fork_parent[N], parent[N] vs child[N-offset]
 *           - uC write/read tap diff: count, first divergence
 *           - "whine score": FFT energy above the serial-mode noise floor
 *         Outputs: diff.csv + a stdout summary.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cerrno>
#include <chrono>
#include <vector>
#include <array>
#include <string>
#include <atomic>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "baseLib/os.h"
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"

/* ============================================================
 * Constants
 * ============================================================ */
constexpr int    SAMPLE_RATE_HZ      = 88200;
constexpr int    DEFAULT_SAMPLES     = 88200 * 5;   /* 5 s */
constexpr int    NOTE_PITCH          = 60;          /* C4 */
constexpr int    NOTE_VELOCITY       = 100;
constexpr int    NOTE_ON_AT_SAMPLE   = 0;
constexpr int    NOTE_OFF_AT_SAMPLE  = INT32_MAX;   /* sustain whole capture */
constexpr int    PARENT_CORE         = 2;
constexpr int    CHILD_CORE          = 3;

constexpr int    GRAM_COUNT          = 6;

/* ============================================================
 * Tap file formats (raw little-endian binary)
 * ============================================================ */
#pragma pack(push, 1)
struct gram_entry_t {
    uint64_t sample_idx;
    int32_t  gram[GRAM_COUNT];
};
struct ucio_entry_t {     /* used for both write and read taps */
    uint64_t sample_idx;
    uint32_t addr;
    uint8_t  asic;
    uint8_t  val;
    uint16_t _pad;
};
#pragma pack(pop)

/* ============================================================
 * WAV writer (16-bit PCM stereo)
 * ============================================================ */
struct wav_writer_t {
    FILE *f;
    uint32_t samples_written;
    int sample_rate;
};

static bool wav_open(wav_writer_t *w, const char *path, int sample_rate) {
    w->f = fopen(path, "wb");
    if (!w->f) return false;
    w->samples_written = 0;
    w->sample_rate = sample_rate;
    /* Reserve header — patch on close */
    uint8_t header[44] = {0};
    fwrite(header, 1, 44, w->f);
    return true;
}
static void wav_write(wav_writer_t *w, int16_t l, int16_t r) {
    fwrite(&l, sizeof(int16_t), 1, w->f);
    fwrite(&r, sizeof(int16_t), 1, w->f);
    w->samples_written++;
}
static void wav_close(wav_writer_t *w) {
    if (!w->f) return;
    uint32_t data_bytes = w->samples_written * 4;
    uint32_t riff_bytes = 36 + data_bytes;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    h[4]=riff_bytes; h[5]=riff_bytes>>8; h[6]=riff_bytes>>16; h[7]=riff_bytes>>24;
    memcpy(h+8, "WAVEfmt ", 8);
    h[16]=16; h[17]=0; h[18]=0; h[19]=0;            /* fmt chunk size */
    h[20]=1; h[21]=0;                               /* PCM */
    h[22]=2; h[23]=0;                               /* stereo */
    h[24]=w->sample_rate; h[25]=w->sample_rate>>8; h[26]=w->sample_rate>>16; h[27]=w->sample_rate>>24;
    uint32_t byte_rate = w->sample_rate * 4;
    h[28]=byte_rate; h[29]=byte_rate>>8; h[30]=byte_rate>>16; h[31]=byte_rate>>24;
    h[32]=4; h[33]=0;                               /* block align */
    h[34]=16; h[35]=0;                              /* bits per sample */
    memcpy(h+36, "data", 4);
    h[40]=data_bytes; h[41]=data_bytes>>8; h[42]=data_bytes>>16; h[43]=data_bytes>>24;
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, 44, w->f);
    fclose(w->f);
    w->f = nullptr;
}

static int16_t clip16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* ============================================================
 * Pin to core
 * ============================================================ */
static void pin_core(int core) {
#ifdef __linux__
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    sched_setaffinity(0, sizeof(cs), &cs);
#else
    (void)core;
#endif
}

/* ============================================================
 * Helper: load ROM + Je8086 + snapshot. Returns nullptr on failure.
 * ============================================================ */
static jeLib::Je8086* boot_je(const char *rom_dir, const char *snap_file) {
    char abspath[4096];
    std::string resolvedDir = realpath(rom_dir, abspath) ? abspath : rom_dir;
    (void)chdir(rom_dir);
    synthLib::RomLoader::addSearchPath(resolvedDir);

    auto rom = jeLib::RomLoader::findROM();
    if (!rom.isValid()) {
        fprintf(stderr, "[boot] no valid ROM at %s\n", rom_dir);
        return nullptr;
    }
    fprintf(stderr, "[boot] rom=%s\n", rom.getName().c_str());

    std::string ramFile = resolvedDir + "/ram_dump.bin";
    auto *je = new jeLib::Je8086(rom.getData(), ramFile);
    if (je->hasDoneFactoryReset()) {
        delete je;
        je = new jeLib::Je8086(rom.getData(), ramFile);
    }

    if (!je->loadSnapshot(snap_file)) {
        fprintf(stderr, "[boot] failed to load snapshot %s\n", snap_file);
        delete je;
        return nullptr;
    }
    fprintf(stderr, "[boot] snapshot loaded from %s\n", snap_file);
    return je;
}

/* ============================================================
 * Phase 1: Serial mode (ground truth)
 * ============================================================ */
static int run_serial(const char *rom_dir, const char *snap_file,
                      const char *out_dir, int samples_to_capture) {
    pin_core(PARENT_CORE);
    baseLib::setFlushDenormalsToZero();

    auto *je = boot_je(rom_dir, snap_file);
    if (!je) return 1;

    /* Output files */
    char path[1024];
    snprintf(path, sizeof(path), "%s/serial.wav", out_dir);
    wav_writer_t wav;
    if (!wav_open(&wav, path, SAMPLE_RATE_HZ)) {
        fprintf(stderr, "[serial] cannot open %s\n", path);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/serial_gram.bin", out_dir);
    FILE *fg = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/serial_ucw.bin", out_dir);
    FILE *fw = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/serial_ucr.bin", out_dir);
    FILE *fr = fopen(path, "wb");
    if (!fg || !fw || !fr) {
        fprintf(stderr, "[serial] cannot open tap files\n");
        return 1;
    }

    /* Sample counter shared across taps */
    int64_t sample_idx = 0;

    /* Install taps */
    jeLib::devices::g_je_tap_gram = [&](const int32_t *gram6) {
        gram_entry_t e;
        e.sample_idx = (uint64_t)sample_idx;
        memcpy(e.gram, gram6, sizeof(e.gram));
        fwrite(&e, sizeof(e), 1, fg);
    };
    jeLib::devices::g_je_tap_uc_write = [&](int asic, uint32_t addr, uint8_t val) {
        if (asic != 2 && asic != 3) return;
        ucio_entry_t e{};
        e.sample_idx = (uint64_t)sample_idx;
        e.addr = addr;
        e.asic = (uint8_t)asic;
        e.val = val;
        fwrite(&e, sizeof(e), 1, fw);
    };
    jeLib::devices::g_je_tap_uc_read = [&](int asic, uint32_t addr, uint8_t val) {
        if (asic != 2 && asic != 3) return;
        ucio_entry_t e{};
        e.sample_idx = (uint64_t)sample_idx;
        e.addr = addr;
        e.asic = (uint8_t)asic;
        e.val = val;
        fwrite(&e, sizeof(e), 1, fr);
    };

    /* Install postSample: write WAV, increment counter, send MIDI at boundaries */
    bool note_on_sent = false;
    je->getAsics().setPostSample([&](int32_t l, int32_t r) {
        wav_write(&wav, clip16(l), clip16(r));
        sample_idx++;
    });

    /* Mode 0 = serial */
    jeLib::devices::g_je_parallel_mode = 0;

    fprintf(stderr, "[serial] running %d samples (~%.2fs at %d Hz)\n",
            samples_to_capture, samples_to_capture / (double)SAMPLE_RATE_HZ, SAMPLE_RATE_HZ);

    auto t0 = std::chrono::high_resolution_clock::now();
    while ((int)sample_idx < samples_to_capture) {
        /* Send note-on once, at first iteration */
        if (!note_on_sent && (int)sample_idx >= NOTE_ON_AT_SAMPLE) {
            synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host, 0x90, NOTE_PITCH, NOTE_VELOCITY);
            je->addMidiEvent(ev);
            note_on_sent = true;
            fprintf(stderr, "[serial] note-on sent at sample %lld\n", (long long)sample_idx);
        }
        je->step();
        if (!je->getSampleBuffer().empty()) je->clearSampleBuffer();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double rt = (samples_to_capture / (double)SAMPLE_RATE_HZ) / elapsed;
    fprintf(stderr, "[serial] %lld samples in %.2fs => %.2fx RT\n",
            (long long)sample_idx, elapsed, rt);

    /* Clear taps before close */
    jeLib::devices::g_je_tap_gram = nullptr;
    jeLib::devices::g_je_tap_uc_write = nullptr;
    jeLib::devices::g_je_tap_uc_read = nullptr;

    wav_close(&wav);
    fclose(fg); fclose(fw); fclose(fr);
    delete je;
    return 0;
}

/* ============================================================
 * Phase 2: Fork-parallel (plugin path)
 *   Parent: H8S + asic0/1 via Je8086::step in mode 1
 *   Child:  asic2/3 via processSampleAsic23 in tight loop (no H8S)
 * ============================================================ */
struct fork_shm_t {
    /* GRAM ring: parent → child */
    static constexpr int GRAM_CAP = 1024;
    struct gram_cell { int32_t v[GRAM_COUNT]; };
    gram_cell gram_ring[GRAM_CAP];
    volatile uint64_t gram_w;
    volatile uint64_t gram_r;

    /* Audio ring: child → parent (consumed only for WAV writing) */
    static constexpr int AUDIO_CAP = 8192;
    int32_t audio_ring[AUDIO_CAP * 2];
    volatile uint64_t audio_w;
    volatile uint64_t audio_r;

    /* uC write ring: parent → child (PRAM forwarding) */
    static constexpr int UCW_CAP = 4096;
    struct uc_cell { uint32_t addr; uint8_t asic; uint8_t val; uint16_t _pad; };
    uc_cell ucw_ring[UCW_CAP];
    volatile uint64_t ucw_w;
    volatile uint64_t ucw_r;

    /* ASIC2/3 readback overlay (child → parent) */
    volatile uint8_t asic2_readback[4];
    volatile uint8_t asic3_readback[4];

    /* Control */
    volatile int child_ready;
    volatile int child_shutdown;
};

#ifdef __aarch64__
#define FENCE_ST() __asm__ volatile("dmb ishst" ::: "memory")
#define FENCE_LD() __asm__ volatile("dmb ishld" ::: "memory")
#else
#define FENCE_ST() __asm__ volatile("" ::: "memory")
#define FENCE_LD() __asm__ volatile("" ::: "memory")
#endif

static int run_fork(const char *rom_dir, const char *snap_file,
                    const char *out_dir, int samples_to_capture) {
    baseLib::setFlushDenormalsToZero();

    /* Shared memory */
    fork_shm_t *shm = (fork_shm_t*)mmap(nullptr, sizeof(*shm),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "[fork] mmap failed: %s\n", strerror(errno));
        return 1;
    }
    memset(shm, 0, sizeof(*shm));

    /* Both parent and child boot from the same snapshot — same starting state.
     * After fork, each process advances its own copy. */
    auto *je = boot_je(rom_dir, snap_file);
    if (!je) { munmap(shm, sizeof(*shm)); return 1; }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[fork] fork failed: %s\n", strerror(errno));
        return 1;
    }

    /* ====================================================
     * ASIC CHILD: runs processSampleAsic23 in tight loop
     * ==================================================== */
    if (pid == 0) {
        pin_core(CHILD_CORE);
        baseLib::setFlushDenormalsToZero();

        /* Tap files (child side) */
        char path[1024];
        snprintf(path, sizeof(path), "%s/fork_child_gram.bin", out_dir);
        FILE *fg = fopen(path, "wb");
        if (!fg) { fprintf(stderr, "[child] cannot open %s\n", path); _exit(1); }

        int64_t child_sample_idx = 0;

        /* GRAM consume */
        jeLib::devices::g_je_gram_consume = [shm](int32_t *gram) -> bool {
            while (shm->gram_w == shm->gram_r) {
                if (shm->child_shutdown) return false;
                usleep(10);
            }
            FENCE_LD();
            uint64_t ri = shm->gram_r % fork_shm_t::GRAM_CAP;
            for (int k = 0; k < GRAM_COUNT; k++)
                gram[k] = shm->gram_ring[ri].v[k];
            shm->gram_r++;
            return true;
        };

        /* Tap GRAM on child side */
        jeLib::devices::g_je_tap_gram = [&](const int32_t *gram6) {
            gram_entry_t e;
            e.sample_idx = (uint64_t)child_sample_idx;
            memcpy(e.gram, gram6, sizeof(e.gram));
            fwrite(&e, sizeof(e), 1, fg);
        };

        /* postSample → audio ring */
        je->getAsics().setPostSample([shm, &child_sample_idx](int32_t l, int32_t r) {
            while (shm->audio_w - shm->audio_r >= fork_shm_t::AUDIO_CAP) {
                if (shm->child_shutdown) return;
                usleep(10);
            }
            uint64_t wi = shm->audio_w % fork_shm_t::AUDIO_CAP;
            shm->audio_ring[wi * 2 + 0] = l;
            shm->audio_ring[wi * 2 + 1] = r;
            FENCE_ST();
            shm->audio_w++;
            child_sample_idx++;
        });

        /* Run asic2/3 loop */
        auto &asics = je->getAsics();
        while (!shm->child_shutdown) {
            /* Drain uC writes from parent */
            while (shm->ucw_w != shm->ucw_r) {
                FENCE_LD();
                uint64_t ri = shm->ucw_r % fork_shm_t::UCW_CAP;
                auto &c = shm->ucw_ring[ri];
                asics.applyUcWrite(c.asic, c.addr, c.val);
                shm->ucw_r++;
            }

            if (!asics.processSampleAsic23()) break;

            asics.getAsic23Readback(
                (uint8_t*)shm->asic2_readback,
                (uint8_t*)shm->asic3_readback);
        }

        jeLib::devices::g_je_tap_gram = nullptr;
        fclose(fg);
        delete je;
        _exit(0);
    }

    /* ====================================================
     * PARENT: H8S + asic0/1 via Je8086::step, mode 1
     * ==================================================== */
    pin_core(PARENT_CORE);
    baseLib::setFlushDenormalsToZero();

    /* Open parent-side tap files */
    char path[1024];
    snprintf(path, sizeof(path), "%s/fork.wav", out_dir);
    wav_writer_t wav;
    if (!wav_open(&wav, path, SAMPLE_RATE_HZ)) {
        fprintf(stderr, "[fork-parent] cannot open %s\n", path);
        shm->child_shutdown = 1;
        return 1;
    }
    snprintf(path, sizeof(path), "%s/fork_parent_gram.bin", out_dir);
    FILE *fg = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/fork_ucw.bin", out_dir);
    FILE *fw = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/fork_ucr.bin", out_dir);
    FILE *fr = fopen(path, "wb");

    /* Parent-side sample counter: increments on parent's mode-1 postSample(0,0) call */
    int64_t parent_sample_idx = 0;

    /* Audio-out counter (consumes child ring, writes WAV) */
    int64_t audio_out_count = 0;

    jeLib::devices::g_je_tap_gram = [&](const int32_t *gram6) {
        gram_entry_t e;
        e.sample_idx = (uint64_t)parent_sample_idx;
        memcpy(e.gram, gram6, sizeof(e.gram));
        fwrite(&e, sizeof(e), 1, fg);
    };
    jeLib::devices::g_je_tap_uc_write = [&](int asic, uint32_t addr, uint8_t val) {
        if (asic != 2 && asic != 3) return;
        ucio_entry_t e{};
        e.sample_idx = (uint64_t)parent_sample_idx;
        e.addr = addr;
        e.asic = (uint8_t)asic;
        e.val = val;
        fwrite(&e, sizeof(e), 1, fw);
    };
    jeLib::devices::g_je_tap_uc_read = [&](int asic, uint32_t addr, uint8_t val) {
        if (asic != 2 && asic != 3) return;
        ucio_entry_t e{};
        e.sample_idx = (uint64_t)parent_sample_idx;
        e.addr = addr;
        e.asic = (uint8_t)asic;
        e.val = val;
        fwrite(&e, sizeof(e), 1, fr);
    };

    /* GRAM produce: push to ring */
    jeLib::devices::g_je_gram_produce = [shm](const int32_t *gram6) {
        while (shm->gram_w - shm->gram_r >= fork_shm_t::GRAM_CAP) {
            if (shm->child_shutdown) return;
            usleep(10);
        }
        uint64_t wi = shm->gram_w % fork_shm_t::GRAM_CAP;
        for (int k = 0; k < GRAM_COUNT; k++)
            shm->gram_ring[wi].v[k] = gram6[k];
        FENCE_ST();
        shm->gram_w++;
    };
    /* uC write forwarding to child */
    jeLib::devices::g_je_uc_write_forward = [shm](int asic, uint32_t addr, uint8_t val) {
        if (shm->ucw_w - shm->ucw_r >= fork_shm_t::UCW_CAP) return; /* drop */
        uint64_t wi = shm->ucw_w % fork_shm_t::UCW_CAP;
        auto &c = shm->ucw_ring[wi];
        c.asic = (uint8_t)asic; c.addr = addr; c.val = val;
        FENCE_ST();
        shm->ucw_w++;
    };

    /* Parent postSample dummy: count parent samples, also overlay readback */
    je->getAsics().setPostSample([&](int32_t /*l*/, int32_t /*r*/) {
        je->getAsics().setAsic23Readback(
            (const uint8_t*)shm->asic2_readback,
            (const uint8_t*)shm->asic3_readback);
        parent_sample_idx++;
    });

    jeLib::devices::g_je_parallel_mode = 1;

    /* Note-on at parent sample 0 */
    bool note_on_sent = false;

    /* Audio drain helper: pulls samples from child ring into WAV */
    auto drain_audio = [&]() {
        while (shm->audio_w != shm->audio_r && audio_out_count < samples_to_capture) {
            FENCE_LD();
            uint64_t ri = shm->audio_r % fork_shm_t::AUDIO_CAP;
            int32_t l = shm->audio_ring[ri * 2 + 0];
            int32_t r = shm->audio_ring[ri * 2 + 1];
            shm->audio_r++;
            wav_write(&wav, clip16(l), clip16(r));
            audio_out_count++;
        }
    };

    fprintf(stderr, "[fork-parent] running %d samples\n", samples_to_capture);
    auto t0 = std::chrono::high_resolution_clock::now();

    while (audio_out_count < samples_to_capture) {
        if (!note_on_sent && parent_sample_idx >= NOTE_ON_AT_SAMPLE) {
            synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host, 0x90, NOTE_PITCH, NOTE_VELOCITY);
            je->addMidiEvent(ev);
            note_on_sent = true;
            fprintf(stderr, "[fork-parent] note-on at parent sample %lld\n",
                    (long long)parent_sample_idx);
        }
        je->step();
        if (!je->getSampleBuffer().empty()) je->clearSampleBuffer();
        drain_audio();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    fprintf(stderr, "[fork-parent] %lld parent samples, %lld audio out in %.2fs (%.2fx RT)\n",
            (long long)parent_sample_idx, (long long)audio_out_count, elapsed,
            (samples_to_capture / (double)SAMPLE_RATE_HZ) / elapsed);

    /* Shutdown child */
    shm->child_shutdown = 1;
    for (int i = 0; i < 30; i++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) break;
        usleep(100000);
    }
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);

    /* Clear taps */
    jeLib::devices::g_je_tap_gram = nullptr;
    jeLib::devices::g_je_tap_uc_write = nullptr;
    jeLib::devices::g_je_tap_uc_read = nullptr;
    jeLib::devices::g_je_gram_produce = nullptr;
    jeLib::devices::g_je_uc_write_forward = nullptr;

    wav_close(&wav);
    fclose(fg); fclose(fw); fclose(fr);
    delete je;
    munmap(shm, sizeof(*shm));
    return 0;
}

/* ============================================================
 * Phase 3: Diff serial vs fork
 * ============================================================ */
static std::vector<gram_entry_t> load_gram(const char *path) {
    std::vector<gram_entry_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    gram_entry_t e;
    while (fread(&e, sizeof(e), 1, f) == 1) v.push_back(e);
    fclose(f);
    return v;
}
static std::vector<ucio_entry_t> load_ucio(const char *path) {
    std::vector<ucio_entry_t> v;
    FILE *f = fopen(path, "rb");
    if (!f) return v;
    ucio_entry_t e;
    while (fread(&e, sizeof(e), 1, f) == 1) v.push_back(e);
    fclose(f);
    return v;
}

struct wav_data_t {
    int sample_rate;
    int channels;
    std::vector<int16_t> samples; /* interleaved */
};
static bool load_wav(const char *path, wav_data_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[44];
    if (fread(h, 1, 44, f) != 44) { fclose(f); return false; }
    out->channels = h[22] | (h[23] << 8);
    out->sample_rate = h[24] | (h[25]<<8) | (h[26]<<16) | (h[27]<<24);
    uint32_t data_bytes = h[40] | (h[41]<<8) | (h[42]<<16) | (h[43]<<24);
    out->samples.resize(data_bytes / 2);
    fread(out->samples.data(), 1, data_bytes, f);
    fclose(f);
    return true;
}

static int run_diff(const char *out_dir) {
    char path[1024];

    /* Load audio */
    snprintf(path, sizeof(path), "%s/serial.wav", out_dir);
    wav_data_t serial_wav;
    if (!load_wav(path, &serial_wav)) {
        fprintf(stderr, "[diff] cannot load %s\n", path);
        return 1;
    }
    snprintf(path, sizeof(path), "%s/fork.wav", out_dir);
    wav_data_t fork_wav;
    if (!load_wav(path, &fork_wav)) {
        fprintf(stderr, "[diff] cannot load %s\n", path);
        return 1;
    }

    size_t N = std::min(serial_wav.samples.size(), fork_wav.samples.size()) / 2;
    fprintf(stderr, "[diff] aligned %zu stereo samples (%.2fs)\n", N, N/(double)SAMPLE_RATE_HZ);

    /* Sample-by-sample audio diff. We try offsets [-32, +32] frames to find best alignment. */
    int best_offset = 0;
    int64_t best_diff_sum = INT64_MAX;
    for (int off = -32; off <= 32; off++) {
        int64_t s = 0;
        int counted = 0;
        for (size_t i = std::max(0, off); i < N && (int)i - off < (int)N; i++) {
            int idx_s = (int)i;
            int idx_f = (int)i - off;
            if (idx_s < 0 || idx_f < 0) continue;
            if (idx_s >= (int)N || idx_f >= (int)N) continue;
            int dl = serial_wav.samples[idx_s*2] - fork_wav.samples[idx_f*2];
            int dr = serial_wav.samples[idx_s*2+1] - fork_wav.samples[idx_f*2+1];
            s += (int64_t)dl*dl + (int64_t)dr*dr;
            counted++;
            if (counted >= 88200) break; /* first second */
        }
        if (s < best_diff_sum) { best_diff_sum = s; best_offset = off; }
    }
    fprintf(stderr, "[diff] best audio offset: %d frames (sum-sq-err over first 1s: %lld)\n",
            best_offset, (long long)best_diff_sum);

    /* First-divergence search (per-sample abs diff above threshold) */
    int first_div = -1;
    int64_t max_abs_diff = 0;
    int64_t sum_abs_diff = 0;
    int sum_count = 0;
    for (size_t i = 0; i < N; i++) {
        int idx_s = (int)i;
        int idx_f = (int)i - best_offset;
        if (idx_s < 0 || idx_f < 0) continue;
        if (idx_s >= (int)N || idx_f >= (int)N) continue;
        int dl = serial_wav.samples[idx_s*2] - fork_wav.samples[idx_f*2];
        int dr = serial_wav.samples[idx_s*2+1] - fork_wav.samples[idx_f*2+1];
        int64_t ad = (int64_t)std::abs(dl) + std::abs(dr);
        sum_abs_diff += ad;
        sum_count++;
        if (ad > max_abs_diff) max_abs_diff = ad;
        if (first_div < 0 && ad > 100) first_div = idx_s;
    }
    double mean_abs = sum_count ? (double)sum_abs_diff / sum_count : 0.0;
    fprintf(stderr, "[diff] AUDIO: first_div_sample=%d max_abs=%lld mean_abs=%.2f\n",
            first_div, (long long)max_abs_diff, mean_abs);

    /* === GRAM diffs === */
    auto sg = load_gram((std::string(out_dir) + "/serial_gram.bin").c_str());
    auto pg = load_gram((std::string(out_dir) + "/fork_parent_gram.bin").c_str());
    auto cg = load_gram((std::string(out_dir) + "/fork_child_gram.bin").c_str());
    fprintf(stderr, "[diff] GRAM tap counts: serial=%zu parent=%zu child=%zu\n",
            sg.size(), pg.size(), cg.size());

    /* Parent vs serial: aligned by sample_idx (both should produce one per sample) */
    int gram_first_div_sp = -1;
    int gram_diff_count_sp = 0;
    int n_sp = std::min(sg.size(), pg.size());
    for (int i = 0; i < n_sp; i++) {
        bool diff = false;
        for (int k = 0; k < GRAM_COUNT; k++)
            if (sg[i].gram[k] != pg[i].gram[k]) { diff = true; break; }
        if (diff) {
            if (gram_first_div_sp < 0) gram_first_div_sp = i;
            gram_diff_count_sp++;
        }
    }
    fprintf(stderr, "[diff] GRAM serial vs parent: first_div=%d, diff_samples=%d/%d\n",
            gram_first_div_sp, gram_diff_count_sp, n_sp);

    /* Parent vs child: ring transport check. Should match perfectly with constant offset. */
    int gram_first_div_pc = -1;
    int gram_diff_count_pc = 0;
    int n_pc = std::min(pg.size(), cg.size());
    for (int i = 0; i < n_pc; i++) {
        bool diff = false;
        for (int k = 0; k < GRAM_COUNT; k++)
            if (pg[i].gram[k] != cg[i].gram[k]) { diff = true; break; }
        if (diff) {
            if (gram_first_div_pc < 0) gram_first_div_pc = i;
            gram_diff_count_pc++;
        }
    }
    fprintf(stderr, "[diff] GRAM parent vs child: first_div=%d, diff_samples=%d/%d\n",
            gram_first_div_pc, gram_diff_count_pc, n_pc);

    /* === uC write/read diffs === */
    auto sw = load_ucio((std::string(out_dir) + "/serial_ucw.bin").c_str());
    auto fw = load_ucio((std::string(out_dir) + "/fork_ucw.bin").c_str());
    auto sr = load_ucio((std::string(out_dir) + "/serial_ucr.bin").c_str());
    auto fr = load_ucio((std::string(out_dir) + "/fork_ucr.bin").c_str());
    fprintf(stderr, "[diff] uC writes: serial=%zu fork=%zu\n", sw.size(), fw.size());
    fprintf(stderr, "[diff] uC reads:  serial=%zu fork=%zu\n", sr.size(), fr.size());

    /* Exact event-by-event match (sequence comparison) */
    int ucw_first_div = -1;
    int n_uw = std::min(sw.size(), fw.size());
    for (int i = 0; i < n_uw; i++) {
        if (sw[i].asic != fw[i].asic || sw[i].addr != fw[i].addr || sw[i].val != fw[i].val) {
            ucw_first_div = i; break;
        }
    }
    int ucr_first_div = -1;
    int n_ur = std::min(sr.size(), fr.size());
    for (int i = 0; i < n_ur; i++) {
        if (sr[i].asic != fr[i].asic || sr[i].addr != fr[i].addr || sr[i].val != fr[i].val) {
            ucr_first_div = i; break;
        }
    }
    fprintf(stderr, "[diff] uC write seq first_div=%d (count_diff=%zd)\n",
            ucw_first_div, (ssize_t)(fw.size() - sw.size()));
    fprintf(stderr, "[diff] uC read  seq first_div=%d (count_diff=%zd)\n",
            ucr_first_div, (ssize_t)(fr.size() - sr.size()));

    /* === Summary CSV === */
    snprintf(path, sizeof(path), "%s/diff.csv", out_dir);
    FILE *csv = fopen(path, "w");
    if (csv) {
        fprintf(csv, "metric,value\n");
        fprintf(csv, "audio_samples_aligned,%zu\n", N);
        fprintf(csv, "audio_align_offset_frames,%d\n", best_offset);
        fprintf(csv, "audio_first_divergence_sample,%d\n", first_div);
        fprintf(csv, "audio_max_abs_diff,%lld\n", (long long)max_abs_diff);
        fprintf(csv, "audio_mean_abs_diff,%.2f\n", mean_abs);
        fprintf(csv, "gram_taps_serial,%zu\n", sg.size());
        fprintf(csv, "gram_taps_fork_parent,%zu\n", pg.size());
        fprintf(csv, "gram_taps_fork_child,%zu\n", cg.size());
        fprintf(csv, "gram_serial_vs_parent_first_div,%d\n", gram_first_div_sp);
        fprintf(csv, "gram_serial_vs_parent_diff_count,%d\n", gram_diff_count_sp);
        fprintf(csv, "gram_parent_vs_child_first_div,%d\n", gram_first_div_pc);
        fprintf(csv, "gram_parent_vs_child_diff_count,%d\n", gram_diff_count_pc);
        fprintf(csv, "uc_writes_serial,%zu\n", sw.size());
        fprintf(csv, "uc_writes_fork,%zu\n", fw.size());
        fprintf(csv, "uc_writes_first_div_seq_idx,%d\n", ucw_first_div);
        fprintf(csv, "uc_reads_serial,%zu\n", sr.size());
        fprintf(csv, "uc_reads_fork,%zu\n", fr.size());
        fprintf(csv, "uc_reads_first_div_seq_idx,%d\n", ucr_first_div);
        fclose(csv);
        fprintf(stderr, "[diff] wrote %s\n", path);
    }

    /* === Per-sample diff dump (truncated) === */
    snprintf(path, sizeof(path), "%s/diff_per_sample.csv", out_dir);
    FILE *psv = fopen(path, "w");
    if (psv) {
        fprintf(psv, "sample,serial_L,serial_R,fork_L,fork_R,abs_diff_L,abs_diff_R\n");
        int max_dump = std::min((int)N, 5000); /* first 5000 samples to keep file small */
        for (int i = 0; i < max_dump; i++) {
            int idx_f = i - best_offset;
            if (idx_f < 0 || idx_f >= (int)N) continue;
            int sl = serial_wav.samples[i*2];
            int sr_ = serial_wav.samples[i*2+1];
            int fl = fork_wav.samples[idx_f*2];
            int fr_ = fork_wav.samples[idx_f*2+1];
            fprintf(psv, "%d,%d,%d,%d,%d,%d,%d\n",
                    i, sl, sr_, fl, fr_, std::abs(sl-fl), std::abs(sr_-fr_));
        }
        fclose(psv);
        fprintf(stderr, "[diff] wrote per-sample dump (first %d samples)\n",
                std::min((int)N, 5000));
    }

    return 0;
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s serial|fork|diff ...\n"
            "  serial <rom_dir> <snap> <out_dir> [samples]\n"
            "  fork   <rom_dir> <snap> <out_dir> [samples]\n"
            "  diff   <out_dir>\n", argv[0]);
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "serial" || cmd == "fork") {
        if (argc < 5) { fprintf(stderr, "missing args\n"); return 1; }
        int samples = (argc >= 6) ? atoi(argv[5]) : DEFAULT_SAMPLES;
        if (cmd == "serial")
            return run_serial(argv[2], argv[3], argv[4], samples);
        else
            return run_fork(argv[2], argv[3], argv[4], samples);
    }
    if (cmd == "diff") {
        if (argc < 3) { fprintf(stderr, "missing out_dir\n"); return 1; }
        return run_diff(argv[2]);
    }
    fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}
