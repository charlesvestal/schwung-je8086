/*
 * jp8000-dsp: the JE-8086 DSP process.
 *
 * Started by dsp.so with posix_spawn (see spawn_dsp_process in
 * jp8000_plugin.cpp) instead of being forked from the host, so it shares no
 * memory with MoveOriginal. It loads the same dsp.so and runs its child_main
 * through the one exported entry point; the shared-memory block arrives as a
 * memfd on fd 3 and the pipeline lock on fd 4.
 *
 * Usage: jp8000-dsp <path-to-dsp.so>
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "jp8000-dsp: usage: jp8000-dsp <dsp.so>\n");
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "jp8000-dsp: %s\n", dlerror());
        return 3;
    }
    int (*entry)(int) = (int (*)(int))dlsym(h, "jp8000_child_entry");
    if (!entry) {
        fprintf(stderr, "jp8000-dsp: %s has no jp8000_child_entry\n", argv[1]);
        return 4;
    }
    return entry(3);
}
