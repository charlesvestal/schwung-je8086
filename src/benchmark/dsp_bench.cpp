/*
 * Headless DSP benchmark for JP-8000 (JE-8086) emulation.
 * Cross-compile for Move, scp to device, run with ROM path.
 *
 * Targets:
 *   bench_je      <rom_dir>   — serial (all 4 ASICs on 1 core)
 *   bench_je_fork <rom_dir>   — fork-parallel (ASIC0+1 and ASIC2+3 on 2 cores)
 */

#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <time.h>
#include <fstream>
#include <vector>
#include <string>

#ifdef __linux__
#include <sched.h>
#endif
#include <unistd.h>

#include "baseLib/os.h"
#include "synthLib/audioTypes.h"

#ifdef BENCH_JE
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"
#endif

/* Optional raw dump (2nd arg): serial = float32 L/R interleaved, fork = je_audio_sample_t records. */
static FILE* g_dump = nullptr;

#if defined(BENCH_JE) || defined(BENCH_JE_FORK) || defined(BENCH_JE_THREAD)
#include "synthLib/midiTypes.h"
#include <array>
#include <string>
#include <vector>
/* ---- Shared test material for both benches ----
 * Boot LCD is kept so the selected patch can be reported. JE_PATCH="msb,lsb,pc"
 * sends a bank select + program change before the phrase (JP-8000: MSB 81,
 * LSB 0 = user, LSB 1 = preset; pc 0..127 = A11..B88). The phrase is
 * C4 E4 G4 (2 s each, released) then C5 held 2 s, then 2 s of tail. */
static std::string g_lastLcd;
static FILE* g_wtrace = nullptr;   /* JE_WTRACE=path: every H8S->ASIC write in the timed region */
static long g_wtraceSample = 0;
static void je_bench_lcd(const std::array<char, 40>& c) {
    char lcd[41]{0};
    for (size_t i = 0; i < c.size(); ++i) lcd[i] = c[i] >= ' ' ? static_cast<char>(c[i]) : ' ';
    g_lastLcd = lcd;
}
static void je_bench_patch_select(std::vector<synthLib::SMidiEvent>& midiIn) {
    const char* e = getenv("JE_PATCH");
    if (!e) return;
    int msb = 81, lsb = 1, pc = 0, ch = 0;
    if (sscanf(e, "%d,%d,%d,%d", &msb, &lsb, &pc, &ch) < 3) { fprintf(stderr, "JE_PATCH wants msb,lsb,pc[,ch]\n"); return; }
    midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xB0 | ch, 0, msb);
    midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xB0 | ch, 32, lsb);
    midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xC0 | ch, pc, 0);
    printf("Patch select: bank %d/%d program %d ch %d\n", msb, lsb, pc, ch + 1);
}
static void je_bench_phrase(int block, std::vector<synthLib::SMidiEvent>& midiIn) {
    constexpr int perSec = 88200 / 128;   // 689 blocks
    /* JE_PHRASE=chord: an eight-note chord (full polyphony) held for 6 s,
     * then a 4 s release tail. Default: a four-note arpeggio, last note held. */
    const char* ph = getenv("JE_PHRASE");
    if (ph && strcmp(ph, "chord") == 0) {
        static const int chord[8] = {48, 52, 55, 59, 62, 66, 69, 72};
        for (int n = 0; n < 8; n++) {
            if (block == perSec / 4 + n * 12) midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x90, chord[n], 100);
            if (block == 6 * perSec) midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x80, chord[n], 0);
        }
        return;
    }
    static const int notes[4] = {60, 64, 67, 72};
    for (int n = 0; n < 4; n++) {
        const int on = n * 2 * perSec;
        const int off = on + 2 * perSec - (n == 3 ? 0 : perSec / 8);
        if (block == on) midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x90, notes[n], 100);
        if (block == off) midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x80, notes[n], 0);
    }
}
#endif

#if defined(BENCH_JE_FORK) || defined(BENCH_JE_THREAD)
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"
#include "je_fork_shm.h"
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#endif

#ifdef BENCH_JE
static int bench_je(const char* romDir) {
    char abspath[4096];
    std::string resolvedDir;
    if (realpath(romDir, abspath)) {
        resolvedDir = abspath;
        printf("ROM dir (resolved): %s\n", abspath);
    } else {
        resolvedDir = romDir;
        printf("ROM dir: %s\n", romDir);
    }

    (void)chdir(romDir);
    synthLib::RomLoader::addSearchPath(resolvedDir);

    auto rom = jeLib::RomLoader::findROM();
    if (!rom.isValid()) {
        fprintf(stderr, "No valid JP-8000 ROM found at %s\n", romDir);
        return 1;
    }
    printf("ROM: %s (%s)\n", rom.getName().c_str(),
           rom.getDeviceType() == jeLib::DeviceType::Keyboard ? "Keyboard" : "Rack");

    synthLib::DeviceCreateParams params;
    params.romData = rom.getData();
    params.romName = rom.getName();
    params.hostSamplerate = 88200;
    params.preferredSamplerate = 88200;
    {
        // Device writes homePath + "/roms/ram_dump.bin", so homePath is the PARENT of the roms dir.
        std::string rp(resolvedDir);   // absolute: we chdir(romDir) above
        auto pos = rp.rfind("/roms");
        params.homePath = (pos != std::string::npos) ? rp.substr(0, pos) : rp;
    }

    printf("Creating device (includes factory reset if needed)...\n");
    jeLib::Device device(params);

    if (!device.isValid()) {
        fprintf(stderr, "Device creation failed\n");
        return 1;
    }

    device.setMasterVolume(7.0f);

    printf("DSP clock: %llu Hz (%.1f MHz)\n",
           (unsigned long long)device.getDspClockHz(),
           device.getDspClockHz() / 1e6);

    /* Boot: run until LCD shows PERFORM */
    constexpr size_t blocksize = 128;
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
        je_bench_lcd(_lcdContent);
        std::string s(lcd);
        if (s.find("PERFORM") != std::string::npos)
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
    printf("Boot complete after %d blocks.\n", bootBlocks);

    /* Optional patch select, then settle */
    {
        je_bench_patch_select(midiIn);
        for (int i = 0; i < 700; i++) {
            device.process(inputs, outputs, blocksize, midiIn, midiOut);
            midiIn.clear();
            for (const auto& e : midiOut) sysexRemote.receive(e);
            midiOut.clear();
        }
        printf("LCD: [%s]\n", g_lastLcd.c_str());
    }

    /* Benchmark: 10 seconds of audio at 88200 Hz */
    constexpr int BLOCK = 128;
    constexpr int totalFrames = 88200 * 10;
    constexpr int totalBlocks = totalFrames / BLOCK;

    printf("Running %d blocks (%d frames, ~10s at 88200 Hz)...\n", totalBlocks, totalFrames);
    static uint64_t rd[4][4], wr[4], wrMode[4][256];
    static int ifMode[4];
    jeLib::devices::g_je_uc_read_capture = [](int a, uint32_t addr, uint8_t) { rd[a][addr & 3]++; };
    jeLib::devices::g_je_uc_write_capture = [](int a, uint32_t addr, uint8_t v) { wr[a]++; if (addr == 0x2003) ifMode[a] = v; else if ((addr & 3) == 3) wrMode[a][ifMode[a]]++; };
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < totalBlocks; i++) {
        je_bench_phrase(i, midiIn);
        device.process(inputs, outputs, BLOCK, midiIn, midiOut);
        midiIn.clear();
        midiOut.clear();
        if (g_dump) {
            float il[BLOCK * 2];
            for (int f = 0; f < BLOCK; f++) { il[2*f] = outputs[0][f]; il[2*f+1] = outputs[1][f]; }
            fwrite(il, sizeof(float), BLOCK * 2, g_dump);
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    for (int a = 0; a < 4; a++) {
        printf("asic%d: H8S reads/sample byte0..3 = %.3f %.3f %.3f %.3f ; writes/sample %.3f ; word writes by if_mode:", a,
               rd[a][0] / (double)totalFrames, rd[a][1] / (double)totalFrames, rd[a][2] / (double)totalFrames, rd[a][3] / (double)totalFrames, wr[a] / (double)totalFrames);
        for (int m = 0; m < 256; m++) if (wrMode[a][m]) printf(" 0x%02x:%.3f", m, wrMode[a][m] / (double)totalFrames);
        printf("\n");
    }
    jeLib::devices::g_je_uc_read_capture = nullptr; jeLib::devices::g_je_uc_write_capture = nullptr;
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double ratio = 10.0 / elapsed;
    printf("Elapsed: %.2f s for 10s audio => %.2fx real-time\n", elapsed, ratio);

    if (g_dump) { fclose(g_dump); g_dump = nullptr; }
    return 0;
}
#endif


#if defined(BENCH_JE_FORK) || defined(BENCH_JE_THREAD)

/* ---- SPSC ring helpers ---- */
static inline int je_ring_avail(int w, int r) {
    int avail = w - r;
    if (avail < 0) avail += JE_RING_CAPACITY * 2;
    return avail;
}
static inline int je_gram_avail(const je_stage_shm_t *st) { return je_ring_avail(st->gram_write, st->gram_read); }
static inline int je_audio_avail(const je_fork_shm_t *shm) { return je_ring_avail(shm->audio_write, shm->audio_read); }

static inline int64_t je_now_ns() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
/* Spin until pred() is true; charge the spin time (if any) to *acc. */
template <class Pred, class Stop>
static inline bool je_spin_wait(Pred pred, Stop stop, volatile int64_t *acc) {
    if (pred()) return true;
    const int64_t t0 = je_now_ns();
    bool ok = true;
    while (!pred()) {
        if (stop()) { ok = false; break; }
#ifdef __aarch64__
        __asm__ volatile("yield" ::: "memory");
#endif
    }
    *acc += je_now_ns() - t0;
    return ok;
}

static inline void je_pin_core(int core) {
#ifdef __linux__
    if (core < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
#else
    (void)core;
#endif
}

/* Push one handoff into stage `to`'s input ring (waits for space, charging `acc`). */
static inline void je_gram_push(je_stage_shm_t *to, const int32_t *gram, volatile int64_t *acc) {
    je_spin_wait([to] { return je_gram_avail(to) < JE_RING_CAPACITY - 1; },
                 [] { return false; }, acc);
    const int wi = to->gram_write & JE_RING_MASK;
    for (int k = 0; k < JE_GRAM_HANDOFF_COUNT; k++) to->gram_ring[wi].gram[k] = gram[k];
#ifdef __aarch64__
    __asm__ volatile("dmb ishst" ::: "memory");
#endif
    to->gram_write = (to->gram_write + 1) % (JE_RING_CAPACITY * 2);
}

/* ---- Child process: one pipeline stage, ASICs [lo, hi) ---- */
static void je_child_main(je_fork_shm_t *shm, int s, int core, jeLib::Je8086 &je) {
    je_stage_shm_t *st = &shm->stage[s];
    je_stage_shm_t *prev = &shm->stage[s - 1];
    je_stage_shm_t *next = (s + 1 < shm->num_stages) ? &shm->stage[s + 1] : nullptr;
    const int lo = st->lo, hi = st->hi;

    je_pin_core(core);
    baseLib::setFlushDenormalsToZero();
    jeLib::devices::g_je_parallel_mode = 2;
    jeLib::devices::g_je_stage_lo = lo;
    jeLib::devices::g_je_stage_hi = hi;

    /* Input: the lo-1 -> lo handoff, from the previous stage */
    jeLib::devices::g_je_gram_consume = [shm, st](int32_t *gram) -> bool {
        if (!je_spin_wait([st] { return je_gram_avail(st) >= 1; },
                          [shm] { return shm->child_shutdown != 0; },
                          &st->in_wait_ns)) return false;
        const int ri = st->gram_read & JE_RING_MASK;
#ifdef __aarch64__
        __asm__ volatile("dmb ishld" ::: "memory");
#endif
        for (int k = 0; k < JE_GRAM_HANDOFF_COUNT; k++) gram[k] = st->gram_ring[ri].gram[k];
        st->gram_read = (st->gram_read + 1) % (JE_RING_CAPACITY * 2);
        return true;
    };

    /* Output: either the hi-1 -> hi handoff to the next stage, or audio. */
    if (next) {
        jeLib::devices::g_je_gram_produce = [st, next](const int32_t *gram) {
            je_gram_push(next, gram, &st->out_wait_ns);
            st->samples_produced++;
        };
    } else {
#ifdef BENCH_JE_THREAD
        /* Shared MultiAsic: stage 0's dummy postSample would otherwise push
         * into this same audio ring. Stage-scoped hook keeps one producer. */
        jeLib::devices::g_je_stage_audio_out = [shm, st](int32_t left, int32_t right) {
#else
        je.getAsics().setPostSample([shm, st](int32_t left, int32_t right) {
#endif
            if (!je_spin_wait([shm] { return je_audio_avail(shm) < JE_RING_CAPACITY - 1; },
                              [shm] { return shm->child_shutdown != 0; },
                              &st->out_wait_ns)) return;
            const int wi = shm->audio_write & JE_RING_MASK;
            shm->audio_ring[wi].left = left;
            shm->audio_ring[wi].right = right;
#ifdef __aarch64__
            __asm__ volatile("dmb ishst" ::: "memory");
#endif
            shm->audio_write = (shm->audio_write + 1) % (JE_RING_CAPACITY * 2);
            st->samples_produced++;
#ifdef BENCH_JE_THREAD
        };
#else
        });
#endif
    }

    st->ready = 1;
    fprintf(stderr, "[je-stage%d] ready on core %d, pid=%d, ASIC%d..%d\n", s, core, (int)getpid(), lo, hi - 1);

    auto& asics = je.getAsics();
    uint32_t sample = 0;
    while (!shm->child_shutdown) {
        /* Every H8S write that precedes sample N is in our uc ring once the
         * PARENT has published sample N; the previous stage only publishes N
         * after seeing that, so its counter implies the parent's. gram N itself
         * is consumed inside processSampleChild (it feeds N+1). */
        if (!je_spin_wait([st, prev, sample] { return je_gram_avail(st) >= 1 && prev->samples_produced > (int64_t)sample; },
                          [shm] { return shm->child_shutdown != 0; },
                          &st->in_wait_ns)) break;
        while (st->uc_read != st->uc_write) {
#ifdef __aarch64__
            __asm__ volatile("dmb ishld" ::: "memory");
#endif
            const je_uc_write_t w = st->uc_ring[st->uc_read];
            if (w.sample > sample) break;
            st->uc_read = (st->uc_read + 1) % JE_UC_RING_CAP;
            asics.applyUcWrite(w.asic, w.addr, w.val);
        }
        if (!asics.processSampleChild()) break;
        sample++;
        for (int a = lo; a < hi; a++) asics.getReadback(a, (uint8_t*)shm->readback[a]);
        st->alive++;
    }

    fprintf(stderr, "[je-stage%d] shutdown, produced %lld samples\n", s, (long long)st->samples_produced);
}

/* ---- Main fork benchmark ---- */
static int bench_je_fork(const char* romDir) {
    char abspath[4096];
    std::string resolvedDir;
    if (realpath(romDir, abspath)) {
        resolvedDir = abspath;
        printf("ROM dir (resolved): %s\n", abspath);
    } else {
        resolvedDir = romDir;
        printf("ROM dir: %s\n", romDir);
    }

    (void)chdir(romDir);
    synthLib::RomLoader::addSearchPath(resolvedDir);

    auto rom = jeLib::RomLoader::findROM();
    if (!rom.isValid()) {
        fprintf(stderr, "No valid JP-8000 ROM found at %s\n", romDir);
        return 1;
    }
    printf("ROM: %s (%s)\n", rom.getName().c_str(),
           rom.getDeviceType() == jeLib::DeviceType::Keyboard ? "Keyboard" : "Rack");

    synthLib::DeviceCreateParams params;
    params.romData = rom.getData();
    params.romName = rom.getName();
    params.hostSamplerate = 88200;
    params.preferredSamplerate = 88200;
    {
        // Device writes homePath + "/roms/ram_dump.bin", so homePath is the PARENT of the roms dir.
        std::string rp(resolvedDir);   // absolute: we chdir(romDir) above
        auto pos = rp.rfind("/roms");
        params.homePath = (pos != std::string::npos) ? rp.substr(0, pos) : rp;
    }

    printf("Creating device...\n");
    jeLib::Device device(params);
    if (!device.isValid()) {
        fprintf(stderr, "Device creation failed\n");
        return 1;
    }
    device.setMasterVolume(7.0f);

    printf("DSP clock: %llu Hz (%.1f MHz)\n",
           (unsigned long long)device.getDspClockHz(),
           device.getDspClockHz() / 1e6);

    /* Boot: run until LCD shows PERFORM */
    constexpr size_t blocksize = 128;
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
        je_bench_lcd(_lcdContent);
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
    printf("Boot complete after %d blocks.\n", bootBlocks);

    /* Optional patch select, then settle */
    {
        je_bench_patch_select(midiIn);
        for (int i = 0; i < 700; i++) {
            device.process(inputs, outputs, blocksize, midiIn, midiOut);
            midiIn.clear();
            for (const auto& e : midiOut) sysexRemote.receive(e);
            midiOut.clear();
        }
        printf("LCD: [%s]\n", g_lastLcd.c_str());
    }
    printf("Device booted.\n");

    /* Pipeline shape. JE_BOUNDS="b1,b2,..." lists the ASIC index that starts each
     * child stage (ascending, 1..3); JE_SPLIT=n is the one-child shorthand. The
     * parent is always H8S + ASICs [0, b1). Cores: JE_CORES="parent,child1,..." */
    int bounds[JE_MAX_STAGES] = {0}; int nbounds = 0;
    if (const char* e = getenv("JE_BOUNDS")) {
        const char* q = e;
        while (*q && nbounds < JE_MAX_STAGES - 1) {
            int b = atoi(q);
            if (b >= 1 && b <= 3 && (nbounds == 0 || b > bounds[nbounds - 1])) bounds[nbounds++] = b;
            while (*q && *q != ',') q++;
            if (*q == ',') q++;
        }
    }
    if (nbounds == 0) {
        bounds[0] = 2; nbounds = 1;
        if (const char* e = getenv("JE_SPLIT")) bounds[0] = atoi(e) == 1 ? 1 : 2;
    }
    int cores[JE_MAX_STAGES] = {2, 3, 1, 0};
    if (const char* e = getenv("JE_CORES")) {
        const char* q = e; int n = 0;
        while (*q && n < JE_MAX_STAGES) {
            cores[n++] = atoi(q);
            while (*q && *q != ',') q++;
            if (*q == ',') q++;
        }
    }
    jeLib::devices::g_je_split_asic = bounds[0];

#ifdef BENCH_JE_THREAD
    /* One address space: the rings are plain memory. Every stage steps a
     * disjoint set of Asic objects, and the bounds and handoff callbacks are
     * thread_local, so the stage loop below is the fork one unchanged. */
    je_fork_shm_t *shm = new je_fork_shm_t();
    memset(shm, 0, sizeof(*shm));
#else
    je_fork_shm_t *shm = (je_fork_shm_t *)mmap(nullptr, sizeof(je_fork_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(shm, 0, sizeof(*shm));
#endif
    shm->num_stages = nbounds + 1;
    shm->stage[0].lo = 0; shm->stage[0].hi = bounds[0];
    for (int s = 1; s < shm->num_stages; s++) {
        shm->stage[s].lo = bounds[s - 1];
        shm->stage[s].hi = (s < nbounds) ? bounds[s] : 4;
    }

    printf("Shared memory: %zu bytes\n", sizeof(je_fork_shm_t));
    printf("Pipeline: stage0 = H8S + ASIC0..%d (core %d)", shm->stage[0].hi - 1, cores[0]);
    for (int s = 1; s < shm->num_stages; s++)
        printf(", stage%d = ASIC%d..%d (core %d)", s, shm->stage[s].lo, shm->stage[s].hi - 1, cores[s]);
    printf("\n");

#ifdef BENCH_JE_THREAD
    std::thread stage_threads[JE_MAX_STAGES];
    for (int s = 1; s < shm->num_stages; s++) {
        jeLib::Je8086 &jeRef = device.getJe8086();
        const int core = cores[s];
        stage_threads[s] = std::thread([shm, s, core, &jeRef] { je_child_main(shm, s, core, jeRef); });
    }
#else
    pid_t pids[JE_MAX_STAGES] = {0};
    for (int s = 1; s < shm->num_stages; s++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            shm->child_shutdown = 1;
            munmap(shm, sizeof(*shm));
            return 1;
        }
        if (pid == 0) {
            je_child_main(shm, s, cores[s], device.getJe8086());
            _exit(0);
        }
        pids[s] = pid;
    }
#endif

    je_pin_core(cores[0]);

    /* Wait for every stage */
    for (int s = 1; s < shm->num_stages; s++) {
        for (int i = 0; i < 100 && !shm->stage[s].ready; i++) {
            usleep(100000);
#ifndef BENCH_JE_THREAD
            int status;
            if (waitpid(pids[s], &status, WNOHANG) == pids[s]) {
                fprintf(stderr, "Stage %d died during init (status=%d)\n", s, status);
                shm->child_shutdown = 1;
                munmap(shm, sizeof(*shm));
                return 1;
            }
#endif
        }
        if (!shm->stage[s].ready) {
            fprintf(stderr, "Stage %d failed to start within 10s\n", s);
            shm->child_shutdown = 1;
#ifdef BENCH_JE_THREAD
            for (int k = 1; k < shm->num_stages; k++)
                if (stage_threads[k].joinable()) stage_threads[k].join();
            delete shm;
#else
            for (int k = 1; k < shm->num_stages; k++) { kill(pids[k], SIGTERM); waitpid(pids[k], nullptr, 0); }
            munmap(shm, sizeof(*shm));
#endif
            return 1;
        }
    }
    printf("All stages ready.\n");

    /* Parent: H8S + ASICs [0, split). Writes go to the ring of the stage that
     * owns the ASIC, stamped with the parent's sample index. */
    jeLib::devices::g_je_parallel_mode = 1;
    jeLib::devices::g_je_uc_write_forward = [shm](int asic, uint32_t addr, uint8_t val) {
        je_stage_shm_t *st = nullptr;
        for (int s = 1; s < shm->num_stages; s++)
            if (asic >= shm->stage[s].lo && asic < shm->stage[s].hi) { st = &shm->stage[s]; break; }
        if (!st) return;
        const int wi = st->uc_write;
        while ((wi + 1) % JE_UC_RING_CAP == st->uc_read) {
#ifdef __aarch64__
            __asm__ volatile("yield" ::: "memory");
#endif
        }
        st->uc_ring[wi] = je_uc_write_t{(uint8_t)asic, val, (uint16_t)addr, (uint32_t)shm->stage[0].samples_produced};
#ifdef __aarch64__
        __asm__ volatile("dmb ishst" ::: "memory");
#endif
        st->uc_write = (wi + 1) % JE_UC_RING_CAP;
    };

    /* GRAM produce callback: the split-1 -> split handoff into stage 1 */
    jeLib::devices::g_je_gram_produce = [shm](const int32_t *gram) {
        je_gram_push(&shm->stage[1], gram, &shm->stage[0].out_wait_ns);
        shm->stage[0].samples_produced++;
    };

    /* Benchmark */
    constexpr int BLOCK = 128;
    constexpr int totalFrames = 88200 * 10;
    constexpr int totalBlocks = totalFrames / BLOCK;
    int64_t audioSamplesRead = 0;

    const int split = jeLib::devices::g_je_split_asic;
    const int last = shm->num_stages - 1;
    printf("Running %d blocks (%d frames, ~10s at 88200 Hz)...\n", totalBlocks, totalFrames);
    if (getenv("JE_WTRACE")) g_wtrace = fopen(getenv("JE_WTRACE"), "w");
    jeLib::devices::g_je_uc_write_capture = [](int a, uint32_t addr, uint8_t v) {
        if (g_wtrace) fprintf(g_wtrace, "%ld a%d %04x %02x\n", g_wtraceSample, a, addr, v); };
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int b = 0; b < totalBlocks; b++) {
        g_wtraceSample = (long)b * BLOCK;
        for (int a = split; a < 4; a++) device.getJe8086().getAsics().setReadback(a, (const uint8_t*)shm->readback[a]);
        je_bench_phrase(b, midiIn);
        device.process(inputs, outputs, BLOCK, midiIn, midiOut);
        midiIn.clear();
        midiOut.clear();

        /* Drain audio ring from the last stage */
        while (je_audio_avail(shm) > 0) {
            int ri = shm->audio_read & JE_RING_MASK;
            if (g_dump) fwrite(&shm->audio_ring[ri], sizeof(shm->audio_ring[ri]), 1, g_dump);
            shm->audio_read = (shm->audio_read + 1) % (JE_RING_CAPACITY * 2);
            audioSamplesRead++;
        }

        if (b % 1000 == 0 && b > 0) {
            auto tnow = std::chrono::high_resolution_clock::now();
            double sec = std::chrono::duration<double>(tnow - t0).count();
            printf("  block %d/%d (%.1fs) audio=%d produced:", b, totalBlocks, sec, je_audio_avail(shm));
            for (int s = 0; s < shm->num_stages; s++) printf(" %lld", (long long)shm->stage[s].samples_produced);
            printf("\n");
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double ratio = 10.0 / elapsed;

    /* Drain remaining audio */
    usleep(50000);
    while (je_audio_avail(shm) > 0) {
        int ri = shm->audio_read & JE_RING_MASK;
        if (g_dump) fwrite(&shm->audio_ring[ri], sizeof(shm->audio_ring[ri]), 1, g_dump);
        shm->audio_read = (shm->audio_read + 1) % (JE_RING_CAPACITY * 2);
        audioSamplesRead++;
    }
    if (g_dump) { fclose(g_dump); g_dump = nullptr; }

    printf("\n=== Results ===\n");
    printf("Elapsed: %.2f s for 10s audio => %.2fx real-time\n", elapsed, ratio);
    for (int s = 0; s < shm->num_stages; s++)
        printf("Stage %d produced: %lld samples\n", s, (long long)shm->stage[s].samples_produced);
    printf("Audio samples read by parent: %lld\n", (long long)audioSamplesRead);
    {
        const double nsamples = 88200.0 * 10.0;
        for (int s = 0; s < shm->num_stages; s++) {
            const je_stage_shm_t *st = &shm->stage[s];
            const double iw = st->in_wait_ns / 1e9, ow = st->out_wait_ns / 1e9;
            const double busy = elapsed - iw - ow;
            printf("Stage %d (%sASIC%d..%d): busy %.2f s = %.2f us/sample  (in-wait %.2f s, out-wait %.2f s)\n",
                   s, s == 0 ? "H8S+" : "", st->lo, st->hi - 1, busy, busy * 1e6 / nsamples, iw, ow);
        }
        printf("Budget: 11.34 us/sample\n");
    }

    /* Shutdown */
    shm->child_shutdown = 1;
#ifdef BENCH_JE_THREAD
    for (int s = 1; s < shm->num_stages; s++)
        if (stage_threads[s].joinable()) stage_threads[s].join();
    delete shm;
#else
    for (int s = 1; s <= last; s++) {
        bool reaped = false;
        for (int i = 0; i < 30 && !reaped; i++) {
            int status;
            if (waitpid(pids[s], &status, WNOHANG) == pids[s]) reaped = true;
            else usleep(100000);
        }
        if (!reaped) { kill(pids[s], SIGKILL); waitpid(pids[s], nullptr, 0); }
    }
    munmap(shm, sizeof(*shm));
#endif
    return 0;
}
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom_directory> [dump_file]\n", argv[0]);
        return 1;
    }
    if (argc > 2) g_dump = fopen(argv[2], "wb");

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
#endif

    baseLib::setFlushDenormalsToZero();

#ifdef BENCH_JE
    return bench_je(argv[1]);
#elif defined(BENCH_JE_FORK) || defined(BENCH_JE_THREAD)
    return bench_je_fork(argv[1]);
#else
    fprintf(stderr, "No target defined\n");
    return 1;
#endif
}
