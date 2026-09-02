#pragma once
#include <stdint.h>

#define JE_RING_CAPACITY 1024
#define JE_RING_MASK     (JE_RING_CAPACITY - 1)
#define JE_GRAM_HANDOFF_COUNT 10  /* widest boundary payload: 2->3 is 8 words + 0xa0/0xa2 */
#define JE_UC_RING_CAP 8192
#define JE_MAX_STAGES 4           /* stage 0 = parent (H8S + ASICs below the first boundary) */

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

/* One pipeline stage. Stage s owns ASICs [lo, hi); its INPUT rings are written
 * by stage s-1 (gram) and by the parent (uc writes, only for ASICs it owns).
 * The parent's own entry (stage 0) uses only samples_produced. */
struct je_stage_shm_t {
    int lo, hi;
    volatile int ready;
    volatile int alive;
    /* H8S -> this stage's ASIC register writes: parent writes, stage reads */
    je_uc_write_t uc_ring[JE_UC_RING_CAP];
    volatile int uc_write;
    volatile int uc_read;
    /* GRAM handoff in from the previous stage */
    je_gram_sample_t gram_ring[JE_RING_CAPACITY];
    volatile int gram_write;
    volatile int gram_read;
    /* Samples this stage has published to the next stage (or to audio) */
    volatile int64_t samples_produced;
    /* Time spent spinning (ns): on input from upstream, on output space downstream */
    volatile int64_t in_wait_ns;
    volatile int64_t out_wait_ns;
};

struct je_fork_shm_t {
    int num_stages;
    je_stage_shm_t stage[JE_MAX_STAGES];

    /* Child-owned ASIC readback regs, exported for the parent's H8S */
    volatile uint8_t readback[4][4];

    /* Audio ring: last stage writes, parent reads */
    je_audio_sample_t audio_ring[JE_RING_CAPACITY];
    volatile int audio_write;
    volatile int audio_read;

    /* Control flags */
    volatile int child_shutdown;
};
