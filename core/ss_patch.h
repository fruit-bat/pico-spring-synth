#pragma once
//
// ss_patch.h - instrument definitions for the spring synth.
//
// A patch describes the bank of damped springs (partials) a key gets when
// it is struck, and how they are excited:
//
//   pluck   at note on every partial gets a velocity impulse, so it starts
//           at its rest position and swings out to amplitude `gain`
//           (piano hammer, plucked string, mallet). Signed: the sign is the
//           starting phase. Striking a key that is still ringing adds a new
//           impulse to the same springs, as a real string would, after
//           taking away `restrike_damp` of their motion (the hammer or
//           plectrum stops the string it hits).
//   drive   while the key is held each partial is pushed a little in the
//           direction it is already moving (negative friction, like a bow
//           or reed). The push is constant, so the partial settles at the
//           amplitude where it balances the spring's own damping: `drive`.
//           A driven partial starts by itself from rest; it swells with a
//           time constant set by its decay time (organ, strings, pads).
//   noise   a short burst of noise fed into every partial (hammer thump,
//           snare wires, breath) and optionally straight to the output.
//
// All times are T60, the seconds to fall by 60 dB, at middle C (261.6 Hz)
// and scaled by (f / 261.6)^-decay_track for other pitches. Amplitudes are
// in "string units": a loud note's strongest partial is about 1.
//
// The patches live in flash and are only read at note on (and at release
// and choke): everything the render loop needs is copied into the string.
//
#include <stdint.h>
#include <stdbool.h>
#include "ss_config.h"

typedef struct {
    float ratio;        // Frequency / note frequency (before inharmonicity); negative: a fixed
                        // frequency of -ratio Hz whatever the note (a soundboard mode, whose
                        // t60 is not scaled with pitch)
    float gain;         // Pluck amplitude at full velocity (signed); also the noise excitation weight
    float t60;          // Decay time while held (s, at middle C)
    float drive;        // Sustain amplitude while held, 0 = not driven
} SsPartial;

// Patch flags
#define SS_PATCH_ONE_SHOT 1     // Note off is ignored (drums, most percussion)

typedef struct SsPatch {
    const char *name;
    uint8_t count;              // Partials used, 1..SS_MAX_PARTIALS
    uint8_t flags;              // SS_PATCH_*
    SsPartial p[SS_MAX_PARTIALS];
    float gain;                 // Overall level
    float decay_track;          // Decay time scales by (f / 261.6)^-decay_track
    float release;              // Extra damping after key up, as a T60 (s); 0 = none
    float restrike_damp;        // Share of a ringing string's motion absorbed when it is struck
                                // again (a hammer or plectrum stops the string it hits; 0 = none,
                                // repeated strikes add up as on a bell)
    float tilt;                 // Level scales by (261.6 / f)^tilt (clamped to +-18 dB)
    float vel_curve;            // Level = (velocity / 127)^vel_curve
    float bright;               // Partial levels scale by (velocity / 127)^(bright log2 ratio)
    float inharm;               // Stiffness B at middle C: ratio n becomes n sqrt(1 + B n^2) (piano)
    float inharm_track;         // B scales by (f / 261.6)^inharm_track (short stiff treble strings)
    float cutoff_hz;            // Hammer low pass on each partial's frequency (2nd order), at full
                                // velocity; 0 = none
    float cutoff_vel;           // The cutoff falls by this many octaves per octave of velocity
    float body_hz;              // Radiation high pass on each partial's frequency (2nd order): a
                                // soundboard hardly radiates a bass fundamental; 0 = none
    float noise;                // Noise burst into the partials (amplitude reached)
    float noise_out;            // Noise burst straight to the output (high passed)
    float noise_ms;             // Noise burst time constant (ms)
    float knock;                // Unpitched thump at the strike (low passed noise, whatever the
                                // note: the case and soundboard of a piano); amplitude, 0 = none
    float knock_hz;             // Its low pass corner (Hz)
    float knock_ms;             // Its decay time constant (ms)
    float rasp;                 // Buzz: the string's output through a cubic curve, scaled to
                                // the strike, so it fades with the note. Only makes harmonics
                                // of harmonic partials (a plucked quill's rasp); 0 = none
    float quill_ms;             // Pluck delay: the quill bends the string this long before it
                                // slips off and the springs are struck (0 = at once)
    float quill;                // Scrape heard over that delay: bright noise (first difference),
                                // amplitude like a partial's gain; 0 = none
    float glide;                // Pitch starts at this ratio and slides to 1 (0 or 1 = none)
    float glide_ms;             // Glide time constant (ms)
    float vibrato;              // Built-in vibrato depth (cents), added to the mod wheel's
    float vib_rate;             // Vibrato rate (Hz); 0 = 5.5 Hz
    float vib_delay;            // Vibrato fades in over this time (s)
    float pan_spread;           // Low keys left, high keys right: 0..1
    float hz;                   // Fixed pitch (drums); 0 = from the key
    // Stretched tuning, as pianos are tuned: keys above stretch_key are
    // sharpened by stretch x (octaves above it)^2 cents. 0 = equal temperament
    float stretch;
    uint8_t stretch_key;
    // Blend into another patch towards the top: below key blend_lo this
    // patch sounds, above blend_hi `upper` does, and in between every
    // number (partial levels and decays, filters, gain...) is interpolated
    // per key, so there is no seam. `upper` must have the same count and
    // partial ratios. NULL = no blend.
    const struct SsPatch *upper;
    uint8_t blend_lo, blend_hi;
} SsPatch;

// Drum kit entry for one key on the drum channel
typedef struct {
    uint8_t patch;              // Index + 1 into ss_drum_patches, 0 = silent key
    uint8_t group;              // Choke group: a hit silences ringing strings of the same group (0 = none)
    float hz;                   // Pitch (0 = the patch's own)
    float pan;                  // -1 left .. +1 right
} SsDrumKey;

// The melodic patches, indexed by ss_gm_patch[program]
extern const SsPatch ss_patches[];
extern const uint32_t ss_patch_count;

// GM program (0..127) -> index into ss_patches
extern const uint8_t ss_gm_patch[128];

// The drum patches and the GM drum map (keys 0..127)
extern const SsPatch ss_drum_patches[];
extern const uint32_t ss_drum_patch_count;
extern const SsDrumKey ss_drum_map[128];

// Index of the "classic" patch in ss_patches: a replica of the original
// Java spring synth (ShoSynQ). Selected by bank select MSB 1.
extern const uint8_t ss_classic_patch;
