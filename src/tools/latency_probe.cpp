/* latency_probe.cpp -- note-on to first audio, with the consumer PACED.
 *
 * plugin_host_test renders blocks back to back, so the plugin never gets
 * ahead and the figure it produces is not the one a player feels. The device
 * consumes exactly 128 frames every 2.9 ms, which lets a plugin that renders
 * ahead fill its buffer -- and everything in that buffer is latency.
 *
 *   latency_probe <dsp.so> <module_dir> [out.wav]
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <time.h>
#include <vector>
#include <string>

/* COPY THESE EXACTLY. Omitting one field silently rebinds every pointer after
 * it: leaving out get_error made render_block resolve to get_error, so the
 * probe rendered nothing and read back an uninitialised buffer -- which looks
 * like a constant near full scale, i.e. silence that reads as a signal. */
typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;
typedef struct {
    uint32_t api_version;
    void *(*create_instance)(const char *, const char *);
    void (*destroy_instance)(void *);
    void (*on_midi)(void *, const uint8_t *, int, int);
    void (*set_param)(void *, const char *, const char *);
    int (*get_param)(void *, const char *, char *, int);
    int (*get_error)(void *, char *, int);
    void (*render_block)(void *, int16_t *, int);
} plugin_api_v2_t;

static const int SR = 44100, FRAMES = 128;

static uint64_t now_us() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <dsp.so> <module_dir> [out.wav]\n", argv[0]); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    auto init = (plugin_api_v2_t *(*)(const host_api_v1_t *))dlsym(h, "move_plugin_init_v2");
    host_api_v1_t host{}; host.api_version = 1; host.sample_rate = SR; host.frames_per_block = FRAMES;
    host.log = [](const char *m) { fprintf(stderr, "[hostlog] %s\n", m); };
    plugin_api_v2_t *api = init(&host);
    void *inst = api->create_instance(argv[2], nullptr);
    /* Extra key=value args are applied before the note, so two runs can differ
     * by exactly one thing -- which is the only way to tell "the state
     * restored" from "the image says it did". */
    std::vector<const char *> ops;
    for (int i = 4; i < argc; i++) ops.push_back(argv[i]);
    /* Silence first: no arpeggiator, and Patch mode so nothing is holding. */
    

    char buf[256];
    for (int i = 0; i < 400 && api->get_param(inst, "__status", buf, sizeof buf) >= 0; i++) {
        if (strstr(buf, "\"ready\"")) break;
        int16_t b[FRAMES * 2]; api->render_block(inst, b, FRAMES);
        struct timespec t{0, 20 * 1000000}; nanosleep(&t, nullptr);
    }

    for (const char *op : ops) {
        const char *eq = strchr(op, '=');
        if (!eq) continue;
        std::string k(op, eq - op);
        api->set_param(inst, k.c_str(), eq + 1);
        for (int i = 0; i < 300; i++) {
            int16_t b[FRAMES * 2] = {0}; api->render_block(inst, b, FRAMES);
            struct timespec t{0, 3 * 1000000}; nanosleep(&t, nullptr);
        }
    }

    /* Paced exactly like the SPI callback: one block every 2.9 ms. */
    std::vector<int16_t> pcm;
    const int total = (SR / FRAMES) * 6;
    const int noteon = (SR / FRAMES) * 3;
    uint64_t t0 = now_us();
    long note_us = 0;
    for (int b = 0; b < total; b++) {
        while (now_us() - t0 < (uint64_t)b * 1000000ull * FRAMES / SR) { }
        if (b == noteon) {
            uint8_t on[3] = {0x90, 48, 100};      /* ch1 = straight to the part's voices */
            api->on_midi(inst, on, 3, 0);
            note_us = (long)(b * 1000000ll * FRAMES / SR);
        }
        /* ZERO IT. render_block need not write when it has nothing, and an
         * uninitialised block reads back as a constant near full scale --
         * which is silence that looks like a signal, i.e. exactly the thing
         * this tool exists to distinguish. */
        int16_t blk[FRAMES * 2] = {0};
        api->render_block(inst, blk, FRAMES);
        pcm.insert(pcm.end(), blk, blk + FRAMES * 2);
    }
    api->get_param(inst, "__status", buf, sizeof buf);
    fprintf(stderr, "[probe] %s\n", buf);
    api->destroy_instance(inst);

    /* The measurement is only valid from SILENCE: the loaded performance's
     * arpeggiator drones, and against a full-scale drone every threshold is
     * already crossed at the note, which reads as 0 ms latency. */
    int32_t pre = 0;
    for (long i = (long)((noteon - 20) * FRAMES) * 2; i < (long)(noteon * FRAMES) * 2 && i < (long)pcm.size(); i++) {
        int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (v > pre) pre = v;
    }
    fprintf(stderr, "[probe] peak in the 60 ms BEFORE the note: %d%s\n", pre,
            pre > 400 ? "   <-- NOT SILENT, latency figure is meaningless" : "");

    /* First sample after the note that clears a floor. */
    const long n0 = (long)((double)note_us * SR / 1e6);
    int32_t peak = 0;
    for (size_t i = n0 * 2; i < pcm.size(); i++) { int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i]; if (v > peak) peak = v; }
    const int32_t thr = peak / 50 > 40 ? peak / 50 : 40;
    long first = -1;
    for (size_t i = n0 * 2; i < pcm.size(); i += 2) {
        int32_t v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > thr) { first = (long)(i / 2); break; }
    }
    printf("note-on at %.1f ms, first audio at %.1f ms  ->  LATENCY %.1f ms (peak %d)\n",
           note_us / 1000.0, first < 0 ? -1.0 : first * 1000.0 / SR,
           first < 0 ? -1.0 : (first - n0) * 1000.0 / SR, peak);

    if (argc > 3) {
        FILE *f = fopen(argv[3], "wb");
        uint32_t bytes = (uint32_t)(pcm.size() * 2), riff = 36 + bytes, rate = SR, brate = SR * 4;
        uint16_t ch = 2, bits = 16, fmt = 1, align = 4; uint32_t sz16 = 16;
        fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
        fwrite(&sz16, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
        fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f);
        fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
        fwrite(pcm.data(), 2, pcm.size(), f); fclose(f);
        fprintf(stderr, "[probe] wrote %s\n", argv[3]);
    }
    return 0;
}
