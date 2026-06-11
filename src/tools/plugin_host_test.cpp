/*
 * plugin_host_test — minimal Move v2 plugin host for autonomous testing.
 *
 * dlopens a dsp.so, boots it, injects MIDI note-ons directly via on_midi,
 * renders in real-block cadence, and reports per-second peak/RMS so a
 * script (or an LLM over ssh) can verify audio output with no UI and no
 * human in the loop.
 *
 * Usage:
 *   plugin_host_test <dsp.so> <module_dir> [seconds] [out.wav]
 *
 * Exit code: 0 if peak after note-on exceeds threshold, 2 if silent,
 *            1 on load/boot error.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <dlfcn.h>
#include <unistd.h>

extern "C" {

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

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

typedef plugin_api_v2_t* (*init_v2_fn)(const host_api_v1_t *host);

} /* extern "C" */

#define SR 44100
#define FRAMES 128

static void host_log(const char *msg) { fprintf(stderr, "[hostlog] %s\n", msg); }
static int host_midi_stub(const uint8_t *, int) { return 0; }

/* minimal 16-bit stereo WAV writer */
static FILE *wav_open(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f) { uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, f); }
    return f;
}
static void wav_close(FILE *f, uint32_t frames) {
    uint32_t bytes = frames * 4, riff = 36 + bytes;
    fseek(f, 0, SEEK_SET);
    auto w32 = [f](uint32_t v){ uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; fwrite(b,1,4,f); };
    auto w16 = [f](uint16_t v){ uint8_t b[2]={(uint8_t)v,(uint8_t)(v>>8)}; fwrite(b,1,2,f); };
    fwrite("RIFF",1,4,f); w32(riff); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); w32(16); w16(1); w16(2); w32(SR); w32(SR*4); w16(4); w16(16);
    fwrite("data",1,4,f); w32(bytes);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dsp.so> <module_dir> [seconds] [out.wav]\n", argv[0]);
        return 1;
    }
    const char *so_path = argv[1];
    const char *module_dir = argv[2];
    int seconds = argc > 3 ? atoi(argv[3]) : 8;
    const char *wav_path = argc > 4 ? argv[4] : nullptr;

    void *so = dlopen(so_path, RTLD_NOW);
    if (!so) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    auto init = (init_v2_fn)dlsym(so, "move_plugin_init_v2");
    if (!init) { fprintf(stderr, "no move_plugin_init_v2: %s\n", dlerror()); return 1; }

    host_api_v1_t host = {};
    host.api_version = 2;
    host.sample_rate = SR;
    host.frames_per_block = FRAMES;
    host.log = host_log;
    host.midi_send_internal = host_midi_stub;
    host.midi_send_external = host_midi_stub;

    plugin_api_v2_t *api = init(&host);
    if (!api) { fprintf(stderr, "init_v2 returned null\n"); return 1; }

    void *inst = api->create_instance(module_dir, "{}");
    if (!inst) { fprintf(stderr, "create_instance failed\n"); return 1; }

    /* Wait for ready — poll __status, render silence meanwhile (a real host
     * keeps calling render_block during load). */
    char status[512];
    int16_t block[FRAMES * 2];
    bool ready = false;
    for (int i = 0; i < 150 * 10; i++) {  /* up to 150s */
        api->render_block(inst, block, FRAMES);
        if (i % 10 == 0) {
            status[0] = 0;
            api->get_param(inst, "__status", status, sizeof(status));
            if (i % 50 == 0) fprintf(stderr, "[host] t=%.1fs status=%s\n", i / 10.0, status);
            if (strstr(status, "\"ready\"")) { ready = true; break; }
            if (strstr(status, "\"error\"")) { fprintf(stderr, "[host] plugin error: %s\n", status); return 1; }
        }
        usleep(100000);
    }
    if (!ready) { fprintf(stderr, "[host] boot timeout\n"); return 1; }
    fprintf(stderr, "[host] plugin ready: %s\n", status);

    FILE *wav = wav_path ? wav_open(wav_path) : nullptr;
    uint32_t wav_frames = 0;

    /* Timeline: 1s idle | note-on C4 | hold (seconds-3) | note-off | 2s tail.
     * Render at real-block cadence (128 frames / 44100 Hz ≈ 2.9 ms). */
    const long total_blocks = (long)seconds * SR / FRAMES;
    const long noteon_blk  = 1L * SR / FRAMES;
    const long noteoff_blk = (long)(seconds - 2) * SR / FRAMES;

    int cur_sec = -1;
    int32_t sec_peak = 0; int64_t sec_sumsq = 0; long sec_n = 0;
    int32_t post_note_peak = 0;

    for (long b = 0; b < total_blocks; b++) {
        if (b == noteon_blk) {
            uint8_t on[3] = {0x90, 60, 100};
            api->on_midi(inst, on, 3, 0);
            fprintf(stderr, "[host] note-on C4 sent\n");
        }
        if (b == noteoff_blk) {
            uint8_t off[3] = {0x80, 60, 0};
            api->on_midi(inst, off, 3, 0);
            fprintf(stderr, "[host] note-off sent\n");
        }

        api->render_block(inst, block, FRAMES);
        if (wav) { fwrite(block, sizeof(int16_t), FRAMES * 2, wav); wav_frames += FRAMES; }

        for (int i = 0; i < FRAMES * 2; i++) {
            int32_t v = block[i] < 0 ? -block[i] : block[i];
            if (v > sec_peak) sec_peak = v;
            if (b > noteon_blk && v > post_note_peak) post_note_peak = v;
            sec_sumsq += (int64_t)block[i] * block[i];
            sec_n++;
        }

        int sec = (int)(b * FRAMES / SR);
        if (sec != cur_sec) {
            if (cur_sec >= 0) {
                double rms = sqrt((double)sec_sumsq / sec_n);
                fprintf(stderr, "[host] sec=%d peak=%d rms=%.1f\n", cur_sec, sec_peak, rms);
            }
            cur_sec = sec; sec_peak = 0; sec_sumsq = 0; sec_n = 0;
        }
        usleep(1000000L * FRAMES / SR);  /* real-time cadence */
    }
    if (sec_n > 0) {
        double rms = sqrt((double)sec_sumsq / sec_n);
        fprintf(stderr, "[host] sec=%d peak=%d rms=%.1f\n", cur_sec, sec_peak, rms);
    }

    status[0] = 0;
    api->get_param(inst, "__status", status, sizeof(status));
    fprintf(stderr, "[host] final status=%s\n", status);

    if (wav) { wav_close(wav, wav_frames); fprintf(stderr, "[host] wrote %s\n", wav_path); }

    api->destroy_instance(inst);

    fprintf(stderr, "[host] post-note peak=%d → %s\n", post_note_peak,
            post_note_peak > 200 ? "AUDIO OK" : "SILENT");
    return post_note_peak > 200 ? 0 : 2;
}
