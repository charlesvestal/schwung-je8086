#pragma once
#include <stdint.h>

#define JE_RING_CAPACITY 1024
#define JE_RING_MASK     (JE_RING_CAPACITY - 1)
#define JE_GRAM_HANDOFF_COUNT 6

struct je_gram_sample_t {
    int32_t gram[JE_GRAM_HANDOFF_COUNT];
};

struct je_audio_sample_t {
    int32_t left;
    int32_t right;
};

struct je_fork_shm_t {
    /* GRAM ring: parent writes, child reads */
    je_gram_sample_t gram_ring[JE_RING_CAPACITY];
    volatile int gram_write;
    volatile int gram_read;

    /* Audio ring: child writes, parent reads */
    je_audio_sample_t audio_ring[JE_RING_CAPACITY];
    volatile int audio_write;
    volatile int audio_read;

    /* Control flags */
    volatile int child_ready;
    volatile int child_shutdown;
    volatile int child_alive;

    /* Profiling */
    volatile int64_t child_samples_produced;
    volatile int64_t parent_samples_produced;
    volatile int child_underruns;
    volatile int parent_underruns;
};
