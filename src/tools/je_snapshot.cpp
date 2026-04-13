/*
 * je_snapshot — Generate boot snapshot for JP-8000 emulator.
 * Usage: je_snapshot <rom_dir> [output_file]
 *
 * Boots the JE-8086 emulator (takes ~30s), then saves the full state
 * to a .snap file that the plugin can load instantly.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

#ifdef __linux__
#include <sched.h>
#endif
#include <unistd.h>

#include "baseLib/os.h"
#include "jeLib/je8086.h"
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/romLoader.h"
#include "synthLib/midiTypes.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom_directory> [output_file]\n", argv[0]);
        fprintf(stderr, "  Boots JP-8000 emulator and saves state snapshot.\n");
        fprintf(stderr, "  Default output: <rom_directory>/boot.snap\n");
        return 1;
    }

    const char *romDir = argv[1];
    std::string outputPath;
    if (argc >= 3) {
        outputPath = argv[2];
    } else {
        outputPath = std::string(romDir) + "/boot.snap";
    }

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    sched_setaffinity(0, sizeof(cpuset), &cpuset);
#endif
    baseLib::setFlushDenormalsToZero();

    /* Load ROM */
    char abspath[4096];
    std::string resolvedDir;
    if (realpath(romDir, abspath))
        resolvedDir = abspath;
    else
        resolvedDir = romDir;

    (void)chdir(romDir);
    synthLib::RomLoader::addSearchPath(resolvedDir);

    auto rom = jeLib::RomLoader::findROM();
    if (!rom.isValid()) {
        fprintf(stderr, "No valid JP-8000 ROM found at %s\n", romDir);
        return 1;
    }
    printf("ROM: %s\n", rom.getName().c_str());

    /* Create Je8086 */
    std::string ramFile = resolvedDir + "/ram_dump.bin";
    printf("Creating Je8086...\n");
    auto *je = new jeLib::Je8086(rom.getData(), ramFile);
    if (je->hasDoneFactoryReset()) {
        printf("Factory reset done, re-creating...\n");
        delete je;
        je = new jeLib::Je8086(rom.getData(), ramFile);
    }

    /* Boot: step until LCD shows PERFORM */
    printf("Booting (this takes ~30 seconds)...\n");
    jeLib::SysexRemoteControl sysexRemote;
    bool bootFinished = false;

    sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& _lcdContent) {
        char lcd[41]{0};
        for (size_t i = 0; i < _lcdContent.size(); ++i)
            lcd[i] = _lcdContent[i] >= ' ' ? static_cast<char>(_lcdContent[i]) : ' ';
        if (std::string(lcd).find("PERFORM") != std::string::npos)
            bootFinished = true;
    });

    auto t0 = std::chrono::high_resolution_clock::now();
    int steps = 0;
    while (!bootFinished && steps < 50000000) {
        je->step();
        steps++;
        if (!je->getSampleBuffer().empty())
            je->clearSampleBuffer();
        std::vector<synthLib::SMidiEvent> midiOut;
        je->readMidiOut(midiOut);
        for (const auto& e : midiOut)
            sysexRemote.receive(e);
        if (steps % 1000000 == 0)
            printf("  step %dM...\n", steps / 1000000);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    if (!bootFinished) {
        fprintf(stderr, "Boot did not complete after %d steps (%.1fs)\n", steps, elapsed);
        delete je;
        return 1;
    }
    printf("Boot complete after %d steps (%.1fs)\n", steps, elapsed);

    /* Save snapshot */
    printf("Saving snapshot to %s...\n", outputPath.c_str());
    if (!je->saveSnapshot(outputPath.c_str())) {
        fprintf(stderr, "Failed to save snapshot\n");
        delete je;
        return 1;
    }

    /* Report file size */
    FILE *check = fopen(outputPath.c_str(), "rb");
    if (check) {
        fseek(check, 0, SEEK_END);
        long sz = ftell(check);
        fclose(check);
        printf("Snapshot saved: %ld bytes (%.1f MB)\n", sz, sz / 1048576.0);
    }

    delete je;
    printf("Done.\n");
    return 0;
}
