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
    params.homePath = std::string(romDir);

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

    /* Send a note to exercise the DSP */
    {
        synthLib::SMidiEvent noteOn(synthLib::MidiEventSource::Host, 0x90, 60, 100);
        midiIn.push_back(noteOn);
        for (int i = 0; i < 16; i++) {
            device.process(inputs, outputs, blocksize, midiIn, midiOut);
            midiIn.clear();
            midiOut.clear();
        }
    }

    /* Benchmark: 10 seconds of audio at 88200 Hz */
    constexpr int BLOCK = 128;
    constexpr int totalFrames = 88200 * 10;
    constexpr int totalBlocks = totalFrames / BLOCK;

    printf("Running %d blocks (%d frames, ~10s at 88200 Hz)...\n", totalBlocks, totalFrames);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < totalBlocks; i++) {
        device.process(inputs, outputs, BLOCK, midiIn, midiOut);
        midiOut.clear();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double ratio = 10.0 / elapsed;
    printf("Elapsed: %.2f s for 10s audio => %.2fx real-time\n", elapsed, ratio);

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
        while (je_gram_ring_available(shm) < 1) {
            if (shm->child_shutdown) return false;
#ifdef __aarch64__
            __asm__ volatile("yield" ::: "memory");
#else
            (void)0;
#endif
        }
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
        while (je_audio_ring_available(shm) >= JE_RING_CAPACITY - 1) {
            if (shm->child_shutdown) return;
#ifdef __aarch64__
            __asm__ volatile("yield" ::: "memory");
#else
            (void)0;
#endif
        }
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

    /* Drive ASIC2+3 directly — no H8S needed. */
    auto& asics = je.getAsics();
    while (!shm->child_shutdown) {
        if (!asics.processSampleAsic23()) break;
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
    params.homePath = std::string(romDir);

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

    /* Send a note */
    {
        synthLib::SMidiEvent noteOn(synthLib::MidiEventSource::Host, 0x90, 60, 100);
        midiIn.push_back(noteOn);
        for (int i = 0; i < 16; i++) {
            device.process(inputs, outputs, blocksize, midiIn, midiOut);
            midiIn.clear();
            midiOut.clear();
        }
    }
    printf("Device booted and note playing.\n");

    /* Allocate shared memory */
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

    /* Parent: ASIC0+1 mode */
    jeLib::devices::g_je_parallel_mode = 1;

    /* GRAM produce callback */
    jeLib::devices::g_je_gram_produce = [shm](const int32_t *gram) {
        while (je_gram_ring_available(shm) >= JE_RING_CAPACITY - 1) {
#ifdef __aarch64__
            __asm__ volatile("yield" ::: "memory");
#else
            (void)0;
#endif
        }
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

    printf("Running %d blocks (%d frames, ~10s at 88200 Hz)...\n", totalBlocks, totalFrames);
    printf("Parent: mode 1 (ASIC0+1), Child: mode 2 (ASIC2+3)\n");
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int b = 0; b < totalBlocks; b++) {
        device.process(inputs, outputs, BLOCK, midiIn, midiOut);
        midiOut.clear();

        /* Drain audio ring from child */
        while (je_audio_ring_available(shm) > 0) {
            int ri = shm->audio_read & JE_RING_MASK;
            (void)shm->audio_ring[ri];
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
        shm->audio_read = (shm->audio_read + 1) % (JE_RING_CAPACITY * 2);
        audioSamplesRead++;
    }

    printf("\n=== Results ===\n");
    printf("Elapsed: %.2f s for 10s audio => %.2fx real-time\n", elapsed, ratio);
    printf("Parent GRAM produced: %lld samples\n", (long long)shm->parent_samples_produced);
    printf("Child audio produced: %lld samples\n", (long long)shm->child_samples_produced);
    printf("Audio samples read by parent: %lld\n", (long long)audioSamplesRead);
    printf("Underruns — child(gram-wait): %d, parent(audio-wait): %d\n",
           shm->child_underruns, shm->parent_underruns);

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
        fprintf(stderr, "Usage: %s <rom_directory>\n", argv[0]);
        return 1;
    }

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
