/*
 * plugin_params — read and write a v2 plugin's parameters from outside.
 *
 * plugin_drive proves the AUDIO; this proves the CONTRACT. It dlopens a dsp.so
 * the way the chain host does, waits for ready, then runs a script of get/set
 * operations. That is what lets the UI-facing behaviour (what a browser row
 * says, which part an edit addresses, what a preset blob restores) be checked
 * without a Move and without a human reading a 128x64 screen.
 *
 *   plugin_params <dsp.so> <module_dir> [op ...]
 *
 * ops:   key            get, print "key = value"
 *        key=value      set
 *        @ms            sleep this many milliseconds
 * With no ops, dumps a default survey of the UI-facing keys.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdint>
#include <ctime>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>

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
    void *reserved[8];
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

/* The child skips every UI service (name tables, preset loads) while its audio
 * ring is full, so a probe that never pulls audio sees a plugin that answers
 * metadata and nothing else. Drain it the way the chain host does. */
struct pump_arg { plugin_api_v2_t *api; void *inst; volatile bool stop; };
/* PLUGIN_PARAMS_NO_PUMP=1 stops draining the plugin's audio, which is what a
 * slot the shim has idled looks like. It existed as a workaround: without the
 * pump the child's ring filled, the child stopped stepping, and every UI
 * service under the throttle stopped with it -- so a bank switch or a preset
 * load never completed. With that fixed, running WITHOUT the pump is the
 * regression test for it. */
static void *render_pump(void *a) {
    pump_arg *p = (pump_arg*)a;
    if (getenv("PLUGIN_PARAMS_NO_PUMP")) return nullptr;
    int16_t buf[256];
    while (!p->stop) { p->api->render_block(p->inst, buf, 128); usleep(2900); }
    return nullptr;
}
static int host_midi_noop(const uint8_t*, int) { return 0; }
static int64_t now_ns() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static const char *g_survey[] = {
    "mode", "part", "panel_select", "bank", "patch", "patch_name", "patch_count",
    "perf_bank", "performance", "performance_name", "performance_count",
    "key_mode", "split_point", "arp_switch", "arp_mode", "arp_beat", "arp_range",
    "arp_hold", "arp_dest", "tempo", "voice_assign", "trigger_note",
    "up_midi_ch", "lo_midi_ch", nullptr
};

static void do_get(plugin_api_v2_t *api, void *inst, const char *key) {
    /* Heap, and big: ui_hierarchy and chain_params are tens of kilobytes, and a
     * short buffer comes back as -1, which looks exactly like "the module does
     * not serve this key". */
    static const size_t BUFSZ = 512 * 1024;
    static char *buf = (char*)malloc(BUFSZ);
    const int n = api->get_param(inst, key, buf, (int)BUFSZ);
    if (n < 0) { printf("  %-20s = <unavailable>\n", key); return; }
    buf[n < (int)BUFSZ ? n : (int)BUFSZ - 1] = '\0';
    if (getenv("JP_FULL")) { printf("%s\n", buf); return; }
    /* PLUGIN_PARAMS_FULL=1 prints the whole value. A state blob is ~1.2 KB and
     * the interesting part is at the END, so the 200-char preview hid exactly
     * what a round-trip check needs to see. */
    if (n > 200 && !getenv("PLUGIN_PARAMS_FULL"))
        printf("  %-20s = (%d bytes) %.200s...\n", key, n, buf);
    else if (n > 200) printf("  %-20s = %s\n", key, buf);
    else printf("  %-20s = %s\n", key, buf);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <dsp.so> <module_dir> [key|key=value|@ms ...]\n", argv[0]); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto init = (plugin_api_v2_t* (*)(const host_api_v1_t*))dlsym(h, "move_plugin_init_v2");
    if (!init) { fprintf(stderr, "no move_plugin_init_v2\n"); return 1; }

    host_api_v1_t host{};
    host.api_version = 1; host.sample_rate = 44100; host.frames_per_block = 128;
    host.log = host_log; host.midi_send_internal = host_midi_noop; host.midi_send_external = host_midi_noop;
    plugin_api_v2_t *api = init(&host);
    if (!api || api->api_version != 2) { fprintf(stderr, "bad plugin api\n"); return 1; }

    void *inst = api->create_instance(argv[2], "{}");
    if (!inst) { fprintf(stderr, "create_instance failed\n"); return 1; }

    char status[1024];
    const int64_t t0 = now_ns();
    /* Not every v2 plugin serves __status -- the chain host does not. Treat an
     * unanswered first read as "no readiness protocol" and carry on, rather
     * than waiting out the timeout on a plugin that is already up. */
    status[0] = '\0';
    if (api->get_param(inst, "__status", status, sizeof(status)) < 0) {
        fprintf(stderr, "no __status; assuming ready\n");
        goto ready;
    }
    for (;;) {
        api->get_param(inst, "__status", status, sizeof(status));
        if (strstr(status, "\"ready\"")) break;
        if (strstr(status, "\"error\"")) { fprintf(stderr, "plugin error: %s\n", status); return 1; }
        if (now_ns() - t0 > 300LL * 1000000000LL) { fprintf(stderr, "boot timeout: %s\n", status); return 1; }
        usleep(100000);
    }
ready:
    fprintf(stderr, "ready after %.1fs\n", (now_ns() - t0) / 1e9);
    /* An op written !key=value is applied IMMEDIATELY after create_instance,
     * before the boot completes -- which is what the chain host does with the
     * slot state, and is a different code path inside the plugin (pending_state
     * applied by the boot thread) from a set_param after it is ready. Without
     * this the harness could only ever exercise the second one. */
    for (int i = 3; i < argc; i++) {
        if (argv[i][0] != '!') continue;
        const char *eq = strchr(argv[i] + 1, '=');
        if (!eq) continue;
        std::string k(argv[i] + 1, eq - argv[i] - 1);
        printf("  early set %s = (%zu bytes)\n", k.c_str(), strlen(eq + 1));
        api->set_param(inst, k.c_str(), eq + 1);
    }

    pump_arg pump{api, inst, false};
    pthread_t pump_th;
    pthread_create(&pump_th, nullptr, render_pump, &pump);
    /* The images fill from DT1 replies at MIDI rate; give them a moment so a
     * survey reads the device rather than "not valid yet". */
    usleep(1500000);

    if (argc == 3) {
        printf("survey:\n");
        for (int i = 0; g_survey[i]; i++) do_get(api, inst, g_survey[i]);
    } else {
        for (int i = 3; i < argc; i++) {
            const char *op = argv[i];
            if (op[0] == '!') continue;   /* already applied before boot */
        if (op[0] == '@') { usleep((useconds_t)atoi(op + 1) * 1000); continue; }
            if (strncmp(op, "note:", 5) == 0) {
                /* note:<on|off>:<n>:<vel>:<ch1based> */
                char which[8]; int nn=60, vv=100, cc=1;
                if (sscanf(op + 5, "%7[^:]:%d:%d:%d", which, &nn, &vv, &cc) >= 2) {
                    uint8_t m[3] = { (uint8_t)((which[1]=='n' ? 0x90 : 0x80) | ((cc-1)&0x0f)),
                                     (uint8_t)nn, (uint8_t)(which[1]=='n' ? vv : 0) };
                    api->on_midi(inst, m, 3, 0);
                    printf("  midi %s note %d ch %d\n", which, nn, cc);
                }
                continue;
            }
            const char *eq = strchr(op, '=');
            if (eq) {
                char key[128];
                const size_t kl = (size_t)(eq - op) < sizeof(key) - 1 ? (size_t)(eq - op) : sizeof(key) - 1;
                memcpy(key, op, kl); key[kl] = '\0';
                api->set_param(inst, key, eq + 1);
                printf("  set %s = %s\n", key, eq + 1);
            } else {
                do_get(api, inst, op);
            }
        }
    }
    pump.stop = true;
    pthread_join(pump_th, nullptr);
    api->destroy_instance(inst);
    return 0;
}
