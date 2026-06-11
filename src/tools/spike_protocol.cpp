/*
 * spike_protocol — characterize the H8S → ESP ASIC parameter protocol.
 *
 * Goal: determine whether ASIC register writes look like a clean musical
 * parameter protocol (voice pitch, filter cutoff, envelope rate as
 * single registers) or like opaque intermediate DSP state, so we can
 * judge the feasibility of replacing the ESPs with native code while
 * keeping the H8S firmware for ROM/sysex/patch compatibility.
 *
 * The tool boots the JE-8086 from boot.snap, installs g_je_uc_write_capture
 * to record every H8S→ASIC write, then drives a scripted MIDI session:
 *   1. Program-change through several patches (record patch-load writes)
 *   2. Note-on / note-off cycles (record note-trigger writes)
 *   3. Real-time CC sweeps (filter cutoff, resonance, LFO rate)
 *   4. Sysex parameter writes for known JP-8000 params
 *
 * Writes are tagged by which session phase they happened in. Output:
 *   - <prefix>_writes.csv : every write (asic, addr, val, phase, time_us)
 *   - <prefix>_summary.md : aggregated report — unique registers, top-N
 *     by write count, registers that fire only on patch change vs note
 *     vs sysex, etc.
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <unistd.h>
#include <time.h>
#include <array>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#include "baseLib/os.h"
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"
#include "synthLib/audioTypes.h"

using namespace jeLib;

enum Phase {
    PH_IDLE,        /* free-running after boot, no input */
    PH_PATCH,       /* patch change in progress */
    PH_NOTE_ON,     /* immediately after note-on */
    PH_NOTE_OFF,    /* after note-off */
    PH_CC,          /* CC sweep */
    PH_SYSEX,       /* sysex param write */
    PH_MAX
};

static const char *phase_name(Phase p) {
    switch (p) {
        case PH_IDLE:     return "idle";
        case PH_PATCH:    return "patch";
        case PH_NOTE_ON:  return "note_on";
        case PH_NOTE_OFF: return "note_off";
        case PH_CC:       return "cc";
        case PH_SYSEX:    return "sysex";
        default:          return "?";
    }
}

struct WriteEvent {
    int asic;
    uint32_t addr;
    uint8_t val;
    Phase phase;
    int64_t time_us;
};

static std::vector<WriteEvent> g_writes;
static std::atomic<Phase> g_phase{PH_IDLE};
static int64_t g_t0 = 0;

static int64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void run_blocks(Device &device, int blocks,
                       std::vector<synthLib::SMidiEvent> &midiIn,
                       std::vector<synthLib::SMidiEvent> &midiOut,
                       SysexRemoteControl &sysexRemote) {
    constexpr size_t blocksize = 128;
    std::array<std::vector<float>, 2> bufs;
    synthLib::TAudioInputs  inputs{};
    synthLib::TAudioOutputs outputs{};
    for (size_t i = 0; i < 2; ++i) {
        bufs[i].resize(blocksize);
        outputs[i] = bufs[i].data();
    }
    for (int b = 0; b < blocks; ++b) {
        device.process(inputs, outputs, blocksize, midiIn, midiOut);
        midiIn.clear();
        for (const auto &e : midiOut) sysexRemote.receive(e);
        midiOut.clear();
    }
}

static void send_note_on(std::vector<synthLib::SMidiEvent> &midiIn, int note, int vel, int ch = 0) {
    midiIn.emplace_back(synthLib::MidiEventSource::Host,
                        (uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)vel);
}
static void send_note_off(std::vector<synthLib::SMidiEvent> &midiIn, int note, int ch = 0) {
    midiIn.emplace_back(synthLib::MidiEventSource::Host,
                        (uint8_t)(0x80 | ch), (uint8_t)note, 0);
}
static void send_program_change(std::vector<synthLib::SMidiEvent> &midiIn, int prog, int ch = 0) {
    midiIn.emplace_back(synthLib::MidiEventSource::Host,
                        (uint8_t)(0xC0 | ch), (uint8_t)prog, 0);
}
static void send_cc(std::vector<synthLib::SMidiEvent> &midiIn, int cc, int val, int ch = 0) {
    midiIn.emplace_back(synthLib::MidiEventSource::Host,
                        (uint8_t)(0xB0 | ch), (uint8_t)cc, (uint8_t)val);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom_dir> [out_prefix]\n", argv[0]);
        return 1;
    }
    const char *prefix = argc >= 3 ? argv[2] : "spike";
    char abspath[4096];
    std::string resolvedDir = realpath(argv[1], abspath) ? abspath : argv[1];
    if (chdir(argv[1]) != 0) {
        fprintf(stderr, "chdir failed\n");
        return 1;
    }
    synthLib::RomLoader::addSearchPath(resolvedDir);

    auto rom = RomLoader::findROM();
    if (!rom.isValid()) {
        fprintf(stderr, "no rom in %s\n", resolvedDir.c_str());
        return 1;
    }

    /* Install capture BEFORE creating Device so we catch init writes too. */
    g_t0 = now_us();
    g_writes.reserve(2 * 1024 * 1024);
    devices::g_je_uc_write_capture = [](int asic, uint32_t addr, uint8_t val) {
        g_writes.push_back({asic, addr, val, g_phase.load(), now_us() - g_t0});
    };

    synthLib::DeviceCreateParams params;
    params.romData = rom.getData();
    params.romName = rom.getName();
    params.homePath = resolvedDir;
    constexpr float SR = 88200.0f;
    params.hostSamplerate = SR;
    params.preferredSamplerate = SR;

    fprintf(stderr, "[spike] constructing Device...\n");
    Device device(params);
    if (!device.isValid()) {
        fprintf(stderr, "device init failed\n");
        return 1;
    }
    device.setMasterVolume(7.0f);

    /* Try snapshot for instant boot. */
    std::string snapPath = resolvedDir + "/boot.snap";
    bool snap = device.getJe8086().loadSnapshot(snapPath.c_str());
    fprintf(stderr, "[spike] snapshot %s\n", snap ? "loaded" : "MISSING — cold boot will follow");

    std::vector<synthLib::SMidiEvent> midiIn, midiOut;
    SysexRemoteControl sysexRemote;
    bool bootFinished = snap;
    sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& lcd) {
        std::string s;
        for (auto c : lcd) s += (c >= ' ' ? (char)c : ' ');
        if (!bootFinished && s.find("PERFORM") != std::string::npos)
            bootFinished = true;
    });

    if (!snap) {
        fprintf(stderr, "[spike] cold boot in progress...\n");
        g_phase = PH_IDLE;
        while (!bootFinished)
            run_blocks(device, 1, midiIn, midiOut, sysexRemote);
    }

    /* === Phase 0: settle / idle === */
    fprintf(stderr, "[spike] settling (idle, 200 blocks)...\n");
    g_phase = PH_IDLE;
    size_t mark_idle_start = g_writes.size();
    run_blocks(device, 200, midiIn, midiOut, sysexRemote);
    size_t mark_idle_end = g_writes.size();

    /* === Phase 1: patch changes A1..A8 === */
    fprintf(stderr, "[spike] patch-change sweep...\n");
    size_t mark_patch_start = g_writes.size();
    for (int prog = 0; prog < 8; ++prog) {
        g_phase = PH_PATCH;
        send_program_change(midiIn, prog);
        run_blocks(device, 100, midiIn, midiOut, sysexRemote);  /* let patch settle */
    }
    size_t mark_patch_end = g_writes.size();

    /* === Phase 2: note-on/note-off cycles on patch 0 === */
    fprintf(stderr, "[spike] note-on/off sweep on patch 0...\n");
    send_program_change(midiIn, 0);
    g_phase = PH_PATCH;
    run_blocks(device, 100, midiIn, midiOut, sysexRemote);

    size_t mark_note_start = g_writes.size();
    int notes[] = { 36, 48, 60, 72, 84 };  /* C2..C6 */
    int vels[]  = { 32, 64, 96, 127 };
    for (int n : notes) {
        for (int v : vels) {
            g_phase = PH_NOTE_ON;
            send_note_on(midiIn, n, v);
            run_blocks(device, 30, midiIn, midiOut, sysexRemote);
            g_phase = PH_NOTE_OFF;
            send_note_off(midiIn, n);
            run_blocks(device, 30, midiIn, midiOut, sysexRemote);
        }
    }
    size_t mark_note_end = g_writes.size();

    /* === Phase 3: CC sweeps === */
    fprintf(stderr, "[spike] CC sweep — filter cutoff (CC 74), resonance (CC 71)...\n");
    /* hold a note so CC has audible effect */
    g_phase = PH_NOTE_ON;
    send_note_on(midiIn, 60, 96);
    run_blocks(device, 30, midiIn, midiOut, sysexRemote);

    size_t mark_cc_start = g_writes.size();
    int ccs[] = { 74, 71, 73, 75, 76 };  /* cutoff, res, attack, decay, sustain */
    for (int cc : ccs) {
        for (int v = 0; v <= 127; v += 16) {
            g_phase = PH_CC;
            send_cc(midiIn, cc, v);
            run_blocks(device, 5, midiIn, midiOut, sysexRemote);
        }
    }
    size_t mark_cc_end = g_writes.size();
    g_phase = PH_NOTE_OFF;
    send_note_off(midiIn, 60);
    run_blocks(device, 50, midiIn, midiOut, sysexRemote);

    /* === Phase 4: trail / final idle === */
    g_phase = PH_IDLE;
    run_blocks(device, 200, midiIn, midiOut, sysexRemote);

    fprintf(stderr, "[spike] captured %zu writes\n", g_writes.size());
    (void)mark_idle_start; (void)mark_idle_end;
    (void)mark_patch_start; (void)mark_patch_end;
    (void)mark_note_start; (void)mark_note_end;
    (void)mark_cc_start; (void)mark_cc_end;

    /* === Persist raw writes === */
    std::string csvPath = std::string(prefix) + "_writes.csv";
    FILE *csv = fopen(csvPath.c_str(), "w");
    fprintf(csv, "time_us,phase,asic,addr,val\n");
    for (const auto &w : g_writes) {
        fprintf(csv, "%lld,%s,%d,0x%04x,0x%02x\n",
                (long long)w.time_us, phase_name(w.phase), w.asic, w.addr, w.val);
    }
    fclose(csv);
    fprintf(stderr, "[spike] wrote %s\n", csvPath.c_str());

    /* === Aggregate === */
    struct RegStats {
        uint64_t total = 0;
        uint64_t per_phase[PH_MAX] = {0};
        std::set<uint8_t> distinct_vals;
        uint8_t last_val = 0;
        uint64_t transitions = 0;
        bool seen = false;
    };
    std::map<uint32_t, RegStats> stats;  /* key = (asic << 16) | addr */

    for (const auto &w : g_writes) {
        uint32_t key = ((uint32_t)w.asic << 16) | w.addr;
        auto &s = stats[key];
        s.total++;
        s.per_phase[w.phase]++;
        s.distinct_vals.insert(w.val);
        if (s.seen && s.last_val != w.val) s.transitions++;
        s.last_val = w.val;
        s.seen = true;
    }

    /* Categorize registers by which phase dominates */
    auto fires_only_in = [](const RegStats &s, Phase p) -> bool {
        for (int i = 0; i < PH_MAX; ++i) {
            if (i == p) continue;
            if (s.per_phase[i] > 0) return false;
        }
        return s.per_phase[p] > 0;
    };
    auto dominant_phase = [](const RegStats &s) -> Phase {
        Phase best = PH_IDLE;
        uint64_t bestCount = 0;
        for (int i = 0; i < PH_MAX; ++i) {
            if (s.per_phase[i] > bestCount) {
                bestCount = s.per_phase[i];
                best = (Phase)i;
            }
        }
        return best;
    };

    /* === Markdown summary === */
    std::string mdPath = std::string(prefix) + "_summary.md";
    FILE *md = fopen(mdPath.c_str(), "w");
    fprintf(md, "# H8S → ESP ASIC protocol spike\n\n");
    fprintf(md, "Total writes captured: **%zu**\n\n", g_writes.size());
    fprintf(md, "Unique (asic, register) pairs: **%zu**\n\n", stats.size());

    /* per-ASIC summary */
    fprintf(md, "## Writes per ASIC\n\n| ASIC | Writes | Unique regs |\n|---|---|---|\n");
    for (int a = 0; a < 4; ++a) {
        uint64_t total = 0;
        uint64_t unique = 0;
        for (const auto &kv : stats) {
            if ((int)(kv.first >> 16) == a) { total += kv.second.total; unique++; }
        }
        fprintf(md, "| %d | %llu | %llu |\n", a, (unsigned long long)total, (unsigned long long)unique);
    }
    fprintf(md, "\n");

    /* per-phase summary */
    fprintf(md, "## Writes per phase\n\n| Phase | Writes |\n|---|---|\n");
    for (int p = 0; p < PH_MAX; ++p) {
        uint64_t total = 0;
        for (const auto &kv : stats) total += kv.second.per_phase[p];
        fprintf(md, "| %s | %llu |\n", phase_name((Phase)p), (unsigned long long)total);
    }
    fprintf(md, "\n");

    /* Registers that fire only in a specific phase — these are the
     * cleanest signal for "this register means X." */
    for (int p = 0; p < PH_MAX; ++p) {
        std::vector<std::pair<uint32_t, RegStats*>> hits;
        for (auto &kv : stats) {
            if (fires_only_in(kv.second, (Phase)p)) hits.push_back({kv.first, &kv.second});
        }
        if (hits.empty()) continue;
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
            return a.second->total > b.second->total;
        });
        fprintf(md, "## Registers that fire ONLY in phase `%s` (top 30)\n\n", phase_name((Phase)p));
        fprintf(md, "These are protocol-cleanest: a register that only fires during patch change is almost certainly a per-patch parameter.\n\n");
        fprintf(md, "| asic | addr | writes | distinct vals | transitions |\n|---|---|---|---|---|\n");
        size_t shown = std::min((size_t)30, hits.size());
        for (size_t i = 0; i < shown; ++i) {
            int a = (int)(hits[i].first >> 16);
            uint32_t addr = hits[i].first & 0xffff;
            fprintf(md, "| %d | 0x%04x | %llu | %zu | %llu |\n", a, addr,
                    (unsigned long long)hits[i].second->total,
                    hits[i].second->distinct_vals.size(),
                    (unsigned long long)hits[i].second->transitions);
        }
        fprintf(md, "\nTotal in this category: %zu registers.\n\n", hits.size());
    }

    /* Top-N most-written registers overall — likely per-sample running state */
    std::vector<std::pair<uint32_t, RegStats*>> all_sorted;
    for (auto &kv : stats) all_sorted.push_back({kv.first, &kv.second});
    std::sort(all_sorted.begin(), all_sorted.end(), [](const auto &a, const auto &b) {
        return a.second->total > b.second->total;
    });
    fprintf(md, "## Top 40 most-written registers (across all phases)\n\n");
    fprintf(md, "Registers near the top are likely per-sample DSP state — bad candidates for native takeover.\n\n");
    fprintf(md, "| asic | addr | writes | distinct vals | transitions | dominant phase |\n|---|---|---|---|---|---|\n");
    size_t topN = std::min((size_t)40, all_sorted.size());
    for (size_t i = 0; i < topN; ++i) {
        int a = (int)(all_sorted[i].first >> 16);
        uint32_t addr = all_sorted[i].first & 0xffff;
        fprintf(md, "| %d | 0x%04x | %llu | %zu | %llu | %s |\n", a, addr,
                (unsigned long long)all_sorted[i].second->total,
                all_sorted[i].second->distinct_vals.size(),
                (unsigned long long)all_sorted[i].second->transitions,
                phase_name(dominant_phase(*all_sorted[i].second)));
    }
    fprintf(md, "\n");

    /* Registers that NEVER fired during idle — i.e., only on real events */
    {
        std::vector<std::pair<uint32_t, RegStats*>> hits;
        for (auto &kv : stats) {
            if (kv.second.per_phase[PH_IDLE] == 0 && kv.second.total > 0)
                hits.push_back({kv.first, &kv.second});
        }
        std::sort(hits.begin(), hits.end(), [](const auto &a, const auto &b) {
            return a.second->total > b.second->total;
        });
        fprintf(md, "## Registers that NEVER fired during idle (top 60)\n\n");
        fprintf(md, "Idle-quiet registers are the highest-signal candidates for \"this is a real parameter.\" Total: %zu\n\n", hits.size());
        fprintf(md, "| asic | addr | writes | distinct vals | dominant phase |\n|---|---|---|---|---|\n");
        size_t n = std::min((size_t)60, hits.size());
        for (size_t i = 0; i < n; ++i) {
            int a = (int)(hits[i].first >> 16);
            uint32_t addr = hits[i].first & 0xffff;
            fprintf(md, "| %d | 0x%04x | %llu | %zu | %s |\n", a, addr,
                    (unsigned long long)hits[i].second->total,
                    hits[i].second->distinct_vals.size(),
                    phase_name(dominant_phase(*hits[i].second)));
        }
        fprintf(md, "\n");
    }

    fclose(md);
    fprintf(stderr, "[spike] wrote %s\n", mdPath.c_str());
    fprintf(stderr, "[spike] done\n");
    return 0;
}
