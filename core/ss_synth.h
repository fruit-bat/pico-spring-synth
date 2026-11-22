#pragma once
//
// ss_synth.h - the spring synthesizer: 16 MIDI channels playing banks of
// damped springs, with mixing and effects. Feed it MIDI messages and pull
// blocks of stereo audio. This is the core's public API.
//
// Typical use:
//   ss_synth_init(&synth, 44100);
//   loop:
//     ss_synth_midi(&synth, msg, len);      // any number of messages
//     ss_synth_render(&synth, out);         // SS_BLOCK_SIZE stereo frames
//
// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
//
// A "string" is what one key of one channel sounds: a small bank of damped
// springs (partials, up to SS_MAX_PARTIALS) whose frequencies, levels,
// decay times and excitation come from the channel's patch (ss_patch.h).
// Each spring is simulated exactly, not stepped: its state is a complex
// number z = x + i y (x the displacement, y = -v / omega, so |z| is the
// amplitude), and every sample it is multiplied by r e^(i omega T), a
// rotation that also decays. That is 4 multiplies and 2 adds per sample,
// unconditionally stable and exactly in tune at any pitch and sample rate.
// The output is the sum of the displacements.
//
// Excitations are added to the state: a pluck is a velocity impulse, a
// noise burst is random pushes. A drive (see ss_patch.h) is a small push
// in the direction of motion every sample; its effect on the amplitude
// over a block is known exactly (an exponential approach to the amplitude
// where push and damping balance), so it is applied once per block as the
// rotation's magnitude rather than per sample.
//
// ---------------------------------------------------------------------------
// Allocation
// ---------------------------------------------------------------------------
//
// Strings come from a pool and are found by channel and key. Striking a
// key whose string is still ringing (even after its note off) excites the
// same springs again instead of starting a new voice: repeated notes build
// on each other the way a real string does, and cost nothing extra.
//
// A string needs no envelope to know when it is finished: each partial is
// dropped as soon as its amplitude falls below SS_SILENCE, and the string
// is freed when it has none left. As upper partials die out first, the
// cost of a string falls as it decays.
//
// The limits are SS_MAX_STRINGS strings and SS_COST_BUDGET (the CPU
// budget: partials, plus SS_STRING_COST per string). A note on that would go over either steals the string with
// the least energy (exactly known from the spring states, weighted so held
// notes are kept over released ones). A stolen string fades out over
// SS_FADE_BLOCKS blocks (3 ms), at most SS_STEAL_RESERVE at a time, so
// steals do not click.
//
// ---------------------------------------------------------------------------
//
// Threading: the synth is not thread safe. All calls must come from one
// thread (on the Pico, the render core), except ss_synth_render_part,
// which is designed to run concurrently on several cores (see below).
// MIDI is applied between blocks, so its timing is quantised to the block
// size (32 frames = 0.7 ms at 44.1 kHz).
//
// Memory: SsSynth is large (the strings and the reverb), so make it
// static, never a local.
//
#include <stdint.h>
#include <stdbool.h>
#include "ss_config.h"
#include "ss_patch.h"
#include "ss_effects.h"

// One damped spring. The rotation (a, b) includes the decay r and the
// current pitch; it is recomputed when either changes. A driven spring
// gets a new rotation magnitude every block instead of r (see
// string_render).
typedef struct {
    float re, im;       // State: re = displacement, im = -velocity / omega
    float a, b;         // Per-sample rotation this block: |(a, b)| = r (or the drive's)
    float c, s;         // Unit rotation: cos(w p), sin(w p)
    float w;            // Radians per sample at pitch factor p = 1
    float r;            // Per-sample decay now
    float r_rel;        // Per-sample decay after the key is released
    float rb;           // r^SS_BLOCK_SIZE
    float drive;        // Amplitude the drive holds it at while held (0 = not driven)
    float exc;          // Weight of the noise burst
    uint8_t idx;        // Which of the patch's partials this is
} SsSpring;

// String states
enum {
    SS_STR_HELD,        // Key down
    SS_STR_SUSTAINED,   // Key up, sustain pedal down: still held
    SS_STR_RELEASED,    // Key up: drive off, release damping on
    SS_STR_DYING,       // Stolen: fading out fast, no longer reachable by key
};

typedef struct {
    SsSpring sp[SS_MAX_PARTIALS];
    uint8_t count;          // Springs in use (sp[0..count))
    uint8_t chan, key;
    uint8_t state;          // SS_STR_*
    bool mapped;            // Reachable by channel + key (SsSynth.key_string)
    uint8_t group;          // Drum choke group (0 = none)
    bool dirty;             // Rotations must be recomputed
    uint8_t fade;           // Dying: blocks of fade left (0 = cut, silent)
    const SsPatch *patch;
    float pitch;            // Pitch factor the rotations were computed for
    float glide;            // Current glide ratio (1 = none)
    float glide_k;          // Per-block glide factor
    float vib_depth;        // Built-in vibrato depth (octaves)
    float vib_phase;        // Turns
    float vib_step;         // Turns per block
    float vib_env;          // Vibrato fade in, 0..1
    float vib_env_step;     // Per block
    float pan;              // -1 .. +1 (key spread or drum map)
    float gl, gr;           // Output gains used for the last block (ramped)
    float last_l, last_r;   // Last output sample, for the declick when cut
    float noise_env;        // Noise burst level now (0 = none)
    float noise_decay;      // Per sample
    float noise_scale;      // Excitation scale for the burst
    float noise_out;        // Direct noise level
    float noise_prev;       // High pass state
    float noise_lp;         // Low pass coefficient (cutoff follows the note)
    float noise_y;          // Low pass state
    float knock_env;        // Knock level now (0 = none)
    float knock_decay;      // Per sample
    float knock_floor;      // It stops below this (40 dB under its start)
    float knock_c;          // Two pole low pass coefficient
    float knock_y1, knock_y2;   // Low pass state
    float rasp;             // The patch's rasp (0 = none)
    float rasp_g;           // Level correction for it
    uint8_t pend_blocks;    // Blocks until the pending pluck lands (0 = none pending)
    float pend[SS_MAX_PARTIALS];    // Pending pluck per spring slot (added to im)
    float quill_amp;        // Scrape level while the pluck is pending
    float quill_prev;       // Its first difference state
    uint32_t noise_seed;
    float energy;           // Sum of |z|^2 over the springs, last block
} SsString;

// Per-channel state
typedef struct {
    uint8_t cc[128];
    uint8_t program;
    uint8_t bank;               // Bank select MSB (1 = classic patch)
    uint8_t pressure;
    bool drums;
    uint16_t bend;              // 0..16383
    uint8_t bend_range;         // Semitones (RPN 0)
    uint8_t rpn_msb, rpn_lsb;   // Selected RPN (127 = none)
    float bend_factor;          // Pitch factor from the bend
    float gain;                 // Volume and expression
    float pan;                  // -1 .. +1
    float reverb, chorus;       // Sends 0..1
    float mod_depth;            // Mod wheel vibrato depth (octaves)
    float drive_scale;          // Channel pressure: drive multiplier
} SsChannel;

// Counters since ss_synth_init, for status reports and tuning
typedef struct {
    uint32_t blocks;
    uint32_t notes;             // Note ons that sounded
    uint32_t strings_started;   // New strings
    uint32_t restrikes;         // Note ons that re-excited a ringing string
    uint32_t steals;            // Strings stolen
    uint32_t cuts;              // Steals that had to cut a string (no spare slot to fade it)
    uint32_t peak_strings;      // Most strings at once, including fading ones
    uint32_t peak_partials;     // Most partials at once
    uint32_t peak_cost;         // Highest ss_synth_cost
    uint32_t clipped;           // Output samples beyond full scale (soft clipped)
    float peak;                 // Largest output magnitude (1.0 = full scale); reset by the user
    // Profiling, per render part, CPU cycles (device only; 0 on the host)
    uint32_t part_cycles[SS_RENDER_PARTS];
    uint32_t partial_blocks[SS_RENDER_PARTS];  // Partial-blocks rendered
} SsSynthStats;

typedef struct {
    float sample_rate;
    float gain;                 // Master gain, see ss_synth_set_gain
    float master_volume;        // GM Master Volume SysEx, 0..1 (default 1)
    bool reverb_enabled;
    bool chorus_enabled;
    int8_t force_bank;          // >= 0: every melodic channel uses this bank (host tools)

    SsChannel channels[SS_MIDI_CHANNELS];

    SsString strings[SS_TOTAL_STRINGS];
    // Indices of the strings in use, in no particular order (freeing swaps
    // the last entry into the gap). Rendering walks this list.
    uint8_t active[SS_TOTAL_STRINGS];
    uint32_t active_count;
    uint32_t dying_count;       // Active strings fading out after a steal
    uint32_t partials;          // Partials of the active strings that are not dying
    uint8_t free_slots[SS_TOTAL_STRINGS];
    uint32_t free_count;
    // String index + 1 for each channel and key, 0 = none
    uint8_t key_string[SS_MIDI_CHANNELS][128];

    // Declick: a string that has to be cut off leaves its last output
    // value here, decaying quickly, so the output has no step
    float declick_l, declick_r, declick_decay;
    float choke_decay;          // Per-sample decay of a choked drum
    uint32_t noise_seed;

    // Render split: part p renders active[part_first[p] .. part_first[p+1])
    uint32_t part_first[SS_RENDER_PARTS + 1];
    float bus_l[SS_RENDER_PARTS][SS_BLOCK_SIZE];
    float bus_r[SS_RENDER_PARTS][SS_BLOCK_SIZE];
    float bus_reverb[SS_RENDER_PARTS][SS_BLOCK_SIZE];
    float bus_chorus[SS_RENDER_PARTS][SS_BLOCK_SIZE];
    uint8_t finished[SS_TOTAL_STRINGS];     // Per active index, set while rendering
    // Effect sends summed over all parts, processed one block later in part 0
    float send_reverb[SS_BLOCK_SIZE];
    float send_chorus[SS_BLOCK_SIZE];

#if SS_ENABLE_REVERB
    SsReverb reverb;
#endif
#if SS_ENABLE_CHORUS
    SsChorus chorus;
#endif
    SsEq eq;                    // Master EQ (GS SysEx 40 02 0x)
    uint8_t eq_gs[4];           // GS EQ parameters: low freq, low gain, high freq, high gain
    uint8_t width_pct;          // Stereo width in percent, 0..100 (default 100)
    float width;
    float dc_coef;              // DC blocker pole
    float dc_x[2], dc_y[2];     // DC blocker state, left and right

    SsSynthStats stats;
} SsSynth;

// Set up the synth at a sample rate of at most SS_MAX_SAMPLE_RATE. All
// channels get their GM defaults, nothing sounds, effects are on, EQ flat,
// master volume full. Call once; use ss_synth_reset to return to defaults.
void ss_synth_init(SsSynth *s, uint32_t sample_rate);

// Master gain (linear), default SS_DEFAULT_GAIN.
#define SS_DEFAULT_GAIN 0.25f
static inline void ss_synth_set_gain(SsSynth *s, float gain) { s->gain = gain; }

// Master volume, as set by the GM Master Volume SysEx (F0 7F dev 04 01 lsb
// msb F7): 0..16383, 16383 (the default) = full. Not changed by resets.
static inline void ss_synth_set_master_volume(SsSynth *s, uint16_t value) {
    s->master_volume = (float)(value > 16383 ? 16383 : value) * (1.0f / 16383.0f);
}

// Master EQ, as the Roland GS master EQ: low shelf 200 or 400 Hz, high shelf
// 3 or 6 kHz, gains -12..+12 dB. Flat by default; not changed by resets.
void ss_synth_set_eq(SsSynth *s, float low_hz, float low_db, float high_hz, float high_db);
// GS EQ parameter (address 40 02 0p): p = 0 low freq (0/1), 1 low gain
// (0x34..0x4c, 0x40 = 0 dB), 2 high freq (0/1), 3 high gain
void ss_synth_set_eq_gs(SsSynth *s, uint8_t param, uint8_t value);

// Stereo width, 0..100 percent: scales the difference between left and
// right of the final mix. Not changed by resets.
void ss_synth_set_width(SsSynth *s, uint8_t percent);

// Switch effects at runtime (when compiled in). Default on.
static inline void ss_synth_set_effects(SsSynth *s, bool reverb, bool chorus) {
    s->reverb_enabled = reverb;
    s->chorus_enabled = chorus;
}
// Reverb: room size 0..1, damping 0..1, width 0..1, level 0..1
void ss_synth_set_reverb(SsSynth *s, float room, float damp, float width, float level);
// Chorus: LFO rate in Hz, modulation depth in ms, level 0..
void ss_synth_set_chorus(SsSynth *s, float rate_hz, float depth_ms, float level);

// MIDI channel voice messages. chan is 0..15; out of range channels and
// keys are ignored. They take effect at the next rendered block.
//
// Note on: excites the key's string (a new one, or the one still ringing
// on that key). Velocity 0 is a note off.
void ss_synth_note_on(SsSynth *s, uint8_t chan, uint8_t key, uint8_t vel);
// Same with an explicit patch instead of the channel's (tests, auditions).
// pan is -1..1, added to the channel's pan; group is a choke group (0 = none).
void ss_synth_note_on_patch(SsSynth *s, uint8_t chan, uint8_t key, uint8_t vel,
                            const SsPatch *patch, float hz, float pan, uint8_t group);
// Note off: releases the key's string, or marks it sustained while the
// sustain pedal (CC64) is down. Ignored for one-shot patches (drums).
void ss_synth_note_off(SsSynth *s, uint8_t chan, uint8_t key);
// Control change: volume (7), pan (10), expression (11), modulation (1),
// sustain (64), reverb / chorus send (91 / 93), bank select (0), RPN 0
// pitch bend range (101/100 then 6), all sound off (120), reset all
// controllers (121), all notes off (123-127).
void ss_synth_control_change(SsSynth *s, uint8_t chan, uint8_t cc, uint8_t value);
// Program change: patch for the channel's next notes (ss_gm_patch). Bank
// MSB 1 selects the classic patch for every program.
void ss_synth_program_change(SsSynth *s, uint8_t chan, uint8_t program);
void ss_synth_pitch_bend(SsSynth *s, uint8_t chan, uint16_t value);  // 0..16383, 8192 = centre
// Channel pressure: scales the drive of driven patches (up to 2x)
void ss_synth_channel_pressure(SsSynth *s, uint8_t chan, uint8_t value);
// Fade out every string within a few ms.
void ss_synth_all_sound_off(SsSynth *s);
void ss_synth_reset(SsSynth *s);    // GM reset: controllers, programs, sound off

// Dispatch a complete MIDI message (status byte first, no running status)
// to the functions above. Also understands GM System On, GM2 System On and
// GS Reset (ss_synth_reset), GM Master Volume, GS master EQ and the stereo
// width SysEx F0 7D 53 53 03 ww F7. Others are ignored.
void ss_synth_midi(SsSynth *s, const uint8_t *msg, uint32_t len);

// The patch a note on would use for this channel and key (NULL for a
// silent drum key).
const SsPatch *ss_synth_channel_patch(const SsSynth *s, uint8_t chan, uint8_t key);

// Render SS_BLOCK_SIZE stereo frames as interleaved int16 (L, R, L, R...).
// The output passes a soft clipper; samples beyond full scale before it
// are counted in stats.clipped.
void ss_synth_render(SsSynth *s, int16_t *out);
// Same, as float in [-1, 1]
void ss_synth_render_float(SsSynth *s, float *left, float *right);

// Rendering split across cores. Per block:
//   ss_synth_render_begin(s);                  owner core
//   ss_synth_render_part(s, p);                p = 0 .. SS_RENDER_PARTS-1, any core,
//                                              parts may run concurrently
//   ss_synth_render_end(s, out);               owner core, after all parts finish
// No other synth call may happen between begin and end. render_begin
// divides the active strings into contiguous ranges with about the same
// number of partials each; part 0 also runs reverb and chorus on the
// previous block's sends, so it gets SS_RENDER_EFFECTS_COST less.
void ss_synth_render_begin(SsSynth *s);
void ss_synth_render_part(SsSynth *s, uint32_t part);
void ss_synth_render_end(SsSynth *s, int16_t *out);
void ss_synth_render_end_float(SsSynth *s, float *left, float *right);

// Strings in use, including ones fading out after a steal
static inline uint32_t ss_synth_active_strings(const SsSynth *s) { return s->active_count; }
// Partials sounding in strings that are not fading out
static inline uint32_t ss_synth_active_partials(const SsSynth *s) { return s->partials; }
// Render cost now, in the units of SS_COST_BUDGET
static inline uint32_t ss_synth_cost(const SsSynth *s) {
    return s->partials + SS_STRING_COST * (s->active_count - s->dying_count);
}
