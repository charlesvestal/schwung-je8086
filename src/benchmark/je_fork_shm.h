#pragma once
#include <stdint.h>

#define JE_RING_CAPACITY 1024
#define JE_RING_MASK     (JE_RING_CAPACITY - 1)
#define JE_GRAM_HANDOFF_COUNT 8   /* widest supported boundary payload */
#define JE_UC_RING_CAP 8192

struct je_gram_sample_t {
    int32_t gram[JE_GRAM_HANDOFF_COUNT];
};

struct je_audio_sample_t {
    int32_t left;
    int32_t right;
};

/* A forwarded H8S register write. `sample` is the parent's sample index at the
 * time of the write: the write precedes that sample's render, so the child must
 * apply it before rendering the same sample — not when it happens to arrive,
 * which is up to a ring's worth early when the parent runs ahead. */
struct je_uc_write_t {
    uint8_t asic; uint8_t val; uint16_t addr;
    uint32_t sample;
};

struct je_fork_shm_t {
    /* H8S -> child-owned ASIC register writes: parent writes, child reads */
    je_uc_write_t uc_ring[JE_UC_RING_CAP];
    volatile int uc_write;
    volatile int uc_read;
    /* Child-owned ASIC readback regs, exported for the parent's H8S */
    volatile uint8_t readback[4][4];

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
    /* Time each side spent spinning on the other (ns). busy = elapsed - wait. */
    volatile int64_t child_gram_wait_ns;
    volatile int64_t child_audio_wait_ns;
    volatile int64_t parent_gram_wait_ns;
};
