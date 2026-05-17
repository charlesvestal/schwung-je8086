// audio_probe — boot Je8086, send MIDI C4, capture 5s, dump stats.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <unistd.h>
#include "baseLib/os.h"
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/je8086devices.h"
#include "jeLib/romloader.h"
#include "jeLib/sysexRemoteControl.h"
#include "synthLib/midiTypes.h"
#include "synthLib/romLoader.h"
#include "synthLib/audioTypes.h"
#include "synthLib/wavWriter.h"
#include "dsp56kEmu/audio.h"

using namespace jeLib;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <rom_dir> [out_wav]\n", argv[0]); return 1; }
    const char *out_wav = argc >= 3 ? argv[2] : "probe.wav";
    char abspath[4096];
    std::string resolvedDir = realpath(argv[1], abspath) ? abspath : argv[1];
    chdir(argv[1]);
    synthLib::RomLoader::addSearchPath(resolvedDir);

    auto rom = RomLoader::findROM();
    if (!rom.isValid()) { fprintf(stderr, "no rom\n"); return 1; }

    synthLib::DeviceCreateParams params;
    params.romData = rom.getData();
    params.romName = rom.getName();
    constexpr uint32_t samplerate = 88200;
    params.hostSamplerate = samplerate;
    params.preferredSamplerate = samplerate;

    Device device(params);
    if (!device.isValid()) { fprintf(stderr, "device init failed\n"); return 1; }
    device.setMasterVolume(7.0f);

    std::array<std::vector<float>, 2> outBuffers;
    synthLib::TAudioInputs inputs;
    synthLib::TAudioOutputs outputs;
    constexpr size_t blocksize = 128;
    for (size_t i=0; i<outBuffers.size(); ++i) {
        outBuffers[i].resize(blocksize);
        outputs[i] = outBuffers[i].data();
    }

    std::vector<synthLib::SMidiEvent> midiIn, midiOut;
    bool bootFinished = false;

    SysexRemoteControl sysexRemote;
    sysexRemote.evLcdDdDataChanged.addListener([&](const std::array<char, 40>& lcd) {
        std::string s;
        for (auto c : lcd) s += (c >= ' ' ? (char)c : ' ');
        if (!bootFinished && s.find("PERFORM") != std::string::npos) bootFinished = true;
    });

    printf("[probe] boot...\n");
    while (!bootFinished) {
        device.process(inputs, outputs, blocksize, midiIn, midiOut);
        for (const auto& e : midiOut) sysexRemote.receive(e);
        midiOut.clear();
    }
    printf("[probe] PERFORM reached\n");

    /* OPTIONAL note-on via env var NOTE=1 for testing.
     * If unset, capture WITHOUT MIDI to isolate engine baseline behavior. */
    if (getenv("NOTE")) {
        synthLib::SMidiEvent noteOn(synthLib::MidiEventSource::Host, 0x90, 60, 100);
        midiIn.push_back(noteOn);
        printf("[probe] note-on C4 vel 100\n");
    } else {
        printf("[probe] no MIDI sent — capturing baseline silence\n");
    }

    /* Capture 5s float audio */
    synthLib::AsyncWriter writer(out_wav, samplerate);
    int total = samplerate * 5;
    std::vector<float> capL, capR;
    capL.reserve(total); capR.reserve(total);
    while ((int)capL.size() < total) {
        device.process(inputs, outputs, blocksize, midiIn, midiOut);
        midiIn.clear();
        for (const auto& e : midiOut) sysexRemote.receive(e);
        midiOut.clear();
        writer.append([&outBuffers, blocksize](std::vector<dsp56k::TWord>& _dst) {
            _dst.reserve(_dst.size() + blocksize * 2);
            for (size_t i=0; i<blocksize; ++i) {
                _dst.push_back(dsp56k::sample2dsp(outBuffers[0][i]));
                _dst.push_back(dsp56k::sample2dsp(outBuffers[1][i]));
            }
        });
        for (size_t i=0; i<blocksize && (int)capL.size()<total; ++i) {
            capL.push_back(outputs[0][i]);
            capR.push_back(outputs[1][i]);
        }
    }

    float peak = 0; double sumSq = 0;
    for (auto v : capL) { float a = std::abs(v); if (a > peak) peak = a; sumSq += v*v; }
    double rms = std::sqrt(sumSq / capL.size());
    printf("[probe] captured %zu samples; L peak=%.4f rms=%.4f\n", capL.size(), peak, rms);
    return 0;
}
