/*
 * jp8000_render — gearmulator-side renderer for A/B testing.
 *
 * Drives gearmulator from a fast-boot snapshot, parses a MIDI script
 * (absolute-time text format), feeds it as sample-accurate SMidiEvents
 * to Device::process, and writes a stereo WAV.
 *
 * MIDI script format (lines):
 *   # comment
 *   <time_ms> on <note> <vel> [ch]      note-on (channel 1-16, default 1)
 *   <time_ms> off <note> [ch]           note-off
 *   <time_ms> pc <prog> [ch]             program change (ch 16 = performance select)
 *   <time_ms> cc <controller> <value> [ch]  control change
 *   <time_ms> pb <lo> <hi>              pitch bend
 *   <time_ms> button <id> <down|up>     front-panel switch (je8086devices.h)
 *   render_seconds <float>              total render length (default 5)
 *   patch_file <path>                   load sysex patch dump before play
 *
 * CLI:
 *   jp8000_render <rom_dir> <script.txt> <out.wav>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>

#include "baseLib/os.h"
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "jeLib/state.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"
#include "synthLib/audioTypes.h"
#include "synthLib/wavWriter.h"
#include "dsp56kEmu/audio.h"

using namespace jeLib;
using jeLib::State;
using jeLib::SystemParameter;

#define JP_UC_CAPTURE_HOOK jeLib::devices::g_je_uc_write_capture

struct ScriptEvent {
    uint64_t time_samples;
    synthLib::SMidiEvent ev;
};

struct ButtonEvent {
    uint64_t time_samples;
    int switch_id;
    bool pressed;
};

struct SnapEvent {
    uint64_t time_samples;
    std::string path;
};

struct Script {
    double render_seconds = 5.0;
    double clock_bpm = 0.0;      /* > 0: emit 24 ppqn MIDI clock for the whole render */
    std::string patch_file;
    std::vector<ScriptEvent> events;
    std::vector<ButtonEvent> buttons;   /* front-panel presses, see je8086devices.h */
    std::vector<SnapEvent> snaps;       /* mid-render state dumps, for A/B diffing */
};

static bool parse_script(const std::string &path, Script &out, uint32_t samplerate) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "cannot open script %s\n", path.c_str()); return false; }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        std::istringstream iss(line);
        std::string tok;
        if (!(iss >> tok)) continue;

        if (tok == "render_seconds") {
            iss >> out.render_seconds;
            continue;
        }
        if (tok == "patch_file") {
            iss >> out.patch_file;
            continue;
        }
        if (tok == "clock_bpm") {
            /* The JP-8000 arpeggiator is clocked.  synthLib::Plugin -- which the
             * JUCE plugin runs through, and which jp8000_render does not --
             * synthesises 24 ppqn MIDI clock from the host transport.  Without
             * it the device gets no clock at all. */
            iss >> out.clock_bpm;
            continue;
        }

        /* time_ms <action> ... */
        double time_ms;
        try { time_ms = std::stod(tok); }
        catch (...) { fprintf(stderr, "[script:%d] bad token '%s'\n", lineno, tok.c_str()); continue; }

        uint64_t t_samp = (uint64_t)(time_ms * 0.001 * samplerate);
        std::string action;
        iss >> action;

        synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
        if (action == "on") {
            int note, vel, ch = 1;
            iss >> note >> vel;
            if (!(iss >> ch)) ch = 1;          /* optional 1-16, default 1 */
            ev.a = (uint8_t)(0x90 | ((ch - 1) & 0x0f)); ev.b = (uint8_t)note; ev.c = (uint8_t)vel;
        } else if (action == "off") {
            int note, ch = 1;
            iss >> note;
            if (!(iss >> ch)) ch = 1;
            ev.a = (uint8_t)(0x80 | ((ch - 1) & 0x0f)); ev.b = (uint8_t)note; ev.c = 0;
        } else if (action == "pc") {
            int prog, ch = 1;
            iss >> prog;
            if (!(iss >> ch)) ch = 1;          /* optional 1-16; 16 selects performances */
            ev.a = (uint8_t)(0xC0 | ((ch - 1) & 0x0f)); ev.b = (uint8_t)prog; ev.c = 0;
        } else if (action == "cc") {
            int controller, value, ch = 1;
            iss >> controller >> value;
            if (!(iss >> ch)) ch = 1;
            ev.a = (uint8_t)(0xB0 | ((ch - 1) & 0x0f)); ev.b = (uint8_t)controller; ev.c = (uint8_t)value;
        } else if (action == "start") {
            ev.a = 0xFA; ev.b = 0; ev.c = 0;
        } else if (action == "stop") {
            ev.a = 0xFC; ev.b = 0; ev.c = 0;
        } else if (action == "continue") {
            ev.a = 0xFB; ev.b = 0; ev.c = 0;
        } else if (action == "pb") {
            int lo, hi;
            iss >> lo >> hi;
            ev.a = 0xE0; ev.b = (uint8_t)lo; ev.c = (uint8_t)hi;
        } else if (action == "sysparam") {
            /* sysparam <SystemParameter index> <value> -- the system area is
             * what the Schwung plugin forces at boot (MidiSync, LocalSwitch,
             * TxRx*) and what nothing else here has ever written. */
            int idx, val;
            iss >> idx >> val;
            auto d = State::createParameterChange((SystemParameter)idx, val);
            ev.sysex.assign(d.begin(), d.end());
            fprintf(stderr, "[script] sysparam %d = %d ->", idx, val);
            for (auto b : ev.sysex) fprintf(stderr, " %02X", b);
            fprintf(stderr, "\n");
            ev.a = 0; ev.b = 0; ev.c = 0;
            out.events.push_back({t_samp, ev});
            continue;
        } else if (action == "perfreq") {
            /* Read back the temp performance (common block included), which is
             * where the arpeggiator lives: ArpeggioSwitch 0x17, Mode 0x18,
             * BeatPattern 0x19, OctaveRange 0x1A, Hold 0x1B, Destination 0x15,
             * Tempo 0x22. */
            auto d = State::createPerformanceRequest(jeLib::AddressArea::PerformanceTemp,
                                                     jeLib::UserPerformanceArea::UserPerformance01);
            ev.sysex.assign(d.begin(), d.end());
            ev.a = 0; ev.b = 0; ev.c = 0;
            out.events.push_back({t_samp, ev});
            continue;
        } else if (action == "perfcommon") {
            /* perfcommon <PerformanceCommon offset> <value> */
            int off, val;
            iss >> off >> val;
            auto d = State::createParameterChange((jeLib::PerformanceCommon)off, val);
            ev.sysex.assign(d.begin(), d.end());
            ev.a = 0; ev.b = 0; ev.c = 0;
            fprintf(stderr, "[script] perfcommon %#04x = %d ->", off, val);
            for (auto b : ev.sysex) fprintf(stderr, " %02X", b);
            fprintf(stderr, "\n");
            out.events.push_back({t_samp, ev});
            continue;
        } else if (action == "sysreq") {
            /* Ask the firmware for its system area.  A reply proves the whole
             * sysex round trip -- injection, parse, and DT1 out -- which is
             * the control every system-area write in here needs. */
            auto d = State::createSystemRequest();
            ev.sysex.assign(d.begin(), d.end());
            ev.a = 0; ev.b = 0; ev.c = 0;
            fprintf(stderr, "[script] sysreq ->");
            for (auto b : ev.sysex) fprintf(stderr, " %02X", b);
            fprintf(stderr, "\n");
            out.events.push_back({t_samp, ev});
            continue;
        } else if (action == "snapshot") {
            /* Dump full device state mid-render.  Two runs that differ only in
             * one button press can then be diffed to locate the byte that press
             * moved -- the firmware's own answer, not our image of it. */
            std::string path; iss >> path;
            out.snaps.push_back({t_samp, path});
            continue;
        } else if (action == "button") {
            /* button <switch_id> <down|up> -- kSwitch_* from jeLib/je8086devices.h
             * (72 = PerformSel, the Patch/Perform toggle; 133 = arp OnOff). */
            int id; std::string state;
            iss >> id >> state;
            out.buttons.push_back({t_samp, id, state != "up"});
            continue;
        } else {
            fprintf(stderr, "[script:%d] unknown action '%s'\n", lineno, action.c_str());
            continue;
        }
        out.events.push_back({t_samp, ev});
    }
    if (out.clock_bpm > 0.0) {
        const double period = 60.0 / (out.clock_bpm * 24.0);
        for (double t = 0.0; t < out.render_seconds; t += period) {
            synthLib::SMidiEvent ck(synthLib::MidiEventSource::Host);
            ck.a = 0xF8; ck.b = 0; ck.c = 0;
            out.events.push_back({(uint64_t)(t * samplerate), ck});
        }
    }
    std::sort(out.events.begin(), out.events.end(),
              [](const ScriptEvent &a, const ScriptEvent &b) { return a.time_samples < b.time_samples; });
    std::sort(out.buttons.begin(), out.buttons.end(),
              [](const ButtonEvent &a, const ButtonEvent &b) { return a.time_samples < b.time_samples; });
    return true;
}

static std::vector<uint8_t> read_binary_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <rom_dir> <script.txt> <out.wav>\n", argv[0]);
        return 1;
    }
    const char *rom_dir = argv[1];
    const char *script_path = argv[2];
    const char *out_wav = argv[3];

    char abspath[4096];
    std::string resolvedDir = realpath(rom_dir, abspath) ? abspath : rom_dir;
    if (chdir(rom_dir) != 0) { fprintf(stderr, "chdir failed\n"); return 1; }
    synthLib::RomLoader::addSearchPath(resolvedDir);

    auto rom = RomLoader::findROM();
    if (!rom.isValid()) { fprintf(stderr, "no rom in %s\n", resolvedDir.c_str()); return 1; }

    constexpr uint32_t SR = 88200;
    synthLib::DeviceCreateParams params;
    params.romData = rom.getData();
    params.romName = rom.getName();
    /* jeLib::Device looks for the battery-backed RAM at homePath + "/roms/
     * ram_dump.bin", so homePath is the PARENT of the rom dir, not the rom dir.
     * Passing the rom dir made every construction miss the RAM image and run
     * runfactoryreset() -- which returns early from the Je8086 ctor and leaves
     * postSample, the MIDI rate limiter and the LCD callbacks unbound.  The
     * snapshot path survived that only because loadSnapshot() rebinds them all;
     * cold boot went straight into std::bad_function_call. */
    std::string homeDir = resolvedDir;
    {
        const size_t slash = homeDir.find_last_of('/');
        if (slash != std::string::npos && slash > 0) homeDir = homeDir.substr(0, slash);
    }
    params.homePath = homeDir;
    params.hostSamplerate = SR;
    params.preferredSamplerate = SR;

    Device device(params);
    if (!device.isValid()) { fprintf(stderr, "device init failed\n"); return 1; }
    device.setMasterVolume(7.0f);

    /* JP_NO_SNAPSHOT: build against a gearmulator tree without our snapshot
     * commits (used to A/B the JP-8000 core against pre-fork vanilla). */
#ifdef JP_NO_SNAPSHOT
    const bool snap = false;
    fprintf(stderr, "[render] snapshot support not built — cold boot\n");
#else
    std::string snapPath = resolvedDir + "/boot.snap";
    bool snap = device.getJe8086().loadSnapshot(snapPath.c_str());
    fprintf(stderr, "[render] snapshot %s\n", snap ? "loaded" : "MISSING — cold boot");
#endif

    std::vector<synthLib::SMidiEvent> midiIn, midiOut;
    SysexRemoteControl sysexRemote;
    bool bootFinished = snap;
    /* JP_LCD_LOG=1 prints every LCD change with its render time.  The LCD is
     * the firmware's own readout, so it is the cheapest proof that a scripted
     * front-panel button actually reached the panel scan. */
    const bool lcdLog = getenv("JP_LCD_LOG") != nullptr;
    const bool sysexLog = getenv("JP_SYSEX_LOG") != nullptr;
    const bool midiOutLog = getenv("JP_MIDIOUT_LOG") != nullptr;
    uint64_t lcdSampleClock = 0;
    std::string lcdLast;
    sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& lcd) {
        std::string s;
        for (auto c : lcd) s += (c >= ' ' ? (char)c : ' ');
        if (!bootFinished && s.find("PERFORM") != std::string::npos)
            bootFinished = true;
        if (lcdLog && s != lcdLast) {
            fprintf(stderr, "[lcd %7.3fs] |%s|\n", (double)lcdSampleClock / SR, s.c_str());
            lcdLast = s;
        }
    });

    constexpr size_t blocksize = 128;
    std::array<std::vector<float>, 2> outBuffers;
    synthLib::TAudioInputs inputs{};
    synthLib::TAudioOutputs outputs{};
    for (size_t i = 0; i < 2; ++i) {
        outBuffers[i].resize(blocksize);
        outputs[i] = outBuffers[i].data();
    }

    if (!snap) {
        fprintf(stderr, "[render] cold boot in progress...\n");
        while (!bootFinished) {
            device.process(inputs, outputs, blocksize, midiIn, midiOut);
            for (const auto& e : midiOut) sysexRemote.receive(e);
            midiOut.clear();
        }
    }

    /* Parse script */
    Script script;
    if (!parse_script(script_path, script, SR)) return 1;
    fprintf(stderr, "[render] script %s: %zu events, %.2fs total\n",
            script_path, script.events.size(), script.render_seconds);

    /* Inject patch dump if requested (parsed as one or more sysex messages). */
    if (!script.patch_file.empty()) {
        auto bytes = read_binary_file(script.patch_file);
        if (bytes.empty()) {
            fprintf(stderr, "[render] patch_file %s empty/missing\n", script.patch_file.c_str());
        } else {
            /* split on 0xF0..0xF7 boundaries */
            size_t i = 0;
            int n = 0;
            while (i < bytes.size()) {
                if (bytes[i] != 0xF0) { i++; continue; }
                size_t j = i + 1;
                while (j < bytes.size() && bytes[j] != 0xF7) j++;
                if (j >= bytes.size()) break;
                synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
                ev.sysex.assign(bytes.begin() + (std::ptrdiff_t)i, bytes.begin() + (std::ptrdiff_t)(j + 1));
                midiIn.push_back(std::move(ev));
                n++;
                i = j + 1;
            }
            fprintf(stderr, "[render] injected %d sysex messages from %s\n", n, script.patch_file.c_str());
            /* Let device absorb the sysex before play starts: render ~0.5s of audio */
            int absorb_blocks = (int)(0.5 * SR / blocksize);
            for (int b = 0; b < absorb_blocks; ++b) {
                device.process(inputs, outputs, blocksize, midiIn, midiOut);
                midiIn.clear();
                for (const auto& e : midiOut) sysexRemote.receive(e);
                midiOut.clear();
            }
        }
    }

    /* Diagnostic: per-block H8S->ASIC write counts.  A note-on makes the
     * firmware program a voice, which is a burst of these; an arpeggiator
     * firing makes one burst per step.  This observes the firmware directly,
     * so it is independent of the patch's envelope, delay and chorus --
     * unlike any audio-domain onset detector.  JP_UC_LOG=<path> to enable. */
    const char *ucLogPath = getenv("JP_UC_LOG");
    constexpr size_t UC_BUCKETS = 4 * 64;   /* asic x (addr >> 8), addr is 14-bit */
    std::vector<uint32_t> ucCounts;
    uint64_t ucBlock = 0;
    if (ucLogPath) {
        JP_UC_CAPTURE_HOOK =
            [&ucCounts, &ucBlock](int asic, uint32_t addr, uint8_t) {
                const size_t need = (size_t)(ucBlock + 1) * UC_BUCKETS;
                if (ucCounts.size() < need) ucCounts.resize(need, 0u);
                if (asic < 0 || asic > 3) return;
                ucCounts[(size_t)ucBlock * UC_BUCKETS + (size_t)asic * 64 + ((addr >> 8) & 63)]++;
            };
    }

    /* Render the script, dispatching events at correct sample offsets */
    synthLib::AsyncWriter writer(out_wav, SR);
    uint64_t total_samples = (uint64_t)(script.render_seconds * SR);
    uint64_t cur_sample = 0;
    size_t next_event = 0;
    size_t next_button = 0;
    size_t next_snap = 0;

    while (cur_sample < total_samples) {
        size_t this_block = std::min<size_t>(blocksize, (size_t)(total_samples - cur_sample));

        /* Pull events landing in [cur_sample, cur_sample + this_block) */
        while (next_event < script.events.size() &&
               script.events[next_event].time_samples < cur_sample + this_block) {
            auto ev = script.events[next_event].ev;
            ev.offset = (uint32_t)(script.events[next_event].time_samples - cur_sample);
            midiIn.push_back(ev);
            next_event++;
        }

        /* Front-panel presses land at block granularity -- the firmware debounces
         * them over milliseconds, so a sample offset would be meaningless. */
        while (next_button < script.buttons.size() &&
               script.buttons[next_button].time_samples < cur_sample + this_block) {
            const auto &b = script.buttons[next_button];
            device.getJe8086().setButton((jeLib::devices::SwitchType)b.switch_id, b.pressed);
            printf("[render] button %d %s at %.3fs\n", b.switch_id,
                   b.pressed ? "down" : "up", (double)b.time_samples / SR);
            next_button++;
        }

        device.process(inputs, outputs, this_block, midiIn, midiOut);
        midiIn.clear();
        for (const auto& e : midiOut) {
            /* JP_MIDIOUT_LOG: the JP-8000 transmits arpeggiated notes, so this
             * shows whether the arp ENGINE runs even when no voice is programmed. */
            if (midiOutLog && e.sysex.empty() && e.a && (e.a & 0xf0) != 0xf0) {
                fprintf(stderr, "[midi-out %7.3fs] %02X %02X %02X\n",
                        (double)cur_sample / SR, e.a, e.b, e.c);
            }
            if (sysexLog && !e.sysex.empty() && !(e.sysex.size() > 5 && e.sysex[5] == 0x12 && e.sysex.size() > 8 && e.sysex[6] == 0x7f)) {
                /* Skip the LCD remote-control chatter, print real DT1 replies. */
                if (!(e.sysex.size() >= 5 && e.sysex[1] == 0x00 && e.sysex[2] == 0x21)) {
                    fprintf(stderr, "[sysex-in %7.3fs %3zu]", (double)cur_sample / SR, e.sysex.size());
                    for (size_t k = 0; k < e.sysex.size() && k < 64; ++k) fprintf(stderr, " %02X", e.sysex[k]);
                    fprintf(stderr, "%s\n", e.sysex.size() > 64 ? " ..." : "");
                }
            }
            sysexRemote.receive(e);
        }
        midiOut.clear();

#ifndef JP_NO_SNAPSHOT
        while (next_snap < script.snaps.size() &&
               script.snaps[next_snap].time_samples < cur_sample + this_block) {
            const bool ok = device.getJe8086().saveSnapshot(script.snaps[next_snap].path.c_str());
            fprintf(stderr, "[render] snapshot %s at %.3fs -> %s\n",
                    script.snaps[next_snap].path.c_str(),
                    (double)script.snaps[next_snap].time_samples / SR, ok ? "ok" : "FAILED");
            next_snap++;
        }
#endif

        writer.append([&outBuffers, this_block](std::vector<dsp56k::TWord>& _dst) {
            _dst.reserve(_dst.size() + this_block * 2);
            for (size_t i = 0; i < this_block; ++i) {
                _dst.push_back(dsp56k::sample2dsp(outBuffers[0][i]));
                _dst.push_back(dsp56k::sample2dsp(outBuffers[1][i]));
            }
        });
        cur_sample += this_block;
        lcdSampleClock = cur_sample;
        ucBlock++;
    }

    if (ucLogPath) {
        JP_UC_CAPTURE_HOOK = nullptr;
        FILE *uf = fopen(ucLogPath, "wb");
        if (uf) {
            /* raw uint32 little-endian, shape (nblocks, 4, 64); block = blocksize samples */
            const size_t nblocks = ucCounts.size() / UC_BUCKETS;
            fwrite(ucCounts.data(), sizeof(uint32_t), nblocks * UC_BUCKETS, uf);
            fclose(uf);
            fprintf(stderr, "[render] uC write log -> %s (%zu blocks x %zu buckets)\n",
                    ucLogPath, nblocks, UC_BUCKETS);
        }
    }

    fprintf(stderr, "[render] wrote %s (%.2fs @ %u Hz)\n", out_wav, script.render_seconds, SR);
    return 0;
}
