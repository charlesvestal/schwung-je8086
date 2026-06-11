/*
 * jp8000_render — gearmulator-side renderer for A/B testing.
 *
 * Drives gearmulator from a fast-boot snapshot, parses a MIDI script
 * (absolute-time text format), feeds it as sample-accurate SMidiEvents
 * to Device::process, and writes a stereo WAV.
 *
 * MIDI script format (lines):
 *   # comment
 *   <time_ms> on <note> <vel>           note-on
 *   <time_ms> off <note>                note-off
 *   <time_ms> pc <prog>                 program change
 *   <time_ms> cc <controller> <value>   control change
 *   <time_ms> pb <lo> <hi>              pitch bend
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
#include <algorithm>

#include "baseLib/os.h"
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"
#include "synthLib/audioTypes.h"
#include "synthLib/wavWriter.h"
#include "dsp56kEmu/audio.h"

using namespace jeLib;

struct ScriptEvent {
    uint64_t time_samples;
    synthLib::SMidiEvent ev;
};

struct Script {
    double render_seconds = 5.0;
    std::string patch_file;
    std::vector<ScriptEvent> events;
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

        /* time_ms <action> ... */
        double time_ms;
        try { time_ms = std::stod(tok); }
        catch (...) { fprintf(stderr, "[script:%d] bad token '%s'\n", lineno, tok.c_str()); continue; }

        uint64_t t_samp = (uint64_t)(time_ms * 0.001 * samplerate);
        std::string action;
        iss >> action;

        synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
        if (action == "on") {
            int note, vel;
            iss >> note >> vel;
            ev.a = 0x90; ev.b = (uint8_t)note; ev.c = (uint8_t)vel;
        } else if (action == "off") {
            int note;
            iss >> note;
            ev.a = 0x80; ev.b = (uint8_t)note; ev.c = 0;
        } else if (action == "pc") {
            int prog;
            iss >> prog;
            ev.a = 0xC0; ev.b = (uint8_t)prog; ev.c = 0;
        } else if (action == "cc") {
            int controller, value;
            iss >> controller >> value;
            ev.a = 0xB0; ev.b = (uint8_t)controller; ev.c = (uint8_t)value;
        } else if (action == "pb") {
            int lo, hi;
            iss >> lo >> hi;
            ev.a = 0xE0; ev.b = (uint8_t)lo; ev.c = (uint8_t)hi;
        } else {
            fprintf(stderr, "[script:%d] unknown action '%s'\n", lineno, action.c_str());
            continue;
        }
        out.events.push_back({t_samp, ev});
    }
    std::sort(out.events.begin(), out.events.end(),
              [](const ScriptEvent &a, const ScriptEvent &b) { return a.time_samples < b.time_samples; });
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
    params.homePath = resolvedDir;
    params.hostSamplerate = SR;
    params.preferredSamplerate = SR;

    Device device(params);
    if (!device.isValid()) { fprintf(stderr, "device init failed\n"); return 1; }
    device.setMasterVolume(7.0f);

    std::string snapPath = resolvedDir + "/boot.snap";
    bool snap = device.getJe8086().loadSnapshot(snapPath.c_str());
    fprintf(stderr, "[render] snapshot %s\n", snap ? "loaded" : "MISSING — cold boot");

    std::vector<synthLib::SMidiEvent> midiIn, midiOut;
    SysexRemoteControl sysexRemote;
    bool bootFinished = snap;
    sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& lcd) {
        std::string s;
        for (auto c : lcd) s += (c >= ' ' ? (char)c : ' ');
        if (!bootFinished && s.find("PERFORM") != std::string::npos)
            bootFinished = true;
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

    /* Render the script, dispatching events at correct sample offsets */
    synthLib::AsyncWriter writer(out_wav, SR);
    uint64_t total_samples = (uint64_t)(script.render_seconds * SR);
    uint64_t cur_sample = 0;
    size_t next_event = 0;

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

        device.process(inputs, outputs, this_block, midiIn, midiOut);
        midiIn.clear();
        for (const auto& e : midiOut) sysexRemote.receive(e);
        midiOut.clear();

        writer.append([&outBuffers, this_block](std::vector<dsp56k::TWord>& _dst) {
            _dst.reserve(_dst.size() + this_block * 2);
            for (size_t i = 0; i < this_block; ++i) {
                _dst.push_back(dsp56k::sample2dsp(outBuffers[0][i]));
                _dst.push_back(dsp56k::sample2dsp(outBuffers[1][i]));
            }
        });
        cur_sample += this_block;
    }

    fprintf(stderr, "[render] wrote %s (%.2fs @ %u Hz)\n", out_wav, script.render_seconds, SR);
    return 0;
}
