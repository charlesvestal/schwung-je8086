/*
 * JP-8000 DSP Plugin for Schwung
 *
 * Runs the JE-8086 (Roland JP-8000) emulator using fork-parallel processing:
 *   Parent process: H8S microcontroller + ASIC0+1 (core 2)
 *   Child process:  ASIC2+3 direct loop (core 3)
 * Communication via mmap'd shared memory SPSC ring buffers.
 *
 * GPL-3.0 License
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <sched.h>
#include <stdarg.h>

/* Plugin API */
typedef struct {
    void (*log)(const char *fmt, ...);
} host_api_v1_t;

typedef struct {
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void  (*destroy_instance)(void *instance);
    void  (*render_block)(void *instance, int16_t *out, int frames);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_error)(void *instance, char *buf, int buf_len);
} plugin_api_v2_t;

/* =====================================================================
 * Stub implementation — placeholder until full plugin is built
 * ===================================================================== */

static const host_api_v1_t *g_host = nullptr;

struct jp8000_instance_t {
    int initialized;
    char module_dir[256];
    char error[256];
};

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    auto *inst = new jp8000_instance_t();
    memset(inst, 0, sizeof(*inst));
    snprintf(inst->module_dir, sizeof(inst->module_dir), "%s", module_dir);
    snprintf(inst->error, sizeof(inst->error), "JP-8000 plugin not yet implemented");
    if (g_host && g_host->log)
        g_host->log("[jp8000] create_instance: %s", module_dir);
    return inst;
}

static void v2_destroy_instance(void *instance) {
    delete static_cast<jp8000_instance_t*>(instance);
}

static void v2_render_block(void *instance, int16_t *out, int frames) {
    memset(out, 0, frames * 2 * sizeof(int16_t));
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance; (void)msg; (void)len; (void)source;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    auto *inst = static_cast<jp8000_instance_t*>(instance);
    if (strcmp(key, "__ui") == 0) {
        return snprintf(buf, buf_len, "{\"pages\":[]}");
    }
    if (strcmp(key, "__status") == 0) {
        return snprintf(buf, buf_len, "{\"status\":\"loading\",\"message\":\"Not yet implemented\"}");
    }
    return snprintf(buf, buf_len, "0");
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    (void)instance; (void)key; (void)val;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    auto *inst = static_cast<jp8000_instance_t*>(instance);
    if (inst->error[0])
        return snprintf(buf, buf_len, "%s", inst->error);
    return 0;
}

static plugin_api_v2_t g_plugin_api_v2;

extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.render_block = v2_render_block;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_error = v2_get_error;
    return &g_plugin_api_v2;
}
