/*
 * plugin_drive — load dsp.so through the Move plugin API (v2) and drive it
 * the way the chain host does: create_instance, poll __status until ready,
 * feed MIDI, call render_block at real-time pace, report underruns.
 *
 * Exercises the fork pipeline end to end (all stages, the SHM rings, MIDI
 * FIFO, decimation, shutdown) without a Move. Output is raw int16 stereo at
 * 44100 Hz.
 *
 *   plugin_drive <dsp.so> <module_dir> <out.raw> [seconds] [program] [rate]
 *
 * `rate` > 1 renders faster than real time (a load test: 1.15 asks for 15%
 * headroom). Env JE_PIPELINE_FILE is honoured by the plugin itself.
 *
 * `program` may be a sweep, "msb,lsb,first-last": one instance, and for every
 * program in the range a bank select + program change followed by the chord
 * phrase (10 s), reporting underruns and peak per program. This is the
 * all-presets check: the ESP cost is fixed per hardware cycle, but the H8S
 * cost is whatever the patch asks of it, so one preset proves one preset.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <ctime>
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
}

static void host_log(const char *msg) { fprintf(stderr, "[host] %s\n", msg); }
static int host_midi_noop(const uint8_t*, int) { return 0; }

static int64_t now_ns() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <dsp.so> <module_dir> <out.raw> [seconds] [program] [rate]\n", argv[0]);
        return 2;
    }
    const char *so = argv[1], *module_dir = argv[2], *out_path = argv[3];
    double seconds = argc > 4 ? atof(argv[4]) : 10.0;
    const double rate = argc > 6 ? atof(argv[6]) : 1.0;
    int program = -1;
    int sweep_msb = 81, sweep_lsb = 1, sweep_first = -1, sweep_last = -1;
    if (argc > 5) {
        if (sscanf(argv[5], "%d,%d,%d-%d", &sweep_msb, &sweep_lsb, &sweep_first, &sweep_last) == 4) {
            seconds = 10.0 * (sweep_last - sweep_first + 1);
        } else {
            program = atoi(argv[5]);
        }
    }
    const bool sweep = sweep_first >= 0;
    const bool perf = sweep && sweep_msb == 80;

    void *h = dlopen(so, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto init = (plugin_api_v2_t* (*)(const host_api_v1_t*))dlsym(h, "move_plugin_init_v2");
    if (!init) { fprintf(stderr, "no move_plugin_init_v2\n"); return 1; }

    host_api_v1_t host{};
    host.api_version = 1; host.sample_rate = 44100; host.frames_per_block = 128;
    host.log = host_log; host.midi_send_internal = host_midi_noop; host.midi_send_external = host_midi_noop;
    plugin_api_v2_t *api = init(&host);
    if (!api || api->api_version != 2) { fprintf(stderr, "bad plugin api\n"); return 1; }

    void *inst = api->create_instance(module_dir, "{}");
    if (!inst) { fprintf(stderr, "create_instance failed\n"); return 1; }

    /* Wait for ready, as the chain host does */
    char status[512];
    const int64_t t_boot = now_ns();
    for (;;) {
        api->get_param(inst, "__status", status, sizeof(status));
        if (strstr(status, "\"ready\"")) break;
        if (strstr(status, "\"error\"")) { fprintf(stderr, "plugin error: %s\n", status); return 1; }
        if (now_ns() - t_boot > 300LL * 1000000000LL) { fprintf(stderr, "boot timeout: %s\n", status); return 1; }
        usleep(100000);
    }
    fprintf(stderr, "ready after %.1fs: %s\n", (now_ns() - t_boot) / 1e9, status);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }

    /* Same phrase as the bench's JE_PHRASE=chord: bank 81/0 program select,
     * eight-note chord staggered in, held 6 s, released, 4 s tail. */
    static const int chord[8] = {48, 52, 55, 59, 62, 66, 69, 72};
    const int frames = 128;
    const int64_t total_blocks = (int64_t)(seconds * 44100 / frames);
    const int64_t block_ns = (int64_t)(frames * 1e9 / 44100 / rate);
    const int per_sec = 44100 / frames;
    int16_t buf[128 * 2];
    int64_t peak = 0, nonzero = 0;
    int last_underruns = 0;
    double sumsq = 0;

    if (program >= 0) {
        uint8_t m[3];
        m[0] = 0xB0; m[1] = 0;  m[2] = 81; api->on_midi(inst, m, 3, 0);
        m[0] = 0xB0; m[1] = 32; m[2] = 0;  api->on_midi(inst, m, 3, 0);
        m[0] = 0xC0; m[1] = (uint8_t)program; api->on_midi(inst, m, 2, 0);
    }

    /* Sweep bookkeeping: the phrase repeats every 10 s window with a new
     * program at the top of it. */
    const int64_t window = 10 * per_sec;
    int win_underruns = 0, win_peak = 0, worst_program = -1, worst_underruns = 0, silent_programs = 0;

    int64_t t0 = now_ns();
    for (int64_t b = 0; b < total_blocks; b++) {
        const int64_t wb = sweep ? b % window : b;
        if (sweep && wb == 0) {
            const int pc = sweep_first + (int)(b / window);
            uint8_t m[3];
            /* Bank 80 is a PERFORMANCE bank, and the JP-8000 only selects a
             * performance on its performance control channel (factory 16);
             * on a part channel the same program change picks a patch, so a
             * "performance sweep" on ch 1 plays one patch 64 times. */
            const uint8_t sel_ch = perf ? 15 : 0;
            m[0] = 0xB0 | sel_ch; m[1] = 0;  m[2] = (uint8_t)sweep_msb; api->on_midi(inst, m, 3, 0);
            m[0] = 0xB0 | sel_ch; m[1] = 32; m[2] = (uint8_t)sweep_lsb; api->on_midi(inst, m, 3, 0);
            m[0] = 0xC0 | sel_ch; m[1] = (uint8_t)pc; api->on_midi(inst, m, 2, 0);
            api->get_param(inst, "__status", status, sizeof(status));
            const char *u = strstr(status, "\"underruns\":");
            win_underruns = u ? atoi(u + 12) : 0;
            win_peak = 0;
        }
        /* A performance has two parts on their own channels (Upper 1,
         * Lower 2 at the factory settings); the chord goes to both so a
         * layer/split renders every voice it can — the load we are after. */
        for (int ch = 0; ch < (perf ? 2 : 1); ch++)
        for (int n = 0; n < 8; n++) {
            uint8_t m[3];
            if (wb == per_sec / 4 + n * 6) { m[0] = 0x90 | ch; m[1] = chord[n]; m[2] = 100; api->on_midi(inst, m, 3, 0); }
            if (wb == 6 * per_sec)         { m[0] = 0x80 | ch; m[1] = chord[n]; m[2] = 0;   api->on_midi(inst, m, 3, 0); }
        }
        api->render_block(inst, buf, frames);
        fwrite(buf, sizeof(buf), 1, out);
        if (sweep) {
            for (int i = 0; i < frames * 2; i++) {
                int v = buf[i] < 0 ? -buf[i] : buf[i];
                if (v > win_peak) win_peak = v;
            }
            if (wb == window - 1) {
                const int pc = sweep_first + (int)(b / window);
                api->get_param(inst, "__status", status, sizeof(status));
                const char *u = strstr(status, "\"underruns\":");
                const int cur = u ? atoi(u + 12) : 0;
                const int d = cur - win_underruns;
                /* A silent program is a failed select, not a quiet patch —
                 * every JP-8000 preset sounds on an 8-note chord. */
                if (win_peak < 100) silent_programs++;
                if (d > worst_underruns) { worst_underruns = d; worst_program = pc; }
                fprintf(stderr, "program %3d: underruns +%d peak=%d%s\n", pc, d, win_peak, win_peak < 100 ? "  SILENT" : "");
            }
        }
        /* Underruns are only useful with a time: a burst at the first note is
         * the JIT compiling a program, a trickle later is starvation. */
        if ((b & 7) == 0) {
            static char prev_status[512] = "";
            static double prev_t = 0;
            api->get_param(inst, "__status", status, sizeof(status));
            const char *u = strstr(status, "\"underruns\":");
            int cur = u ? atoi(u + 12) : 0;
            const double t = (double)b * frames / 44100.0;
            if (cur != last_underruns) {
                /* The poll before the underrun shows which stage's counter
                 * had already stopped moving. */
                if (prev_status[0]) fprintf(stderr, "  t=%.2fs before: %s\n", prev_t, prev_status);
                fprintf(stderr, "  t=%.2fs underruns %d -> %d (%s)\n", t, last_underruns, cur, status);
                last_underruns = cur;
                prev_status[0] = 0;
            } else {
                strncpy(prev_status, status, sizeof(prev_status) - 1);
                prev_t = t;
            }
        }
        for (int i = 0; i < frames * 2; i++) {
            int v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > peak) peak = v;
            if (v) nonzero++;
            sumsq += (double)buf[i] * buf[i];
        }
        /* pace. If WE fell behind (an fwrite to eMMC can block ~100 ms),
         * re-base rather than catch up: a burst of back-to-back blocks
         * drains the ring at far more than `rate` and reports underruns the
         * synth did not cause. */
        const int64_t due = t0 + (b + 1) * block_ns;
        int64_t now = now_ns();
        if (due > now) usleep((useconds_t)((due - now) / 1000));
        else if (now - due > 4 * block_ns) {
            fprintf(stderr, "  t=%.2fs harness stalled %.1f ms, re-basing\n", (double)b * frames / 44100.0, (now - due) / 1e6);
            t0 += now - due;
        }
    }
    const double elapsed = (now_ns() - t0) / 1e9;
    fclose(out);

    api->get_param(inst, "__status", status, sizeof(status));
    fprintf(stderr, "rendered %.1fs in %.1fs (rate %.2f): peak=%lld rms=%.1f nonzero=%.1f%% status=%s\n",
            seconds, elapsed, rate, (long long)peak,
            sqrt(sumsq / (double)(total_blocks * frames * 2)),
            100.0 * nonzero / (double)(total_blocks * frames * 2), status);
    if (sweep)
        fprintf(stderr, "sweep bank %d/%d programs %d-%d: worst program %d (+%d underruns), %d silent\n",
                sweep_msb, sweep_lsb, sweep_first, sweep_last, worst_program, worst_underruns, silent_programs);

    api->destroy_instance(inst);
    dlclose(h);
    return 0;
}
