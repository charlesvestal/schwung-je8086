/*
 * nusaw_render — drive schwung-nusaw's engine with the same MIDI script
 * format used by jp8000_render, so the two outputs can be compared directly.
 *
 * Maps a basic subset of CC events to engine params; PC events are ignored
 * (nusaw doesn't have JP-8000-style patches). Output: float32 stereo WAV
 * @ 44100 Hz, matching the engine's native rate.
 *
 * Script format is identical to jp8000_render's.
 *
 * CLI:
 *   nusaw_render <script.txt> <out.wav>
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

extern "C" {
#include "nusaw_engine.h"
}

constexpr float SR = 44100.0f;
constexpr int BLOCKSIZE = 128;

struct Event {
    uint64_t t_samp;
    int type;      /* 0=on, 1=off, 2=cc, 3=pb */
    int a, b;      /* type-specific */
};

struct Script {
    double render_seconds = 5.0;
    std::vector<Event> events;
};

static bool parse_script(const std::string &path, Script &out) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "cannot open script %s\n", path.c_str()); return false; }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        auto h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        std::istringstream iss(line);
        std::string tok;
        if (!(iss >> tok)) continue;

        if (tok == "render_seconds") { iss >> out.render_seconds; continue; }
        if (tok == "patch_file")     { continue; }  /* ignored */

        double time_ms;
        try { time_ms = std::stod(tok); } catch (...) { continue; }
        uint64_t t = (uint64_t)(time_ms * 0.001 * SR);

        std::string action;
        iss >> action;
        Event ev{t, -1, 0, 0};
        if (action == "on")  { iss >> ev.a >> ev.b; ev.type = 0; }
        else if (action == "off") { iss >> ev.a; ev.type = 1; }
        else if (action == "cc")  { iss >> ev.a >> ev.b; ev.type = 2; }
        else if (action == "pb")  { iss >> ev.a >> ev.b; ev.type = 3; }
        else if (action == "pc")  { continue; }   /* ignored */
        else continue;
        out.events.push_back(ev);
    }
    std::sort(out.events.begin(), out.events.end(),
              [](const Event &a, const Event &b) { return a.t_samp < b.t_samp; });
    return true;
}

static void apply_cc(nsaw_engine_t &eng, int cc, int val) {
    float v = val / 127.0f;
    switch (cc) {
        case 74: eng.cutoff   = v; break;
        case 71: eng.resonance= v; break;
        case 73: eng.attack   = v; break;
        case 75: eng.decay    = v; break;
        case 76: eng.sustain  = v; break;
        case 72: eng.release  = v; break;
        case  7: eng.volume   = v; break;
        case  1: eng.f_amount = v; break;  /* mod wheel → filter env amount */
        default: break;
    }
}

/* Minimal 16-bit PCM stereo WAV writer.
 * Caller appends interleaved L/R int16 frames via add_block(); writer
 * stamps the correct sizes into the header on close. */
struct WavWriter {
    FILE *f = nullptr;
    uint32_t sr = 0;
    uint32_t samples_written = 0;

    bool open(const std::string &path, uint32_t samplerate) {
        f = fopen(path.c_str(), "wb");
        if (!f) return false;
        sr = samplerate;
        uint8_t hdr[44] = {0};
        fwrite(hdr, 1, 44, f);   /* placeholder; rewritten on close */
        return true;
    }
    void add_block(const float *L, const float *R, int n) {
        for (int i = 0; i < n; ++i) {
            int32_t l = (int32_t)std::lround(std::max(-1.0f, std::min(1.0f, L[i])) * 32767.0f);
            int32_t r = (int32_t)std::lround(std::max(-1.0f, std::min(1.0f, R[i])) * 32767.0f);
            int16_t s[2] = { (int16_t)l, (int16_t)r };
            fwrite(s, sizeof(int16_t), 2, f);
        }
        samples_written += (uint32_t)n;
    }
    void close() {
        if (!f) return;
        uint32_t byte_count = samples_written * 2 /*ch*/ * 2 /*bytes*/;
        uint32_t riff_size  = 36 + byte_count;
        fseek(f, 0, SEEK_SET);
        auto w32 = [this](uint32_t v) {
            uint8_t b[4] = {(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
            fwrite(b, 1, 4, f);
        };
        auto w16 = [this](uint16_t v) {
            uint8_t b[2] = {(uint8_t)v,(uint8_t)(v>>8)};
            fwrite(b, 1, 2, f);
        };
        fwrite("RIFF", 1, 4, f); w32(riff_size); fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2); w32(sr);
        w32(sr * 2 * 2); w16(4); w16(16);
        fwrite("data", 1, 4, f); w32(byte_count);
        fclose(f); f = nullptr;
    }
};

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <script.txt> <out.wav>\n", argv[0]);
        return 1;
    }
    const char *script_path = argv[1];
    const char *out_path    = argv[2];

    Script script;
    if (!parse_script(script_path, script)) return 1;

    nsaw_engine_t eng{};
    nsaw_engine_init(&eng);
    eng.sample_rate = SR;
    eng.volume = 0.8f;
    fprintf(stderr, "[nusaw] script %s: %zu events, %.2fs\n",
            script_path, script.events.size(), script.render_seconds);

    WavWriter w;
    if (!w.open(out_path, (uint32_t)SR)) {
        fprintf(stderr, "cannot open %s\n", out_path);
        return 1;
    }

    std::vector<float> L(BLOCKSIZE), R(BLOCKSIZE);
    uint64_t total = (uint64_t)(script.render_seconds * SR);
    uint64_t cur = 0;
    size_t next_ev = 0;

    while (cur < total) {
        size_t this_block = std::min<size_t>(BLOCKSIZE, (size_t)(total - cur));

        /* Apply any events that fall in this block at start-of-block resolution.
         * nusaw_engine doesn't expose sample-accurate event dispatch, so all
         * events in a block effectively fire at the block boundary — fine
         * for 128-frame blocks at 44.1k = 2.9ms granularity. */
        while (next_ev < script.events.size() &&
               script.events[next_ev].t_samp < cur + this_block) {
            const auto &ev = script.events[next_ev];
            switch (ev.type) {
                case 0: nsaw_engine_note_on(&eng, ev.a, ev.b / 127.0f); break;
                case 1: nsaw_engine_note_off(&eng, ev.a); break;
                case 2: apply_cc(eng, ev.a, ev.b); break;
                case 3: {
                    int v = (ev.b << 7) | ev.a;  /* 14-bit pitch bend */
                    float norm = (v - 8192) / 8192.0f;
                    nsaw_engine_pitch_bend(&eng, norm);
                    break;
                }
                default: break;
            }
            next_ev++;
        }

        nsaw_engine_render(&eng, L.data(), R.data(), (int)this_block);
        w.add_block(L.data(), R.data(), (int)this_block);
        cur += this_block;
    }

    w.close();
    fprintf(stderr, "[nusaw] wrote %s\n", out_path);
    return 0;
}
