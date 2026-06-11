/*
 * snap_bisect — isolate why snapshot-boot mutes notes.
 *
 * Boots a raw Je8086 (same path as the Move plugin) in SERIAL mode either
 * from a snapshot or from scratch, optionally warms up, sends a C4
 * note-on, renders, and reports peak/RMS.
 *
 * Usage:
 *   snap_bisect <rom_dir> snap|scratch [warmup_samples] [out.wav]
 */

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <unistd.h>

#include <chrono>

#include "baseLib/os.h"
#include "jeLib/je8086.h"

/* JIT activity counters (defined inline in esp_opt.hpp) */
extern uint64_t g_esp_genprogram_count;
extern uint64_t g_esp_dirty_count;
extern uint64_t g_esp_updatecoef_count;
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"

static FILE *wav_open(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f) { uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, f); }
    return f;
}
static void wav_close(FILE *f, uint32_t frames, uint32_t sr) {
    uint32_t bytes = frames * 4, riff = 36 + bytes;
    fseek(f, 0, SEEK_SET);
    auto w32 = [f](uint32_t v){ uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; fwrite(b,1,4,f); };
    auto w16 = [f](uint16_t v){ uint8_t b[2]={(uint8_t)v,(uint8_t)(v>>8)}; fwrite(b,1,2,f); };
    fwrite("RIFF",1,4,f); w32(riff); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(2); w32(sr); w32(sr*4); w16(4); w16(16);
    fwrite("data",1,4,f); w32(bytes);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom_dir> snap|scratch [warmup_samples] [out.wav]\n", argv[0]);
        return 1;
    }
    const char *rom_dir = argv[1];
    const bool use_snap = strcmp(argv[2], "snap") == 0;
    const long warmup = argc > 3 ? atol(argv[3]) : 176400;
    const char *wav_path = argc > 4 ? argv[4] : nullptr;

    char abspath[4096];
    std::string resolvedDir = realpath(rom_dir, abspath) ? abspath : rom_dir;
    (void)chdir(rom_dir);
    synthLib::RomLoader::addSearchPath(resolvedDir);
    baseLib::setFlushDenormalsToZero();

    auto rom = jeLib::RomLoader::findROM();
    if (!rom.isValid()) { fprintf(stderr, "no rom\n"); return 1; }

    std::string ramFile = resolvedDir + "/ram_dump.bin";
    auto *je = new jeLib::Je8086(rom.getData(), ramFile);
    if (je->hasDoneFactoryReset()) {
        delete je;
        je = new jeLib::Je8086(rom.getData(), ramFile);
    }

    jeLib::devices::g_je_parallel_mode = 0;

    if (use_snap) {
        std::string snap = resolvedDir + "/boot.snap";
        if (!je->loadSnapshot(snap.c_str())) {
            fprintf(stderr, "snapshot load failed: %s\n", snap.c_str());
            return 1;
        }
        fprintf(stderr, "[bisect] snapshot loaded\n");
    } else {
        jeLib::SysexRemoteControl sysexRemote;
        bool bootFinished = false;
        sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& c) {
            char lcd[41]{0};
            for (size_t i = 0; i < c.size(); ++i) lcd[i] = c[i] >= ' ' ? (char)c[i] : ' ';
            if (std::string(lcd).find("PERFORM") != std::string::npos) bootFinished = true;
        });
        int steps = 0;
        while (!bootFinished && steps < 50000000) {
            je->step();
            steps++;
            if (!je->getSampleBuffer().empty()) je->clearSampleBuffer();
            std::vector<synthLib::SMidiEvent> midiOut;
            je->readMidiOut(midiOut);
            for (const auto& e : midiOut) sysexRemote.receive(e);
        }
        fprintf(stderr, "[bisect] scratch boot done: %d steps\n", steps);
    }

    /* Warm-up (mirrors plugin) */
    long warmed = 0;
    while (warmed < warmup) {
        je->step();
        auto& buf = je->getSampleBuffer();
        warmed += (long)buf.size();
        if (!buf.empty()) je->clearSampleBuffer();
    }
    fprintf(stderr, "[bisect] warm-up: %ld samples\n", warmed);

    FILE *wav = wav_path ? wav_open(wav_path) : nullptr;
    uint32_t wav_frames = 0;

    /* Note test: 0.5s idle, note-on, 3s hold, note-off, 1s tail @88.2k */
    const long SR = 88200;
    const long t_on = SR / 2, t_off = t_on + 3 * SR, t_end = t_off + SR;
    bool sent_on = false, sent_off = false;
    long n = 0;
    int32_t peak_pre = 0, peak_note = 0;
    int64_t sumsq_note = 0; long n_note = 0;

    auto wall = [] { return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count(); };
    auto report_phase = [&](const char *name, long samples, double secs,
                            uint64_t gp, uint64_t dt, uint64_t uc) {
        fprintf(stderr, "[jit] %s: %.2fx RT (%ld samp / %.2fs) genProgram=%llu dirty=%llu updateCoef=%llu\n",
                name, samples / 88200.0 / secs, samples, secs,
                (unsigned long long)gp, (unsigned long long)dt, (unsigned long long)uc);
    };
    double t_phase = wall();
    long n_phase = 0;
    uint64_t gp0 = g_esp_genprogram_count, dt0 = g_esp_dirty_count, uc0 = g_esp_updatecoef_count;

    while (n < t_end) {
        if (!sent_on && n >= t_on) {
            report_phase("idle", n - n_phase, wall() - t_phase,
                         g_esp_genprogram_count - gp0, g_esp_dirty_count - dt0,
                         g_esp_updatecoef_count - uc0);
            t_phase = wall(); n_phase = n;
            gp0 = g_esp_genprogram_count; dt0 = g_esp_dirty_count; uc0 = g_esp_updatecoef_count;
            je->addMidiEvent(synthLib::SMidiEvent(synthLib::MidiEventSource::Host, 0x90, 60, 100));
            sent_on = true;
            fprintf(stderr, "[bisect] note-on at %ld\n", n);
        }
        if (!sent_off && n >= t_off) {
            report_phase("note", n - n_phase, wall() - t_phase,
                         g_esp_genprogram_count - gp0, g_esp_dirty_count - dt0,
                         g_esp_updatecoef_count - uc0);
            t_phase = wall(); n_phase = n;
            gp0 = g_esp_genprogram_count; dt0 = g_esp_dirty_count; uc0 = g_esp_updatecoef_count;
            je->addMidiEvent(synthLib::SMidiEvent(synthLib::MidiEventSource::Host, 0x80, 60, 0));
            sent_off = true;
        }
        je->step();
        auto& buf = je->getSampleBuffer();
        for (auto& s : buf) {
            int32_t l = s.first >> 9, r = s.second >> 9;  /* same scaling as plugin */
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            int32_t a = std::max(std::abs(l), std::abs(r));
            if (n < t_on) { if (a > peak_pre) peak_pre = a; }
            else { if (a > peak_note) peak_note = a; sumsq_note += (int64_t)l*l; n_note++; }
            if (wav) {
                int16_t fr[2] = {(int16_t)l, (int16_t)r};
                fwrite(fr, 2, 2, wav); wav_frames++;
            }
            n++;
        }
        if (!buf.empty()) je->clearSampleBuffer();
    }

    report_phase("tail", n - n_phase, wall() - t_phase,
                 g_esp_genprogram_count - gp0, g_esp_dirty_count - dt0,
                 g_esp_updatecoef_count - uc0);

    if (wav) { wav_close(wav, wav_frames, (uint32_t)SR); fprintf(stderr, "[bisect] wrote %s\n", wav_path); }

    double rms = n_note ? sqrt((double)sumsq_note / n_note) : 0;
    fprintf(stderr, "[bisect] mode=%s pre-note peak=%d | post-note peak=%d rms=%.1f → %s\n",
            use_snap ? "snap" : "scratch", peak_pre, peak_note, rms,
            peak_note > 200 ? "AUDIO" : "SILENT");
    return peak_note > 200 ? 0 : 2;
}
