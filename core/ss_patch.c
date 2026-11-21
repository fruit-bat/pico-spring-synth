//
// ss_patch.c - the built-in patches, the GM program map and the drum kit.
//
// See ss_patch.h for what each field means. Partials are written
// P(ratio, gain, t60, drive). Fields left out are 0, which means "off" for
// everything except vel_curve (0 = 1.5) and vib_rate (0 = 5.5 Hz).
//
// The models, roughly:
//   struck / plucked  harmonic partials with gains from where the string
//                     is struck, upper partials dying faster (piano,
//                     guitars, harp, harpsichord)
//   bars and bells    the inharmonic modes of free bars (2.756, 5.404 ...),
//                     tuned bars (marimba 1 : 4 : 10) and bells (hum,
//                     prime, tierce, quint, nominal ...)
//   bowed / blown     driven partials whose drive levels give the spectrum
//                     (about 1/n for bowed strings and brass, odd only for
//                     the clarinet and square leads) and whose held decay
//                     time sets how fast they speak
//   drums             membrane modes (1, 1.59, 2.14, 2.30 ...) with a pitch
//                     drop, metal clusters fed by noise, snare wires
//
#include "ss_patch.h"

#define P(r, g, t, d) { (r), (g), (t), (d) }

enum {
    PIANO, PIANO_UPPER, EPIANO, HARPSI, CLAV, MALLET, MARIMBA, BELL,
    ORGAN, CHURCH, ACCORDION,
    NYLON, STEEL, MUTED, OVERDRIVE,
    BASS, SYNBASS,
    STRINGS, ENSEMBLE, PIZZ, HARP, TIMPANI,
    CHOIR, BRASS, REED, CLARINET, FLUTE,
    LEAD, PAD, SPACE,
    STEELDRUM, WOODBLOCK, TOM,
    CLASSIC,
    PATCH_COUNT
};

const SsPatch ss_patches[] = {
    // The piano is two patches blended by key: A up to C4, B from C5 (you
    // preferred A low and B high), interpolated per key in between.
    //
    // A: struck at 1/7 of the string, so partial n gets sin(n pi / 7) / n,
    // consecutive harmonics up to the 14th. Unison partners at 1 and 2 beat
    // and give the two stage decay. The hammer cuts the highs (more when
    // played softly), the soundboard the bass fundamental; decay follows
    // each partial's own frequency (T60 about 24 s at A0, 8 s at C4).
    // Stiffness is kept low and constant: stretching the treble partials
    // (as real strings do) needs stretched tuning to match, or octaves
    // beat. An unpitched knock (low passed noise) stands for the case.
    [PIANO] = {
        .name = "piano", .count = 16,
        .p = { P(1, .8f, 8, 0), P(1.0008f, .55f, 2.8f, 0), P(2, .45f, 5.66f, 0), P(2.0019f, .3f, 2.0f, 0),
               P(3, .6f, 4.62f, 0), P(4, .45f, 4, 0), P(5, .29f, 3.58f, 0), P(6, .13f, 3.27f, 0),
               P(7, .066f, 3.02f, 0), P(8, -.1f, 2.83f, 0), P(9, -.16f, 2.67f, 0), P(10, -.18f, 2.53f, 0),
               P(11, -.16f, 2.41f, 0), P(12, -.12f, 2.31f, 0), P(13, -.062f, 2.22f, 0), P(14, .033f, 2.14f, 0) },
        .gain = .2f, .decay_track = .5f, .restrike_damp = .6f, .release = .25f, .tilt = .2f, .vel_curve = 1.6f,
        .inharm = .0001f, .cutoff_hz = 5000, .cutoff_vel = 1.0f, .body_hz = 60,
        .noise = .08f, .noise_out = .02f, .noise_ms = 2, .knock = .15f, .knock_hz = 250, .knock_ms = 25,
        .pan_spread = .6f, .upper = &ss_patches[PIANO_UPPER], .blend_lo = 60, .blend_hi = 72,
    },
    // B, the top of the piano: the original 8 partial piano (in A's slots,
    // the partials it lacks at level 0) with its decay eased so the treble
    // rings on (decay_track .8 -> .5). Not on a program of its own.
    [PIANO_UPPER] = {
        .name = "piano (upper)", .count = 16,
        .p = { P(1, .55f, 7, 0), P(1.0009f, .45f, 3.5f, 0), P(2, .85f, 4, 0), P(2.0019f, 0, 2, 0),
               P(3, .7f, 3, 0), P(4, .5f, 2.4f, 0), P(5, .33f, 2, 0), P(6, .18f, 1.6f, 0),
               P(7, 0, 1.3f, 0), P(8, -.1f, 1.1f, 0), P(9, 0, 1, 0), P(10, 0, 1, 0),
               P(11, 0, 1, 0), P(12, 0, 1, 0), P(13, 0, 1, 0), P(14, 0, 1, 0) },
        .gain = .28f, .decay_track = .5f, .restrike_damp = .6f, .release = .25f, .tilt = .25f, .vel_curve = 1.6f, .bright = 1.0f,
        .inharm = .0004f, .noise = .08f, .noise_out = .02f, .noise_ms = 2, .pan_spread = .6f,
    },
    [EPIANO] = {
        .name = "electric piano", .count = 6,
        .p = { P(1, 1, 4.5f, 0), P(1.0015f, .25f, 3, 0), P(2, .2f, 2.5f, 0), P(3, .06f, 1.5f, 0),
               P(7, .3f, .35f, 0), P(11.3f, .12f, .15f, 0) },
        .gain = .61f, .decay_track = .7f, .restrike_damp = .5f, .release = .2f, .tilt = .4f, .vel_curve = 1.5f, .bright = 1.6f,
        .pan_spread = .5f,
    },
    [HARPSI] = {
        .name = "harpsichord", .count = 8,
        .p = { P(1, .7f, 3, 0), P(2, -.75f, 2.5f, 0), P(3, .6f, 2, 0), P(4, -.55f, 1.6f, 0),
               P(5, .45f, 1.3f, 0), P(6, -.4f, 1.1f, 0), P(7, .35f, .9f, 0), P(8, -.3f, .8f, 0) },
        .gain = .25f, .decay_track = .8f, .restrike_damp = .85f, .release = .12f, .tilt = .3f, .vel_curve = .6f, .bright = .1f,
        .noise = .05f, .noise_out = .03f, .noise_ms = 1.5f, .pan_spread = .5f,
    },
    [CLAV] = {
        .name = "clavinet", .count = 8,
        .p = { P(1, .6f, 1.5f, 0), P(2, .7f, 1.3f, 0), P(3, .6f, 1.1f, 0), P(4, .5f, .9f, 0),
               P(5, .45f, .8f, 0), P(6, .35f, .7f, 0), P(7, .3f, .6f, 0), P(9, .2f, .5f, 0) },
        .gain = .6f, .decay_track = .6f, .restrike_damp = .85f, .release = .06f, .tilt = .3f, .vel_curve = 1.2f, .bright = .9f,
        .noise = .03f, .noise_ms = 1, .pan_spread = .3f,
    },
    [MALLET] = {
        .name = "mallet", .count = 5,
        .p = { P(1, 1, 3.5f, 0), P(1.0025f, .2f, 3, 0), P(2.756f, .4f, 1.2f, 0), P(5.404f, .2f, .5f, 0),
               P(8.933f, .1f, .25f, 0) },
        .restrike_damp = .2f, .gain = .42f, .decay_track = .6f, .tilt = .3f, .vel_curve = 1.4f, .bright = 1.0f,
        .noise = .03f, .noise_ms = 1, .pan_spread = .5f,
    },
    [MARIMBA] = {
        .name = "marimba", .count = 3,
        .p = { P(1, 1, 1.4f, 0), P(3.99f, .35f, .35f, 0), P(9.9f, .12f, .12f, 0) },
        .restrike_damp = .4f, .gain = .58f, .decay_track = .7f, .tilt = .3f, .vel_curve = 1.4f, .bright = 1.2f,
        .noise = .05f, .noise_ms = 1.5f, .pan_spread = .6f,
    },
    [BELL] = {
        .name = "bell", .count = 8,
        .p = { P(.5f, .35f, 9, 0), P(1, .6f, 7, 0), P(1.183f, .4f, 5, 0), P(1.506f, .3f, 4, 0),
               P(2, .45f, 3.5f, 0), P(2.514f, .2f, 2.5f, 0), P(2.662f, .18f, 2, 0), P(3.011f, .12f, 1.6f, 0) },
        .gain = .7f, .decay_track = .6f, .tilt = .4f, .vel_curve = 1.4f, .bright = .8f,
        .noise = .02f, .noise_ms = 1, .pan_spread = .5f,
    },
    [ORGAN] = {
        .name = "drawbar organ", .count = 8,
        .p = { P(.5f, 0, .15f, .45f), P(1, 0, .15f, .8f), P(1.5f, 0, .15f, .35f), P(2, 0, .15f, .5f),
               P(3, 0, .15f, .25f), P(4, 0, .15f, .25f), P(6, 0, .15f, .12f), P(8, 0, .15f, .12f) },
        .gain = .22f, .release = .08f, .tilt = .15f, .vel_curve = .05f,
        .noise_out = .02f, .noise_ms = .8f, .pan_spread = .3f,
    },
    [CHURCH] = {
        .name = "church organ", .count = 8,
        .p = { P(.5f, 0, .5f, .5f), P(1, 0, .5f, .8f), P(2, 0, .5f, .55f), P(3, 0, .5f, .3f),
               P(4, 0, .5f, .3f), P(5, 0, .5f, .15f), P(6, 0, .5f, .12f), P(8, 0, .5f, .1f) },
        .gain = .19f, .release = .5f, .tilt = .1f, .vel_curve = .05f,
        .noise = .02f, .noise_out = .01f, .noise_ms = 4, .pan_spread = .4f,
    },
    [ACCORDION] = {
        .name = "accordion", .count = 8,
        .p = { P(1, 0, .2f, .6f), P(1.0045f, 0, .2f, .5f), P(2, 0, .2f, .4f), P(2.009f, 0, .2f, .3f),
               P(3, 0, .2f, .35f), P(4, 0, .2f, .2f), P(5, 0, .2f, .18f), P(6, 0, .2f, .1f) },
        .gain = .29f, .release = .1f, .tilt = .15f, .vel_curve = .3f, .pan_spread = .3f,
    },
    [NYLON] = {
        .name = "nylon guitar", .count = 7,
        .p = { P(1, 1, 3, 0), P(2, .5f, 2.2f, 0), P(3, .4f, 1.7f, 0), P(4, .25f, 1.3f, 0),
               P(5, .18f, 1, 0), P(6, .1f, .8f, 0), P(7, .06f, .6f, 0) },
        .gain = .57f, .decay_track = .7f, .restrike_damp = .85f, .release = .35f, .tilt = .3f, .vel_curve = 1.4f, .bright = .7f,
        .noise = .03f, .noise_out = .01f, .noise_ms = 1.5f, .pan_spread = .4f,
    },
    [STEEL] = {
        .name = "steel guitar", .count = 8,
        .p = { P(1, .9f, 3.5f, 0), P(2, .7f, 2.6f, 0), P(3, .55f, 2, 0), P(4, .45f, 1.6f, 0),
               P(5, .35f, 1.3f, 0), P(6, .25f, 1.1f, 0), P(7, .18f, .9f, 0), P(8, .12f, .8f, 0) },
        .gain = .4f, .decay_track = .7f, .restrike_damp = .85f, .release = .3f, .tilt = .3f, .vel_curve = 1.3f, .bright = .8f,
        .inharm = .0001f, .noise = .04f, .noise_out = .015f, .noise_ms = 1, .pan_spread = .4f,
    },
    [MUTED] = {
        .name = "muted guitar", .count = 5,
        .p = { P(1, 1, .35f, 0), P(2, .5f, .25f, 0), P(3, .3f, .2f, 0), P(4, .15f, .15f, 0), P(5, .08f, .12f, 0) },
        .gain = .89f, .decay_track = .4f, .restrike_damp = .9f, .release = .08f, .tilt = .3f, .vel_curve = 1.3f, .bright = .7f,
        .noise = .05f, .noise_ms = 1.5f, .pan_spread = .4f,
    },
    [OVERDRIVE] = {
        .name = "overdrive guitar", .count = 8,
        .p = { P(1, .8f, 1.5f, .5f), P(2, .6f, 1.2f, .35f), P(3, .5f, 1, .35f), P(4, .4f, .9f, .2f),
               P(5, .35f, .8f, .2f), P(6, .25f, .7f, .12f), P(7, .2f, .6f, .1f), P(9, .12f, .5f, .06f) },
        .gain = .34f, .decay_track = .3f, .restrike_damp = .5f, .release = .1f, .tilt = .2f, .vel_curve = .7f, .bright = .3f,
        .vibrato = 4, .vib_delay = .6f, .pan_spread = .3f,
    },
    [BASS] = {
        .name = "bass", .count = 6,
        .p = { P(1, 1, 2.8f, 0), P(2, .55f, 1.6f, 0), P(3, .3f, 1.1f, 0), P(4, .18f, .8f, 0),
               P(5, .1f, .6f, 0), P(6, .05f, .5f, 0) },
        .gain = .59f, .decay_track = .4f, .restrike_damp = .85f, .release = .12f, .tilt = .1f, .vel_curve = 1.3f, .bright = .8f,
        .noise = .02f, .noise_ms = 1.5f,
    },
    [SYNBASS] = {
        .name = "synth bass", .count = 8,
        .p = { P(1, 1, 1.2f, 0), P(2, .5f, 1, 0), P(3, .33f, .8f, 0), P(4, .25f, .7f, 0),
               P(5, .2f, .6f, 0), P(6, .16f, .5f, 0), P(7, .14f, .4f, 0), P(8, .12f, .35f, 0) },
        .gain = .77f, .restrike_damp = .9f, .release = .08f, .tilt = .1f, .vel_curve = 1, .bright = 1.5f,
        .glide = 1.03f, .glide_ms = 15,
    },
    [STRINGS] = {
        .name = "strings", .count = 8,
        .p = { P(1, 0, 1.2f, 1), P(2, 0, 1.2f, .5f), P(3, 0, 1.2f, .33f), P(4, 0, 1.2f, .25f),
               P(5, 0, 1.2f, .2f), P(6, 0, 1.2f, .16f), P(7, 0, 1.2f, .13f), P(8, 0, 1.2f, .11f) },
        .gain = .48f, .decay_track = .3f, .release = .5f, .tilt = .1f, .vel_curve = .8f, .bright = .4f,
        .vibrato = 12, .vib_rate = 5.2f, .vib_delay = .35f, .pan_spread = .5f,
    },
    [ENSEMBLE] = {
        .name = "string ensemble", .count = 8,
        .p = { P(1, 0, 1.6f, 1), P(1.0035f, 0, 1.6f, .8f), P(2, 0, 1.6f, .45f), P(2.006f, 0, 1.6f, .35f),
               P(3, 0, 1.6f, .33f), P(4, 0, 1.6f, .25f), P(5, 0, 1.6f, .2f), P(6, 0, 1.6f, .16f) },
        .gain = .19f, .decay_track = .3f, .release = .7f, .tilt = .1f, .vel_curve = .8f, .bright = .4f,
        .vibrato = 6, .vib_rate = 5, .vib_delay = .4f, .pan_spread = .6f,
    },
    [PIZZ] = {
        .name = "pizzicato", .count = 5,
        .p = { P(1, 1, .9f, 0), P(2, .5f, .6f, 0), P(3, .3f, .45f, 0), P(4, .2f, .35f, 0), P(5, .1f, .3f, 0) },
        .restrike_damp = .7f, .gain = .91f, .decay_track = .5f, .tilt = .2f, .vel_curve = 1.3f, .bright = .6f,
        .noise = .02f, .noise_ms = 1, .pan_spread = .5f,
    },
    [HARP] = {
        .name = "harp", .count = 5,
        .p = { P(1, 1, 4, 0), P(1.0006f, .2f, 3, 0), P(2, .4f, 3, 0), P(3, .2f, 2, 0), P(4, .1f, 1.5f, 0) },
        .restrike_damp = .6f, .gain = .83f, .decay_track = .7f, .tilt = .3f, .vel_curve = 1.4f, .bright = .7f, .pan_spread = .6f,
    },
    [TIMPANI] = {
        .name = "timpani", .flags = SS_PATCH_ONE_SHOT, .count = 5,
        .p = { P(1, .8f, 2.5f, 0), P(1.5f, .5f, 2, 0), P(1.98f, .35f, 1.5f, 0), P(2.44f, .2f, 1.2f, 0),
               P(2.9f, .12f, 1, 0) },
        .restrike_damp = .3f, .gain = .66f, .decay_track = .3f, .tilt = .1f, .vel_curve = 1.5f, .bright = .8f,
        .noise = .1f, .noise_out = .02f, .noise_ms = 4,
    },
    [CHOIR] = {
        .name = "choir", .count = 8,
        .p = { P(1, 0, 1.4f, 1), P(1.004f, 0, 1.4f, .5f), P(2, 0, 1.4f, .7f), P(3, 0, 1.4f, .5f),
               P(4, 0, 1.4f, .45f), P(5, 0, 1.4f, .3f), P(6, 0, 1.4f, .15f), P(7, 0, 1.4f, .08f) },
        .gain = .25f, .release = .6f, .tilt = .15f, .vel_curve = .6f, .bright = .3f,
        .vibrato = 15, .vib_rate = 5, .vib_delay = .25f, .pan_spread = .5f,
    },
    [BRASS] = {
        .name = "brass", .count = 8,
        .p = { P(1, 0, .45f, 1), P(2, 0, .45f, .85f), P(3, 0, .45f, .7f), P(4, 0, .45f, .55f),
               P(5, 0, .45f, .45f), P(6, 0, .45f, .35f), P(7, 0, .45f, .25f), P(8, 0, .45f, .18f) },
        .gain = .24f, .decay_track = .2f, .release = .15f, .tilt = .15f, .vel_curve = .9f, .bright = 1.2f,
        .glide = .99f, .glide_ms = 20, .vibrato = 6, .vib_delay = .4f, .pan_spread = .4f,
    },
    [REED] = {
        .name = "reed", .count = 8,
        .p = { P(1, 0, .35f, 1), P(2, 0, .35f, .75f), P(3, 0, .35f, .55f), P(4, 0, .35f, .45f),
               P(5, 0, .35f, .35f), P(6, 0, .35f, .25f), P(7, 0, .35f, .18f), P(8, 0, .35f, .12f) },
        .gain = .27f, .release = .12f, .tilt = .15f, .vel_curve = .9f, .bright = 1.0f,
        .noise_out = .01f, .noise_ms = 20, .vibrato = 8, .vib_delay = .3f, .pan_spread = .3f,
    },
    [CLARINET] = {
        .name = "clarinet", .count = 6,
        .p = { P(1, 0, .35f, 1), P(3, 0, .35f, .6f), P(5, 0, .35f, .35f), P(7, 0, .35f, .2f),
               P(2, 0, .35f, .08f), P(4, 0, .35f, .06f) },
        .gain = .41f, .release = .12f, .tilt = .15f, .vel_curve = .9f, .bright = .8f,
        .vibrato = 3, .vib_delay = .4f, .pan_spread = .3f,
    },
    [FLUTE] = {
        .name = "flute", .count = 4,
        .p = { P(1, 0, .25f, 1), P(2, 0, .25f, .25f), P(3, 0, .25f, .1f), P(4, 0, .25f, .05f) },
        .gain = .44f, .release = .1f, .tilt = .1f, .vel_curve = .7f, .bright = .5f,
        .noise = .02f, .noise_out = .02f, .noise_ms = 25, .vibrato = 10, .vib_delay = .35f, .pan_spread = .3f,
    },
    [LEAD] = {
        .name = "lead", .count = 8,
        .p = { P(1, 0, .1f, 1), P(1.006f, 0, .1f, .6f), P(2, 0, .1f, .25f), P(3, 0, .1f, .33f),
               P(4, 0, .1f, .1f), P(5, 0, .1f, .2f), P(7, 0, .1f, .14f), P(9, 0, .1f, .11f) },
        .gain = .41f, .release = .08f, .tilt = .1f, .vel_curve = .4f, .bright = .5f, .pan_spread = .2f,
    },
    [PAD] = {
        .name = "pad", .count = 8,
        .p = { P(.5f, 0, 4, .3f), P(1, 0, 4, 1), P(1.006f, 0, 4, .8f), P(2, 0, 4, .45f),
               P(2.004f, 0, 4, .35f), P(3, 0, 4, .25f), P(4, 0, 4, .15f), P(6, 0, 4, .08f) },
        .gain = .28f, .release = 2.5f, .tilt = .15f, .vel_curve = .5f, .bright = .3f,
        .vibrato = 5, .vib_rate = 4, .vib_delay = .5f, .pan_spread = .7f,
    },
    [SPACE] = {
        .name = "space", .count = 8,
        .p = { P(1, .5f, 3, .4f), P(2.756f, .3f, 1.5f, 0), P(5.404f, .15f, .6f, 0), P(.5f, 0, 3, .3f),
               P(2, 0, 3, .25f), P(1.003f, 0, 3, .35f), P(3, .1f, 1, 0), P(4.01f, 0, 3, .1f) },
        .gain = .64f, .release = 2.5f, .tilt = .2f, .vel_curve = .8f, .bright = .6f,
        .vibrato = 4, .vib_rate = 3, .vib_delay = 1, .pan_spread = .8f,
    },
    [STEELDRUM] = {
        .name = "steel drum", .flags = SS_PATCH_ONE_SHOT, .count = 5,
        .p = { P(1, 1, 1.8f, 0), P(1.003f, .3f, 1.5f, 0), P(2, .5f, 1.2f, 0), P(3, .25f, .8f, 0),
               P(4, .12f, .5f, 0) },
        .restrike_damp = .4f, .gain = .57f, .decay_track = .5f, .tilt = .2f, .vel_curve = 1.3f, .bright = 1.0f,
        .noise = .03f, .noise_ms = 1.5f, .pan_spread = .5f,
    },
    [WOODBLOCK] = {
        .name = "woodblock", .flags = SS_PATCH_ONE_SHOT, .count = 3,
        .p = { P(1, 1, .12f, 0), P(2.6f, .5f, .08f, 0), P(4.2f, .25f, .05f, 0) },
        .restrike_damp = .5f, .gain = 1.35f, .decay_track = .3f, .vel_curve = 1.3f, .noise = .1f, .noise_ms = 1, .pan_spread = .3f,
    },
    [TOM] = {
        .name = "melodic tom", .flags = SS_PATCH_ONE_SHOT, .count = 5,
        .p = { P(1, 1, .6f, 0), P(1.59f, .5f, .4f, 0), P(2.14f, .3f, .3f, 0), P(2.3f, .25f, .25f, 0),
               P(2.65f, .15f, .2f, 0) },
        .restrike_damp = .5f, .gain = .62f, .decay_track = .3f, .vel_curve = 1.4f, .bright = .6f,
        .noise = .1f, .noise_out = .05f, .noise_ms = 3, .glide = 1.15f, .glide_ms = 25, .pan_spread = .3f,
    },
    // The original Java spring synth (ShoSynQ): springs at 1.998, 1 and
    // 4.001 times the note, plucked by velocity impulses -0.16, 0.3 and
    // -0.03 (the displacement is impulse / omega, hence the gains divided
    // by the ratios and tilt 1), all decaying with T60 3.1 s at every
    // pitch. Only the fundamental was driven while held, settling at 2 /
    // (pi x 4.42) = 0.144 of its pluck; the other two met Coulomb friction
    // that stopped them in about pi seconds (here: T60 1.8 s). The note
    // off friction stopped a note in about 0.2 s. Velocity was linear and
    // the output mono. The Java played key 60 at 440 Hz; here keys are in
    // tune (69 = 440 Hz).
    [CLASSIC] = {
        .name = "classic", .count = 3,
        .p = { P(1.998f, -.16f / 1.998f, 1.8f, 0), P(1, .3f, 3.1f, .3f * .144f), P(4.001f, -.03f / 4.001f, 1.8f, 0) },
        .gain = 1.2f, .release = .3f, .tilt = 1.0f, .vel_curve = 1.0f,
    },
};

const uint32_t ss_patch_count = PATCH_COUNT;
const uint8_t ss_classic_patch = CLASSIC;

_Static_assert(sizeof(ss_patches) / sizeof(ss_patches[0]) == PATCH_COUNT, "one entry per patch");

// GM program -> patch, by family of 8
const uint8_t ss_gm_patch[128] = {
    // 0-7 piano: acoustic, bright, electric grand, honky-tonk, e.piano 1, 2, harpsichord, clavinet
    PIANO, PIANO, PIANO, PIANO, EPIANO, EPIANO, HARPSI, CLAV,
    // 8-15 chromatic percussion: celesta, glockenspiel, music box, vibraphone, marimba, xylophone, tubular bells, dulcimer
    MALLET, MALLET, MALLET, MALLET, MARIMBA, MARIMBA, BELL, STEEL,
    // 16-23 organ: drawbar, percussive, rock, church, reed, accordion, harmonica, tango accordion
    ORGAN, ORGAN, ORGAN, CHURCH, CHURCH, ACCORDION, ACCORDION, ACCORDION,
    // 24-31 guitar: nylon, steel, jazz, clean, muted, overdrive, distortion, harmonics
    NYLON, STEEL, STEEL, STEEL, MUTED, OVERDRIVE, OVERDRIVE, MALLET,
    // 32-39 bass: acoustic, finger, pick, fretless, slap 1, slap 2, synth 1, synth 2
    BASS, BASS, BASS, BASS, BASS, BASS, SYNBASS, SYNBASS,
    // 40-47 strings: violin, viola, cello, contrabass, tremolo, pizzicato, harp, timpani
    STRINGS, STRINGS, STRINGS, STRINGS, ENSEMBLE, PIZZ, HARP, TIMPANI,
    // 48-55 ensemble: strings 1, 2, synth strings 1, 2, choir aahs, voice oohs, synth voice, orchestra hit
    ENSEMBLE, ENSEMBLE, ENSEMBLE, PAD, CHOIR, CHOIR, CHOIR, BRASS,
    // 56-63 brass: trumpet, trombone, tuba, muted trumpet, french horn, section, synth brass 1, 2
    BRASS, BRASS, BRASS, BRASS, BRASS, BRASS, BRASS, BRASS,
    // 64-71 reed: soprano, alto, tenor, baritone sax, oboe, english horn, bassoon, clarinet
    REED, REED, REED, REED, REED, REED, REED, CLARINET,
    // 72-79 pipe: piccolo, flute, recorder, pan flute, blown bottle, shakuhachi, whistle, ocarina
    FLUTE, FLUTE, FLUTE, FLUTE, FLUTE, FLUTE, FLUTE, FLUTE,
    // 80-87 synth lead
    LEAD, LEAD, LEAD, LEAD, LEAD, CHOIR, LEAD, LEAD,
    // 88-95 synth pad
    PAD, PAD, PAD, CHOIR, PAD, PAD, PAD, PAD,
    // 96-103 synth effects
    SPACE, SPACE, MALLET, SPACE, SPACE, SPACE, SPACE, SPACE,
    // 104-111 ethnic: sitar, banjo, shamisen, koto, kalimba, bagpipe, fiddle, shanai
    STEEL, STEEL, STEEL, HARP, MARIMBA, REED, STRINGS, REED,
    // 112-119 percussive: tinkle bell, agogo, steel drums, woodblock, taiko, melodic tom, synth drum, reverse cymbal
    BELL, MALLET, STEELDRUM, WOODBLOCK, TOM, TOM, TOM, SPACE,
    // 120-127 sound effects: fret noise, breath, seashore, bird, telephone, helicopter, applause, gunshot
    MUTED, FLUTE, SPACE, FLUTE, MALLET, SPACE, SPACE, WOODBLOCK,
};

// ---------------------------------------------------------------------------
// Drum kit
// ---------------------------------------------------------------------------

enum {
    DK_KICK, DK_SNARE, DK_RIM, DK_CLAP, DK_TOM, DK_HAT_C, DK_HAT_P, DK_HAT_O,
    DK_CRASH, DK_RIDE, DK_RIDE_BELL, DK_COWBELL, DK_TAMB, DK_HAND, DK_SHAKER,
    DK_WOOD, DK_TRI_M, DK_TRI_O,
    DRUM_COUNT
};

// Inharmonic metal cluster (cymbals, hats): ratios spread so no two line up
#define METAL(t) P(1, .3f, (t), 0), P(1.342f, .3f, (t), 0), P(1.783f, .28f, (t), 0), P(2.051f, .26f, (t), 0), \
                 P(2.53f, .24f, (t), 0), P(2.87f, .22f, (t) * .9f, 0), P(3.24f, .2f, (t) * .8f, 0), P(3.73f, .18f, (t) * .7f, 0)

const SsPatch ss_drum_patches[] = {
    [DK_KICK] = {
        .name = "kick", .flags = SS_PATCH_ONE_SHOT, .count = 3, .hz = 55,
        .p = { P(1, 1, .45f, 0), P(1.59f, .25f, .2f, 0), P(2.14f, .1f, .12f, 0) },
        .restrike_damp = .6f, .gain = 1.63f, .vel_curve = 1.4f, .noise = .15f, .noise_out = .04f, .noise_ms = 2,
        .glide = 2.2f, .glide_ms = 18,
    },
    [DK_SNARE] = {
        .name = "snare", .flags = SS_PATCH_ONE_SHOT, .count = 8, .hz = 185,
        .p = { P(1, .6f, .25f, 0), P(1.59f, .4f, .2f, 0), P(2.14f, .3f, .15f, 0), P(2.3f, .25f, .15f, 0),
               P(16, .3f, .18f, 0), P(21.5f, .25f, .15f, 0), P(27, .2f, .12f, 0), P(33, .15f, .1f, 0) },
        .restrike_damp = .3f, .gain = 1.05f, .vel_curve = 1.4f, .noise = .25f, .noise_out = .35f, .noise_ms = 40,
    },
    [DK_RIM] = {
        .name = "side stick", .flags = SS_PATCH_ONE_SHOT, .count = 3, .hz = 1700,
        .p = { P(1, 1, .06f, 0), P(2.3f, .5f, .04f, 0), P(3.9f, .3f, .03f, 0) },
        .restrike_damp = .5f, .gain = .7f, .vel_curve = 1.3f, .noise = .2f, .noise_out = .1f, .noise_ms = 1,
    },
    [DK_CLAP] = {
        .name = "clap", .flags = SS_PATCH_ONE_SHOT, .count = 3, .hz = 1100,
        .p = { P(1, .3f, .08f, 0), P(1.4f, .3f, .08f, 0), P(2.1f, .2f, .06f, 0) },
        .restrike_damp = .5f, .gain = .93f, .vel_curve = 1.3f, .noise = .6f, .noise_out = .5f, .noise_ms = 25,
    },
    [DK_TOM] = {
        .name = "tom", .flags = SS_PATCH_ONE_SHOT, .count = 4, .hz = 120,
        .p = { P(1, 1, .5f, 0), P(1.59f, .45f, .35f, 0), P(2.14f, .3f, .25f, 0), P(2.65f, .15f, .2f, 0) },
        .restrike_damp = .5f, .gain = 1.16f, .vel_curve = 1.4f, .noise = .15f, .noise_out = .04f, .noise_ms = 3,
        .glide = 1.25f, .glide_ms = 30,
    },
    [DK_HAT_C] = {
        .name = "closed hi-hat", .flags = SS_PATCH_ONE_SHOT, .count = 8, .hz = 3200,
        .p = { METAL(.07f) }, .gain = .7f, .vel_curve = 1.3f, .noise = .4f, .noise_out = .4f, .noise_ms = 12,
    },
    [DK_HAT_P] = {
        .name = "pedal hi-hat", .flags = SS_PATCH_ONE_SHOT, .count = 8, .hz = 3200,
        .p = { METAL(.05f) }, .gain = .52f, .vel_curve = 1.3f, .noise = .4f, .noise_out = .3f, .noise_ms = 8,
    },
    [DK_HAT_O] = {
        .name = "open hi-hat", .flags = SS_PATCH_ONE_SHOT, .count = 8, .hz = 3200,
        .p = { METAL(.7f) }, .gain = .58f, .vel_curve = 1.3f, .noise = .4f, .noise_out = .3f, .noise_ms = 150,
    },
    [DK_CRASH] = {
        .name = "crash", .flags = SS_PATCH_ONE_SHOT, .count = 8, .hz = 2200,
        .p = { METAL(2.2f) }, .gain = .52f, .vel_curve = 1.3f, .noise = .3f, .noise_out = .3f, .noise_ms = 350,
    },
    [DK_RIDE] = {
        .name = "ride", .flags = SS_PATCH_ONE_SHOT, .count = 8, .hz = 3500,
        .p = { METAL(2.5f) }, .gain = .41f, .vel_curve = 1.3f, .noise = .15f, .noise_out = .12f, .noise_ms = 60,
    },
    [DK_RIDE_BELL] = {
        .name = "ride bell", .flags = SS_PATCH_ONE_SHOT, .count = 4, .hz = 1100,
        .p = { P(1, 1, 2, 0), P(2.4f, .5f, 1.5f, 0), P(3.9f, .3f, 1, 0), P(5.5f, .2f, .8f, 0) },
        .restrike_damp = .3f, .gain = .47f, .vel_curve = 1.3f, .noise = .05f, .noise_ms = 1,
    },
    [DK_COWBELL] = {
        .name = "cowbell", .flags = SS_PATCH_ONE_SHOT, .count = 3, .hz = 560,
        .p = { P(1, 1, .35f, 0), P(1.48f, .7f, .3f, 0), P(2.4f, .3f, .2f, 0) },
        .restrike_damp = .5f, .gain = .7f, .vel_curve = 1.3f, .noise = .05f, .noise_ms = 1,
    },
    [DK_TAMB] = {
        .name = "tambourine", .flags = SS_PATCH_ONE_SHOT, .count = 8, .hz = 4500,
        .p = { METAL(.25f) }, .gain = .47f, .vel_curve = 1.3f, .noise = .4f, .noise_out = .3f, .noise_ms = 40,
    },
    [DK_HAND] = {
        .name = "hand drum", .flags = SS_PATCH_ONE_SHOT, .count = 3, .hz = 300,
        .p = { P(1, 1, .25f, 0), P(1.59f, .4f, .18f, 0), P(2.14f, .25f, .12f, 0) },
        .restrike_damp = .5f, .gain = .93f, .vel_curve = 1.4f, .noise = .15f, .noise_ms = 1.5f, .glide = 1.08f, .glide_ms = 10,
    },
    [DK_SHAKER] = {
        .name = "shaker", .flags = SS_PATCH_ONE_SHOT, .count = 1, .hz = 6000,
        .p = { P(1, .05f, .03f, 0) }, .gain = .58f, .vel_curve = 1.3f, .noise_out = .35f, .noise_ms = 15,
    },
    [DK_WOOD] = {
        .name = "wood", .flags = SS_PATCH_ONE_SHOT, .count = 3, .hz = 1000,
        .p = { P(1, 1, .12f, 0), P(2.6f, .5f, .08f, 0), P(4.2f, .25f, .05f, 0) },
        .restrike_damp = .5f, .gain = .82f, .vel_curve = 1.3f, .noise = .1f, .noise_ms = 1,
    },
    [DK_TRI_M] = {
        .name = "mute triangle", .flags = SS_PATCH_ONE_SHOT, .count = 4, .hz = 4000,
        .p = { P(1, .5f, .15f, 0), P(2.83f, .3f, .12f, 0), P(5.1f, .2f, .1f, 0), P(7.2f, .15f, .08f, 0) },
        .gain = .58f, .vel_curve = 1.3f, .noise = .05f, .noise_ms = 1,
    },
    [DK_TRI_O] = {
        .name = "open triangle", .flags = SS_PATCH_ONE_SHOT, .count = 4, .hz = 4000,
        .p = { P(1, .5f, 3, 0), P(2.83f, .3f, 2.5f, 0), P(5.1f, .2f, 2, 0), P(7.2f, .15f, 1.5f, 0) },
        .gain = .58f, .vel_curve = 1.3f, .noise = .05f, .noise_ms = 1,
    },
};

const uint32_t ss_drum_patch_count = DRUM_COUNT;

_Static_assert(sizeof(ss_drum_patches) / sizeof(ss_drum_patches[0]) == DRUM_COUNT, "one entry per drum patch");

// Keys not listed are silent (patch 0)
#define D(p, g, hz, pan) { (p) + 1, (g), (hz), (pan) }

// GM drum map. Hi-hats choke each other (group 1), so do the triangles
// (group 2) and the cuicas, whistles and guiros of each pair.
const SsDrumKey ss_drum_map[128] = {
    [31] = D(DK_WOOD, 0, 2500, 0),          // Sticks
    [33] = D(DK_RIM, 0, 2200, 0),           // Metronome click
    [34] = D(DK_RIDE_BELL, 0, 1500, 0),     // Metronome bell
    [35] = D(DK_KICK, 0, 48, 0),            // Acoustic bass drum
    [36] = D(DK_KICK, 0, 55, 0),            // Bass drum 1
    [37] = D(DK_RIM, 0, 0, 0),              // Side stick
    [38] = D(DK_SNARE, 0, 185, 0),          // Acoustic snare
    [39] = D(DK_CLAP, 0, 0, -.1f),          // Hand clap
    [40] = D(DK_SNARE, 0, 210, 0),          // Electric snare
    [41] = D(DK_TOM, 0, 80, -.5f),          // Low floor tom
    [42] = D(DK_HAT_C, 1, 0, .3f),          // Closed hi-hat
    [43] = D(DK_TOM, 0, 95, -.4f),          // High floor tom
    [44] = D(DK_HAT_P, 1, 0, .3f),          // Pedal hi-hat
    [45] = D(DK_TOM, 0, 115, -.2f),         // Low tom
    [46] = D(DK_HAT_O, 1, 0, .3f),          // Open hi-hat
    [47] = D(DK_TOM, 0, 135, 0),            // Low-mid tom
    [48] = D(DK_TOM, 0, 160, .2f),          // Hi-mid tom
    [49] = D(DK_CRASH, 0, 0, -.4f),         // Crash cymbal 1
    [50] = D(DK_TOM, 0, 190, .4f),          // High tom
    [51] = D(DK_RIDE, 0, 0, .5f),           // Ride cymbal 1
    [52] = D(DK_CRASH, 0, 1700, .5f),       // Chinese cymbal
    [53] = D(DK_RIDE_BELL, 0, 0, .5f),      // Ride bell
    [54] = D(DK_TAMB, 0, 0, .4f),           // Tambourine
    [55] = D(DK_CRASH, 0, 3000, -.3f),      // Splash cymbal
    [56] = D(DK_COWBELL, 0, 0, .3f),        // Cowbell
    [57] = D(DK_CRASH, 0, 2500, .5f),       // Crash cymbal 2
    [58] = D(DK_HAND, 0, 180, -.3f),        // Vibraslap
    [59] = D(DK_RIDE, 0, 3900, .5f),        // Ride cymbal 2
    [60] = D(DK_HAND, 0, 420, .3f),         // Hi bongo
    [61] = D(DK_HAND, 0, 320, .2f),         // Low bongo
    [62] = D(DK_HAND, 0, 360, -.2f),        // Mute hi conga
    [63] = D(DK_HAND, 0, 300, -.2f),        // Open hi conga
    [64] = D(DK_HAND, 0, 220, -.3f),        // Low conga
    [65] = D(DK_TOM, 0, 330, .3f),          // High timbale
    [66] = D(DK_TOM, 0, 260, .3f),          // Low timbale
    [67] = D(DK_COWBELL, 0, 900, .4f),      // High agogo
    [68] = D(DK_COWBELL, 0, 700, .4f),      // Low agogo
    [69] = D(DK_SHAKER, 0, 5000, -.4f),     // Cabasa
    [70] = D(DK_SHAKER, 0, 0, .4f),         // Maracas
    [73] = D(DK_SHAKER, 3, 3500, .3f),      // Short guiro
    [74] = D(DK_SHAKER, 3, 3000, .3f),      // Long guiro
    [75] = D(DK_WOOD, 0, 2500, .3f),        // Claves
    [76] = D(DK_WOOD, 0, 1000, .4f),        // Hi wood block
    [77] = D(DK_WOOD, 0, 800, .4f),         // Low wood block
    [78] = D(DK_HAND, 4, 600, -.3f),        // Mute cuica
    [79] = D(DK_HAND, 4, 450, -.3f),        // Open cuica
    [80] = D(DK_TRI_M, 2, 0, .5f),          // Mute triangle
    [81] = D(DK_TRI_O, 2, 0, .5f),          // Open triangle
};
