//
// bench.c - on-device micro benchmarks of the render (SS_BENCH=ON).
//
// Runs once on core 0 after the USB console connects, before the audio
// starts, on a private synth instance, and prints CPU cycles per output
// sample for: the effects alone, and N strings of P undriven / driven /
// vibrato partials. From these the per-partial and per-string costs follow,
// which is what SS_COST_BUDGET, SS_STRING_COST and SS_MAX_STRINGS are sized by.
//
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "ss_synth.h"
#include "bench.h"

static SsSynth bench_synth;

#define BENCH_BLOCKS 200

// Cycles per sample to render BENCH_BLOCKS blocks on this core
static float time_render(SsSynth *s) {
    float l[SS_BLOCK_SIZE], r[SS_BLOCK_SIZE];
    ss_synth_render_float(s, l, r); // Warm up (first coefficient update)
    const uint32_t t0 = SS_CYCLE_COUNT();
    for (int b = 0; b < BENCH_BLOCKS; ++b) ss_synth_render_float(s, l, r);
    return (float)(SS_CYCLE_COUNT() - t0) / (BENCH_BLOCKS * SS_BLOCK_SIZE);
}

// Fresh synth with `strings` strings of a `partials` partial test patch
static float run(bool effects, uint32_t strings, uint32_t partials, bool drive, bool vibrato) {
    static SsPatch p;
    memset(&p, 0, sizeof(p));
    p.name = "bench";
    p.count = (uint8_t)partials;
    p.gain = 0.1f;
    p.vel_curve = 1.0f;
    p.vibrato = vibrato ? 10.0f : 0.0f;
    for (uint32_t i = 0; i < partials; ++i) {
        p.p[i] = (SsPartial){ (float)(i + 1), 0.5f, 60.0f, drive ? 0.5f : 0.0f };
    }
    SsSynth *s = &bench_synth;
    ss_synth_init(s, SS_SAMPLE_RATE);
    ss_synth_set_effects(s, effects, effects);
    for (uint32_t i = 0; i < strings; ++i) {
        ss_synth_note_on_patch(s, (uint8_t)(i / 32), (uint8_t)(36 + i % 32), 100, &p, 0, 0, 0);
    }
    return time_render(s);
}

void bench_run(void) {
    printf("bench: cycles per sample on one core (%lu available per core)\n",
           (unsigned long)(SS_SYS_CLOCK_KHZ * 1000ull / SS_SAMPLE_RATE));
    const float fx = run(true, 0, 0, false, false);
    const float none = run(false, 0, 0, false, false);
    printf("  empty: effects on %.0f, off %.0f\n", (double)fx, (double)none);
    static const uint32_t pcount[] = { 1, 2, 4, 8 };
    float c16[4];
    for (int i = 0; i < 4; ++i) {
        c16[i] = run(false, 16, pcount[i], false, false);
        printf("  16 strings x %lu partials: %.0f (%.1f per string)\n", (unsigned long)pcount[i],
               (double)c16[i], (double)((c16[i] - none) / 16));
    }
    const float c32 = run(false, 32, 8, false, false);
    printf("  32 strings x 8 partials: %.0f\n", (double)c32);
    const float per_partial = (c16[3] - c16[0]) / (16 * 7);
    const float per_string = (c16[0] - none) / 16 - per_partial;
    printf("  => per partial %.1f, per string %.1f\n", (double)per_partial, (double)per_string);
    const float drv = run(false, 16, 8, true, false);
    const float vib = run(false, 16, 8, false, true);
    // Note on cost: 32 piano notes into a fresh synth
    {
        SsSynth *s = &bench_synth;
        ss_synth_init(s, SS_SAMPLE_RATE);
        const uint32_t t0 = SS_CYCLE_COUNT();
        for (uint8_t k = 0; k < 32; ++k) ss_synth_note_on(s, 0, (uint8_t)(40 + k), 100);
        const uint32_t t1 = SS_CYCLE_COUNT();
        for (uint8_t k = 0; k < 32; ++k) ss_synth_note_on(s, 0, (uint8_t)(40 + k), 100);
        const uint32_t t2 = SS_CYCLE_COUNT();
        printf("  note on (piano, 8 partials): new string %lu cycles, re-strike %lu cycles (a block is %lu)\n",
               (unsigned long)((t1 - t0) / 32), (unsigned long)((t2 - t1) / 32),
               (unsigned long)(SS_SYS_CLOCK_KHZ * 1000ull * SS_BLOCK_SIZE / SS_SAMPLE_RATE));
    }
    printf("  16 x 8 driven: %.0f (+%.1f per partial)   16 x 8 vibrato: %.0f (+%.1f per string)\n",
           (double)drv, (double)((drv - c16[3]) / 128), (double)vib, (double)((vib - c16[3]) / 16));
}
