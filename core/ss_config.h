#pragma once
//
// ss_config.h - compile time configuration of the spring synth core.
//
// Every core file includes this. Override any #ifndef'd value with -D on
// the command line (or target_compile_definitions in CMake); the firmware
// and the host tools may use different values. The rest are derived
// values and checks.
//
// Cost model, measured on the RP2350 (firmware/bench.c): a sounding
// partial (spring) costs about 11 CPU cycles per output sample, 15 if it
// is driven; a string costs about 35 more for its panning, mixing and
// housekeeping. The render cost is budgeted in units of one partial, a
// string counting SS_STRING_COST (the firmware's CMakeLists.txt scales the
// budget with the clock). See ss_synth.h for how strings are allocated.
//

// Strings (one per sounding channel + key) that may ring at once
#ifndef SS_MAX_STRINGS
#define SS_MAX_STRINGS 48
#endif

// Stolen strings fading out at once (SS_FADE_BLOCKS each), in extra
// slots. Their cost is outside the budget, so this bounds the overshoot;
// a steal beyond it cuts the string that has faded furthest instead
// (with a declick).
#ifndef SS_STEAL_RESERVE
#define SS_STEAL_RESERVE 4
#endif

// Blocks a stolen string takes to fade out (4 = 2.9 ms at 44.1 kHz)
#ifndef SS_FADE_BLOCKS
#define SS_FADE_BLOCKS 4
#endif

#define SS_TOTAL_STRINGS (SS_MAX_STRINGS + SS_STEAL_RESERVE)

// Most partials (damped springs) one string may have
#ifndef SS_MAX_PARTIALS
#define SS_MAX_PARTIALS 8
#endif

// The CPU budget: partials + SS_STRING_COST x strings, over all strings
// that are not fading out after a steal. A note on that would go over it
// steals the quietest strings first.
#ifndef SS_COST_BUDGET
#define SS_COST_BUDGET 280
#endif

// What a string costs on its own, in partials
#ifndef SS_STRING_COST
#define SS_STRING_COST 3
#endif

// Partials above this frequency (Hz) are left out at note on: hardly
// audible, and they cost as much as any other. Never more than about
// 0.46 x the sample rate (aliasing).
#ifndef SS_MAX_PARTIAL_HZ
#define SS_MAX_PARTIAL_HZ 16000.0f
#endif

// Frames rendered per block. Control-rate values (pitch, pan, volume,
// vibrato, glide) are updated once per block.
#ifndef SS_BLOCK_SIZE
#define SS_BLOCK_SIZE 32
#endif

// Highest sample rate the effects buffers are sized for
#ifndef SS_MAX_SAMPLE_RATE
#define SS_MAX_SAMPLE_RATE 48000
#endif

// Reverb and chorus can be compiled out to save RAM and CPU
#ifndef SS_ENABLE_REVERB
#define SS_ENABLE_REVERB 1
#endif
#ifndef SS_ENABLE_CHORUS
#define SS_ENABLE_CHORUS 1
#endif

// A partial is dropped once its amplitude falls below this (string units,
// where a loud note's fundamental is around 1). About -90 dB.
#ifndef SS_SILENCE
#define SS_SILENCE 3e-5f
#endif

// String rendering can be split into this many parts, one per core
#ifndef SS_RENDER_PARTS
#define SS_RENDER_PARTS 2
#endif

// Part 0 also runs the effects and the final mix, so it can be given this
// many fewer cost units than the other parts. On the Pico core 0 renders
// the other part in an interrupt alongside USB, and measured with a dense
// song that costs it about as much as the effects cost core 1, so 0
// balances the two cores best there.
#ifndef SS_RENDER_EFFECTS_COST
#define SS_RENDER_EFFECTS_COST 0
#endif

// Place time critical functions in RAM on the Pico. Wrap the function name:
// void SS_RAM_FUNC(name)(args) { ... }. Code in RAM does not stall on
// flash cache misses, and keeps running while flash is being written. A
// no-op on the host.
#ifdef PICO_ON_DEVICE
#include "pico.h"
#define SS_RAM_FUNC(f) __not_in_flash_func(f)
#else
#define SS_RAM_FUNC(f) f
#endif

// CPU cycle counter for profiling (the DWT counter on the M33, which the
// firmware enables on each core). Always 0 on the host.
#ifndef SS_CYCLE_COUNT
#if defined(PICO_ON_DEVICE) && PICO_RP2350
#include "hardware/structs/m33.h"
#define SS_CYCLE_COUNT() (m33_hw->dwt_cyccnt)
#else
#define SS_CYCLE_COUNT() 0u
#endif
#endif

#define SS_MIDI_CHANNELS 16
#define SS_DRUM_CHANNEL 9

_Static_assert(SS_TOTAL_STRINGS < 255, "string indices are 8 bit (0 = none)");
_Static_assert(SS_MAX_PARTIALS <= 16, "partial counts are small");
