/*
 * Headless DSP benchmark for JP-8000 (JE-8086) emulation.
 * Cross-compile for Move, scp to device, run with ROM path.
 *
 * Targets:
 *   bench_je      <rom_dir>   — serial (all 4 ASICs on 1 core)
 *   bench_je_fork <rom_dir>   — fork-parallel (ASIC0+1 and ASIC2+3 on 2 cores)
 */

#include <cstdio>
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

#if defined(BENCH_JE) || defined(BENCH_JE_FORK)
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
static void je_bench_lcd(const std::array<char, 40>& c) {
    char lcd[41]{0};
    for (size_t i = 0; i < c.size(); ++i) lcd[i] = c[i] >= ' ' ? static_cast<char>(c[i]) : ' ';
    g_lastLcd = lcd;
}
static void je_bench_patch_select(std::vector<synthLib::SMidiEvent>& midiIn) {
    const char* e = getenv("JE_PATCH");
    if (!e) return;
    int msb = 81, lsb = 1, pc = 0;
    if (sscanf(e, "%d,%d,%d", &msb, &lsb, &pc) < 3) { fprintf(stderr, "JE_PATCH wants msb,lsb,pc\n"); return; }
    midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xB0, 0, msb);
    midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xB0, 32, lsb);
    midiIn.emplace_back(synthLib::MidiEventSource::Host, 0xC0, pc, 0);
    printf("Patch select: bank %d/%d program %d\n", msb, lsb, pc);
}
static void je_bench_phrase(int block, std::vector<synthLib::SMidiEvent>& midiIn) {
    constexpr int perSec = 88200 / 128;   // 689 blocks
    static const int notes[4] = {60, 64, 67, 72};
    for (int n = 0; n < 4; n++) {
        const int on = n * 2 * perSec;
        const int off = on + 2 * perSec - (n == 3 ? 0 : perSec / 8);
        if (block == on) midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x90, notes[n], 100);
        if (block == off) midiIn.emplace_back(synthLib::MidiEventSource::Host, 0x80, notes[n], 0);
    }
}
#endif

#ifdef BENCH_JE_FORK
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
    jeLib::devices::g_je_uc_read_capture = [](int a, uint32_t addr) { rd[a][addr & 3]++; };
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

#ifdef BENCH_JE_FORK

/* ---- SPSC ring helpers ---- */
static int je_gram_ring_available(je_fork_shm_t *shm) {
    int avail = shm->gram_write - shm->gram_read;
    if (avail < 0) avail += JE_RING_CAPACITY * 2;
    return avail;
}
static int je_audio_ring_available(je_fork_shm_t *shm) {
    int avail = shm->audio_write - shm->audio_read;
    if (avail < 0) avail += JE_RING_CAPACITY * 2;
    return avail;
}

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

/* ---- Child process: ASIC2+3 ---- */
static void je_child_main(je_fork_shm_t *shm, jeLib::Je8086 &je) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
#endif
    baseLib::setFlushDenormalsToZero();

    /* GRAM consume callback: reads from parent's ring buffer */
    jeLib::devices::g_je_gram_consume = [shm](int32_t *gram) -> bool {
        if (!je_spin_wait([shm] { return je_gram_ring_available(shm) >= 1; },
                          [shm] { return shm->child_shutdown != 0; },
                          &shm->child_gram_wait_ns)) return false;
        int ri = shm->gram_read & JE_RING_MASK;
#ifdef __aarch64__
        __asm__ volatile("dmb ishld" ::: "memory");
#endif
        for (int k = 0; k < JE_GRAM_HANDOFF_COUNT; k++)
            gram[k] = shm->gram_ring[ri].gram[k];
        shm->gram_read = (shm->gram_read + 1) % (JE_RING_CAPACITY * 2);
        return true;
    };

    /* Audio output callback: writes to audio ring for parent to read */
    je.getAsics().setPostSample([shm](int32_t left, int32_t right) {
        if (!je_spin_wait([shm] { return je_audio_ring_available(shm) < JE_RING_CAPACITY - 1; },
                          [shm] { return shm->child_shutdown != 0; },
                          &shm->child_audio_wait_ns)) return;
        int wi = shm->audio_write & JE_RING_MASK;
        shm->audio_ring[wi].left = left;
        shm->audio_ring[wi].right = right;
#ifdef __aarch64__
        __asm__ volatile("dmb ishst" ::: "memory");
#endif
        shm->audio_write = (shm->audio_write + 1) % (JE_RING_CAPACITY * 2);
        shm->child_samples_produced++;
    });

    shm->child_ready = 1;
    fprintf(stderr, "[je-child] ready on core 3, pid=%d, ASIC2+3 direct loop\n", (int)getpid());

    /* Drive the child's ASICs directly — no H8S needed. */
    auto& asics = je.getAsics();
    const int split = jeLib::devices::g_je_split_asic;
    uint32_t sample = 0;
    while (!shm->child_shutdown) {
        /* All of the parent's writes that precede sample N are in the ring once
         * gram N has been published (same thread, store-ordered), so wait for it
         * before rendering N. gram N itself is consumed inside processSampleChild
         * (it feeds sample N+1); gram N-1 went in at the end of the last iteration. */
        if (!je_spin_wait([shm, sample] { return (uint32_t)je_gram_ring_available(shm) >= 1 && shm->parent_samples_produced > (int64_t)sample; },
                          [shm] { return shm->child_shutdown != 0; },
                          &shm->child_gram_wait_ns)) break;
        while (shm->uc_read != shm->uc_write) {
#ifdef __aarch64__
            __asm__ volatile("dmb ishld" ::: "memory");
#endif
            const je_uc_write_t w = shm->uc_ring[shm->uc_read];
            if (w.sample > sample) break;
            shm->uc_read = (shm->uc_read + 1) % JE_UC_RING_CAP;
            asics.applyUcWrite(w.asic, w.addr, w.val);
        }
        if (!asics.processSampleChild()) break;
        sample++;
        for (int a = split; a < 4; a++) asics.getReadback(a, (uint8_t*)shm->readback[a]);
        shm->child_alive++;
    }

    fprintf(stderr, "[je-child] shutdown, produced %lld samples\n",
            (long long)shm->child_samples_produced);
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

    /* Allocate shared memory */
    if (const char* e = getenv("JE_SPLIT")) jeLib::devices::g_je_split_asic = atoi(e) == 1 ? 1 : 2;
    je_fork_shm_t *shm = (je_fork_shm_t *)mmap(nullptr, sizeof(je_fork_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(shm, 0, sizeof(*shm));

    printf("Shared memory: %zu bytes\n", sizeof(je_fork_shm_t));
    printf("Forking child for ASIC2+3...\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        munmap(shm, sizeof(*shm));
        return 1;
    }

    if (pid == 0) {
        je_child_main(shm, device.getJe8086());
        _exit(0);
    }

    /* Parent: pin to core 2 */
#ifdef __linux__
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);
        sched_setaffinity(0, sizeof(cpuset), &cpuset);
    }
#endif

    /* Wait for child */
    for (int i = 0; i < 100 && !shm->child_ready; i++) {
        usleep(100000);
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            fprintf(stderr, "Child died during init (status=%d)\n", status);
            munmap(shm, sizeof(*shm));
            return 1;
        }
    }
    if (!shm->child_ready) {
        fprintf(stderr, "Child failed to start within 10s\n");
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        munmap(shm, sizeof(*shm));
        return 1;
    }
    printf("Child ready.\n");

    /* Parent: H8S + ASICs [0, split) */
    jeLib::devices::g_je_parallel_mode = 1;
    jeLib::devices::g_je_uc_write_forward = [shm](int asic, uint32_t addr, uint8_t val) {
        const int wi = shm->uc_write;
        while ((wi + 1) % JE_UC_RING_CAP == shm->uc_read) {
#ifdef __aarch64__
            __asm__ volatile("yield" ::: "memory");
#endif
        }
        shm->uc_ring[wi] = je_uc_write_t{(uint8_t)asic, val, (uint16_t)addr, (uint32_t)shm->parent_samples_produced};
#ifdef __aarch64__
        __asm__ volatile("dmb ishst" ::: "memory");
#endif
        shm->uc_write = (wi + 1) % JE_UC_RING_CAP;
    };

    /* GRAM produce callback */
    jeLib::devices::g_je_gram_produce = [shm](const int32_t *gram) {
        je_spin_wait([shm] { return je_gram_ring_available(shm) < JE_RING_CAPACITY - 1; },
                     [] { return false; },
                     &shm->parent_gram_wait_ns);
        int wi = shm->gram_write & JE_RING_MASK;
        for (int k = 0; k < JE_GRAM_HANDOFF_COUNT; k++)
            shm->gram_ring[wi].gram[k] = gram[k];
#ifdef __aarch64__
        __asm__ volatile("dmb ishst" ::: "memory");
#endif
        shm->gram_write = (shm->gram_write + 1) % (JE_RING_CAPACITY * 2);
        shm->parent_samples_produced++;
    };

    /* Benchmark */
    constexpr int BLOCK = 128;
    constexpr int totalFrames = 88200 * 10;
    constexpr int totalBlocks = totalFrames / BLOCK;
    int64_t audioSamplesRead = 0;

    const int split = jeLib::devices::g_je_split_asic;
    printf("Running %d blocks (%d frames, ~10s at 88200 Hz)...\n", totalBlocks, totalFrames);
    printf("Parent: H8S + ASIC0..%d, Child: ASIC%d..3 (JE_SPLIT=%d)\n", split - 1, split, split);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int b = 0; b < totalBlocks; b++) {
        for (int a = split; a < 4; a++) device.getJe8086().getAsics().setReadback(a, (const uint8_t*)shm->readback[a]);
        je_bench_phrase(b, midiIn);
        device.process(inputs, outputs, BLOCK, midiIn, midiOut);
        midiIn.clear();
        midiOut.clear();

        /* Drain audio ring from child */
        while (je_audio_ring_available(shm) > 0) {
            int ri = shm->audio_read & JE_RING_MASK;
            if (g_dump) fwrite(&shm->audio_ring[ri], sizeof(shm->audio_ring[ri]), 1, g_dump);
            shm->audio_read = (shm->audio_read + 1) % (JE_RING_CAPACITY * 2);
            audioSamplesRead++;
        }

        if (b % 1000 == 0 && b > 0) {
            auto tnow = std::chrono::high_resolution_clock::now();
            double sec = std::chrono::duration<double>(tnow - t0).count();
            printf("  block %d/%d (%.1fs) gram=%d audio=%d parent=%lld child=%lld\n",
                   b, totalBlocks, sec,
                   je_gram_ring_available(shm), je_audio_ring_available(shm),
                   (long long)shm->parent_samples_produced,
                   (long long)shm->child_samples_produced);
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double ratio = 10.0 / elapsed;

    /* Drain remaining audio */
    usleep(50000);
    while (je_audio_ring_available(shm) > 0) {
        int ri = shm->audio_read & JE_RING_MASK;
        if (g_dump) fwrite(&shm->audio_ring[ri], sizeof(shm->audio_ring[ri]), 1, g_dump);
        shm->audio_read = (shm->audio_read + 1) % (JE_RING_CAPACITY * 2);
        audioSamplesRead++;
    }
    if (g_dump) { fclose(g_dump); g_dump = nullptr; }

    printf("\n=== Results ===\n");
    printf("Elapsed: %.2f s for 10s audio => %.2fx real-time\n", elapsed, ratio);
    printf("Parent GRAM produced: %lld samples\n", (long long)shm->parent_samples_produced);
    printf("Child audio produced: %lld samples\n", (long long)shm->child_samples_produced);
    printf("Audio samples read by parent: %lld\n", (long long)audioSamplesRead);
    printf("Underruns — child(gram-wait): %d, parent(audio-wait): %d\n",
           shm->child_underruns, shm->parent_underruns);
    {
        const double us = 1e3 * 88200.0 * 10.0;   // samples in the run, for per-sample figures
        const double pw = shm->parent_gram_wait_ns, cg = shm->child_gram_wait_ns, ca = shm->child_audio_wait_ns;
        printf("Parent (H8S+ASICs<split): busy %.2f s = %.2f us/sample  (gram-full wait %.2f s)\n",
               elapsed - pw / 1e9, (elapsed - pw / 1e9) * 1e6 / (us / 1e3), pw / 1e9);
        printf("Child  (ASICs>=split):    busy %.2f s = %.2f us/sample  (gram-empty wait %.2f s, audio-full wait %.2f s)\n",
               elapsed - (cg + ca) / 1e9, (elapsed - (cg + ca) / 1e9) * 1e6 / (us / 1e3), cg / 1e9, ca / 1e9);
        printf("Budget: 11.34 us/sample\n");
    }

    /* Shutdown */
    shm->child_shutdown = 1;
    for (int i = 0; i < 30; i++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) goto done;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
done:
    munmap(shm, sizeof(*shm));
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
#elif defined(BENCH_JE_FORK)
    return bench_je_fork(argv[1]);
#else
    fprintf(stderr, "No target defined\n");
    return 1;
#endif
}
