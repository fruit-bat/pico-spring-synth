//
// test_synth.c - tests of the spring synth core: tuning, decay, drive,
// string allocation (re-strike, self freeing, stealing, the partial
// budget), pedals, pitch bend, drums, the split render and every patch.
//
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "ss_synth.h"

#define RATE 44100

static SsSynth synth;

// Fresh synth with no effects, unity gain, and channel 0 at full volume
// and centre pan, so a string of amplitude A gives A cos(pi/4) per side
static void setup(void) {
    ss_synth_init(&synth, RATE);
    ss_synth_set_effects(&synth, false, false);
    ss_synth_set_gain(&synth, 1.0f);
    for (uint8_t c = 0; c < 16; ++c) ss_synth_control_change(&synth, c, 7, 127);
}

// Render `secs` seconds (rounded up to whole blocks); the left channel
// goes to buf (if not NULL)
static void render(float secs, float *buf) {
    const uint32_t blocks = (uint32_t)ceilf(secs * RATE / SS_BLOCK_SIZE);
    float l[SS_BLOCK_SIZE], r[SS_BLOCK_SIZE];
    for (uint32_t b = 0; b < blocks; ++b) {
        ss_synth_render_float(&synth, l, r);
        if (buf) memcpy(buf + b * SS_BLOCK_SIZE, l, sizeof(l));
    }
}

// Frequency from rising zero crossings (interpolated) over buf[0..n)
static double measure_freq(const float *buf, uint32_t n) {
    double first = -1, last = -1;
    int count = 0;
    for (uint32_t i = 1; i < n; ++i) {
        if (buf[i - 1] < 0 && buf[i] >= 0) {
            const double t = (i - 1) + buf[i - 1] / (double)(buf[i - 1] - buf[i]);
            if (first < 0) first = t;
            else ++count;
            last = t;
        }
    }
    return count > 0 ? count * (double)RATE / (last - first) : 0.0;
}

// Largest magnitude in buf[from..to)
static float peak(const float *buf, uint32_t from, uint32_t to) {
    float p = 0;
    for (uint32_t i = from; i < to; ++i) p = fmaxf(p, fabsf(buf[i]));
    return p;
}

// A single spring patch
static SsPatch sine(float gain, float t60, float drive) {
    SsPatch p = { .name = "test", .count = 1, .gain = 1.0f, .vel_curve = 1.0f };
    p.p[0] = (SsPartial){ 1.0f, gain, t60, drive };
    return p;
}

static float *buffer(float secs) {
    return calloc((size_t)(secs * RATE) + 2 * SS_BLOCK_SIZE, sizeof(float));
}

TEST(tuning) {
    static const struct { uint8_t key; double hz; } cases[] = {
        { 69, 440.0 }, { 21, 27.5 }, { 60, 261.6256 }, { 108, 4186.009 },
    };
    float *buf = buffer(2);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        setup();
        const SsPatch p = sine(0.1f, 20.0f, 0);
        ss_synth_note_on_patch(&synth, 0, cases[i].key, 127, &p, 0, 0, 0);
        render(2, buf);
        // From 0.25 s: past the output DC blocker's start up transient
        CHECK_NEAR(measure_freq(buf + RATE / 4, 7 * RATE / 4), cases[i].hz, cases[i].hz * 2e-5);
    }
    free(buf);
}

TEST(amplitude_and_decay) {
    setup();
    float *buf = buffer(1.2f);
    const SsPatch p = sine(0.1f, 1.0f, 0); // T60 1 s
    ss_synth_note_on_patch(&synth, 0, 69, 127, &p, 0, 0, 0);
    render(1.2f, buf);
    // Starts at amplitude 0.1 (times the pan law)
    CHECK_NEAR(peak(buf, 0, RATE / 50), 0.1 * 0.70711, 0.002);
    // 30 dB down after 0.5 s, 60 dB after 1 s (peaks of 20 ms windows)
    const float a0 = peak(buf, 0, RATE / 50);
    const float a1 = peak(buf, RATE / 2, RATE / 2 + RATE / 50);
    const float a2 = peak(buf, RATE, RATE + RATE / 50);
    CHECK_NEAR(20 * log10(a0 / a1), 30.0, 1.0);
    CHECK_NEAR(20 * log10(a0 / a2), 60.0, 1.5);
    free(buf);
}

TEST(drive_settles_at_its_amplitude) {
    setup();
    float *buf = buffer(3);
    const SsPatch p = sine(0, 0.3f, 0.1f); // Not plucked: starts by itself
    ss_synth_note_on_patch(&synth, 0, 57, 127, &p, 0, 0, 0);
    render(3, buf);
    CHECK(peak(buf, 0, 64) < 0.01f);                       // Starts quietly
    CHECK_NEAR(peak(buf, 2 * RATE, 3 * RATE), 0.1 * 0.70711, 0.004);  // Settled
    // Released: drive off, and it dies away and frees itself
    ss_synth_note_off(&synth, 0, 57);
    render(2, NULL);
    CHECK_EQ(ss_synth_active_strings(&synth), 0);
    free(buf);
}

TEST(restrike_reuses_the_string) {
    setup();
    const SsPatch p = sine(0.1f, 3.0f, 0);
    ss_synth_note_on_patch(&synth, 0, 60, 100, &p, 0, 0, 0);
    render(0.1f, NULL);
    ss_synth_note_off(&synth, 0, 60);
    render(0.1f, NULL);
    ss_synth_note_on_patch(&synth, 0, 60, 100, &p, 0, 0, 0);
    render(0.1f, NULL);
    CHECK_EQ(ss_synth_active_strings(&synth), 1);
    CHECK_EQ(synth.stats.strings_started, 1);
    CHECK_EQ(synth.stats.restrikes, 1);
    // The same key on another channel is another string
    ss_synth_note_on_patch(&synth, 1, 60, 100, &p, 0, 0, 0);
    CHECK_EQ(ss_synth_active_strings(&synth), 2);
}

TEST(strings_free_themselves) {
    setup();
    ss_synth_program_change(&synth, 0, 24); // Nylon guitar
    for (uint8_t k = 40; k < 70; k += 3) ss_synth_note_on(&synth, 0, k, 100);
    render(0.05f, NULL);
    CHECK_EQ(ss_synth_active_strings(&synth), 10);
    for (uint8_t k = 40; k < 70; k += 3) ss_synth_note_off(&synth, 0, k);
    render(3, NULL);
    CHECK_EQ(ss_synth_active_strings(&synth), 0);
    CHECK_EQ(ss_synth_active_partials(&synth), 0);
}

TEST(partials_above_nyquist_are_left_out) {
    setup();
    SsPatch p = { .name = "t", .count = 8, .gain = 1, .vel_curve = 1 };
    for (int i = 0; i < 8; ++i) p.p[i] = (SsPartial){ (float)(i + 1), 0.1f, 5.0f, 0 };
    ss_synth_note_on_patch(&synth, 0, 60, 127, &p, 0, 0, 0);    // 262 Hz: all 8 below 16 kHz
    ss_synth_note_on_patch(&synth, 0, 120, 127, &p, 0, 0, 0);   // 8.4 kHz: only the fundamental
    CHECK_EQ(synth.strings[synth.key_string[0][60] - 1].count, 8);
    CHECK_EQ(synth.strings[synth.key_string[0][120] - 1].count, 1);
}

TEST(fixed_frequency_partials) {
    setup();
    float *buf = buffer(1);
    SsPatch p = { .name = "t", .count = 1, .gain = 1, .vel_curve = 1, .decay_track = 1 };
    p.p[0] = (SsPartial){ -440.0f, 0.1f, 20.0f, 0 };   // 440 Hz whatever the key
    ss_synth_note_on_patch(&synth, 0, 30, 127, &p, 0, 0, 0);
    render(1, buf);
    CHECK_NEAR(measure_freq(buf + RATE / 4, 3 * RATE / 4), 440.0, 0.05);
    free(buf);
}

// Amplitude of the spring for the patch's partial j on a fresh string
static float spring_amp(const SsPatch *pt, uint8_t key, uint32_t j) {
    setup();
    ss_synth_note_on_patch(&synth, 0, key, 100, pt, 0, 0, 0);
    const SsString *x = &synth.strings[synth.key_string[0][key] - 1];
    for (uint32_t i = 0; i < x->count; ++i) if (x->sp[i].idx == j) return fabsf(x->sp[i].im);
    return 0.0f;
}

TEST(key_blend) {
    const SsPatch *piano = &ss_patches[ss_gm_patch[0]];
    CHECK(piano->upper != NULL);
    SsPatch lower = *piano;
    lower.upper = NULL;
    const SsPatch *upper = piano->upper;
    // Partial 2 (the 2nd harmonic): A and B give it different levels
    const uint8_t lo = piano->blend_lo, hi = piano->blend_hi, mid = (uint8_t)((lo + hi) / 2);
    CHECK_NEAR(spring_amp(piano, (uint8_t)(lo - 5), 2), spring_amp(&lower, (uint8_t)(lo - 5), 2), 1e-7);
    CHECK_NEAR(spring_amp(piano, (uint8_t)(hi + 5), 2), spring_amp(upper, (uint8_t)(hi + 5), 2), 1e-7);
    const float a = spring_amp(&lower, mid, 2), b = spring_amp(upper, mid, 2), m = spring_amp(piano, mid, 2);
    CHECK(m > fminf(a, b) && m < fmaxf(a, b));
    // The string still belongs to the program's patch (re-strikes find it)
    setup();
    ss_synth_note_on(&synth, 0, mid, 100);
    ss_synth_note_on(&synth, 0, mid, 100);
    CHECK_EQ(synth.stats.restrikes, 1);
}

// Angular frequency (radians per sample) of a key's partial j
static float spring_w(const SsPatch *pt, uint8_t key, uint32_t j) {
    setup();
    ss_synth_note_on_patch(&synth, 0, key, 100, pt, 0, 0, 0);
    const SsString *x = &synth.strings[synth.key_string[0][key] - 1];
    for (uint32_t i = 0; i < x->count; ++i) if (x->sp[i].idx == j) return x->sp[i].w;
    return 0.0f;
}

TEST(stretched_tuning_and_stiffness) {
    const float two_pi_over_fs = 6.2831853f / 44100.0f;
    // Equal temperament up to stretch_key, then stretch x octaves^2 cents
    SsPatch p = sine(0.1f, 2.0f, 0);
    p.stretch = 4.0f;
    p.stretch_key = 72;
    const float c6 = 440.0f * powf(2.0f, 15.0f / 12.0f), c8 = 440.0f * powf(2.0f, 39.0f / 12.0f);
    CHECK_NEAR(spring_w(&p, 84, 0) / (two_pi_over_fs * c6), powf(2.0f, 4.0f / 1200.0f), 2e-5);
    CHECK_NEAR(spring_w(&p, 108, 0) / (two_pi_over_fs * c8), powf(2.0f, 36.0f / 1200.0f), 2e-5);
    CHECK_NEAR(spring_w(&p, 72, 0) / (two_pi_over_fs * c6 * 0.5f), 1.0f, 2e-5);
    // Stiffness sharpens the overtones but leaves the fundamental in tune
    SsPatch q = sine(0.1f, 2.0f, 0);
    q.count = 2;
    q.p[1] = (SsPartial){ 2.0f, 0.5f, 2.0f, 0 };
    q.inharm = 0.002f;
    const float w1 = spring_w(&q, 60, 0), w2 = spring_w(&q, 60, 1);
    CHECK_NEAR(w1 / (two_pi_over_fs * 261.6256f), 1.0f, 2e-5);
    CHECK_NEAR(w2 / w1, 2.0f * sqrtf((1.0f + 4.0f * 0.002f) / (1.0f + 0.002f)), 2e-5);
    // The piano's blend chains to a third patch at the top
    const SsPatch *piano = &ss_patches[ss_gm_patch[0]];
    CHECK(piano->upper && piano->upper->upper);
}

TEST(steals_the_quietest) {
    setup();
    const SsPatch p = sine(0.1f, 30.0f, 0);
    // A loud held note, then quiet released ones filling the pool
    ss_synth_note_on_patch(&synth, 0, 30, 127, &p, 0, 0, 0);
    for (uint32_t i = 1; i < SS_MAX_STRINGS; ++i) {
        const uint8_t ch = (uint8_t)(1 + i / 100), key = (uint8_t)(i % 100 + 10);
        ss_synth_note_on_patch(&synth, ch, key, 20, &p, 0, 0, 0);
        ss_synth_note_off(&synth, ch, key);
    }
    render(0.01f, NULL);
    CHECK_EQ(ss_synth_active_strings(&synth), SS_MAX_STRINGS);
    // More notes, 20 ms apart: each steals a quiet released string (never
    // the loud one), which fades out in the reserve
    for (uint8_t k = 0; k < 20; ++k) {
        ss_synth_note_on_patch(&synth, 15, (uint8_t)(k + 40), 60, &p, 0, 0, 0);
        CHECK(synth.active_count - synth.dying_count <= SS_MAX_STRINGS);
        render(0.02f, NULL);
    }
    CHECK_EQ(synth.stats.steals, 20);
    CHECK_EQ(synth.stats.cuts, 0);
    CHECK(synth.key_string[0][30] != 0);
    CHECK_EQ(synth.dying_count, 0);
    CHECK_EQ(ss_synth_active_strings(&synth), SS_MAX_STRINGS);
    // A burst bigger than the reserve, all in one block: once the spare
    // slots are full of fading strings, those are cut (with a declick)
    for (uint8_t k = 0; k < 20; ++k) ss_synth_note_on_patch(&synth, 14, (uint8_t)(k + 40), 60, &p, 0, 0, 0);
    CHECK_EQ(synth.stats.steals, 40);
    CHECK_EQ(synth.stats.cuts, 20 - SS_STEAL_RESERVE);
    CHECK(synth.key_string[0][30] != 0);
}

TEST(cost_budget) {
    setup();
    ss_synth_program_change(&synth, 0, 16); // Drawbar organ: 8 driven partials, all held
    for (int i = 0; i < 90; ++i) {
        ss_synth_note_on(&synth, (uint8_t)(i / 40), (uint8_t)(30 + i % 40), 100);
        if (i % 7 == 0) render(0.01f, NULL);
        CHECK(ss_synth_cost(&synth) <= SS_COST_BUDGET);
    }
    CHECK(synth.stats.steals > 0);
    render(0.1f, NULL);
    CHECK(ss_synth_cost(&synth) <= SS_COST_BUDGET);
}

TEST(sustain_pedal) {
    setup();
    float *buf = buffer(1);
    const SsPatch p = { .name = "t", .count = 1, .gain = 1, .vel_curve = 1, .release = 0.1f,
                        .p = { { 1, 0.1f, 5.0f, 0 } } };
    ss_synth_control_change(&synth, 0, 64, 127);
    ss_synth_note_on_patch(&synth, 0, 60, 127, &p, 0, 0, 0);
    ss_synth_note_off(&synth, 0, 60);
    render(0.5f, buf);
    const float held = peak(buf, RATE * 4 / 10, RATE / 2);
    CHECK(held > 0.03f);  // Still ringing at its own pace (T60 5 s: 0.07 -> 0.04)
    ss_synth_control_change(&synth, 0, 64, 0);
    render(0.5f, buf);
    CHECK(peak(buf, RATE * 4 / 10, RATE / 2) < held * 0.01f);  // Damped (0.1 s)
    free(buf);
}

TEST(pitch_bend) {
    setup();
    float *buf = buffer(1);
    const SsPatch p = sine(0.1f, 20.0f, 0);
    ss_synth_note_on_patch(&synth, 0, 69, 127, &p, 0, 0, 0);
    ss_synth_pitch_bend(&synth, 0, 16383);
    render(1, buf);
    CHECK_NEAR(measure_freq(buf, RATE), 440.0 * pow(2.0, (2.0 * 8191 / 8192) / 12.0), 0.05);
    // Bend range 12 via RPN 0
    ss_synth_control_change(&synth, 0, 101, 0);
    ss_synth_control_change(&synth, 0, 100, 0);
    ss_synth_control_change(&synth, 0, 6, 12);
    ss_synth_pitch_bend(&synth, 0, 0);
    render(1, buf);
    CHECK_NEAR(measure_freq(buf, RATE), 220.0, 0.05);
    free(buf);
}

TEST(drums) {
    setup();
    // One-shot: note off does nothing
    ss_synth_note_on(&synth, 9, 38, 100);
    const SsString *snare = &synth.strings[synth.key_string[9][38] - 1];
    ss_synth_note_off(&synth, 9, 38);
    CHECK_EQ(snare->state, SS_STR_HELD);
    // A closed hi-hat chokes the open one
    ss_synth_note_on(&synth, 9, 46, 100);
    render(0.05f, NULL);
    const SsString *open = &synth.strings[synth.key_string[9][46] - 1];
    const float before = open->energy;
    ss_synth_note_on(&synth, 9, 42, 100);
    CHECK_EQ(open->state, SS_STR_RELEASED);
    render(0.1f, NULL);
    CHECK(synth.key_string[9][46] == 0 || open->energy < before * 1e-3f);
    // Unmapped keys are silent
    const uint32_t n = ss_synth_active_strings(&synth);
    ss_synth_note_on(&synth, 9, 5, 100);
    CHECK_EQ(ss_synth_active_strings(&synth), n);
}

TEST(all_sound_off) {
    setup();
    float *buf = buffer(0.2f);
    ss_synth_program_change(&synth, 0, 19); // Church organ
    for (uint8_t k = 48; k < 60; ++k) ss_synth_note_on(&synth, 0, k, 100);
    render(0.5f, NULL);
    ss_synth_control_change(&synth, 0, 120, 0);
    render(0.2f, buf);
    CHECK(peak(buf, RATE / 10, RATE / 5) < 1e-4f);
    CHECK_EQ(ss_synth_active_strings(&synth), 0);
    free(buf);
}

// Rendering the parts in any order gives exactly the same output: they
// share no writable state
TEST(split_render_is_independent) {
    static SsSynth a, b;
    ss_synth_init(&a, RATE);
    ss_synth_init(&b, RATE);
    for (uint8_t k = 40; k < 80; k += 2) {
        ss_synth_note_on(&a, (uint8_t)(k % 3), k, 100);
        ss_synth_note_on(&b, (uint8_t)(k % 3), k, 100);
    }
    int diffs = 0;
    for (int blk = 0; blk < 200; ++blk) {
        int16_t oa[SS_BLOCK_SIZE * 2], ob[SS_BLOCK_SIZE * 2];
        ss_synth_render(&a, oa);
        ss_synth_render_begin(&b);
        for (int p = SS_RENDER_PARTS; p-- > 0;) ss_synth_render_part(&b, (uint32_t)p);
        ss_synth_render_end(&b, ob);
        diffs += memcmp(oa, ob, sizeof(oa)) != 0;
    }
    CHECK_EQ(diffs, 0);
    // Both parts got work
    CHECK(a.stats.partial_blocks[0] > 0);
    CHECK(a.stats.partial_blocks[SS_RENDER_PARTS - 1] > 0);
}

// Every program and every drum key sounds, stays finite and bounded
TEST(every_patch) {
    float l[SS_BLOCK_SIZE], r[SS_BLOCK_SIZE];
    int silent = 0, bad = 0;
    for (int prog = 0; prog <= 128; ++prog) {
        setup();
        ss_synth_set_effects(&synth, true, true);
        ss_synth_set_gain(&synth, SS_DEFAULT_GAIN);
        if (prog < 128) {
            ss_synth_program_change(&synth, 0, (uint8_t)prog);
            static const uint8_t keys[] = { 24, 48, 60, 72, 100 };
            for (unsigned i = 0; i < sizeof(keys); ++i) ss_synth_note_on(&synth, 0, keys[i], (uint8_t)(30 + 20 * i));
        } else {
            for (uint8_t k = 27; k <= 87; ++k) ss_synth_note_on(&synth, 9, k, 100);
        }
        float pk = 0;
        for (int blk = 0; blk < RATE / SS_BLOCK_SIZE; ++blk) {
            if (blk == RATE / SS_BLOCK_SIZE / 2) {
                for (uint8_t k = 0; k < 128; ++k) ss_synth_note_off(&synth, 0, k);
            }
            ss_synth_render_float(&synth, l, r);
            for (int k = 0; k < SS_BLOCK_SIZE; ++k) {
                if (!isfinite(l[k]) || !isfinite(r[k]) || fabsf(l[k]) > 1.0f || fabsf(r[k]) > 1.0f) ++bad;
                pk = fmaxf(pk, fmaxf(fabsf(l[k]), fabsf(r[k])));
            }
        }
        if (pk < 0.01f) {
            ++silent;
            fprintf(stderr, "program %d: peak %g\n", prog, pk);
        }
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(silent, 0);
}

TEST(classic_bank) {
    setup();
    ss_synth_control_change(&synth, 0, 0, 1);
    ss_synth_program_change(&synth, 0, 40);
    CHECK(ss_synth_channel_patch(&synth, 0, 60) == &ss_patches[ss_classic_patch]);
    ss_synth_control_change(&synth, 0, 0, 0);
    CHECK(ss_synth_channel_patch(&synth, 0, 60) == &ss_patches[ss_gm_patch[40]]);
    // Bank 120 makes any channel a drum channel
    ss_synth_control_change(&synth, 3, 0, 120);
    CHECK(synth.channels[3].drums);
    CHECK(ss_synth_channel_patch(&synth, 3, 5) == NULL);
}

TEST(program_change_lets_the_old_string_ring) {
    setup();
    ss_synth_program_change(&synth, 0, 0);
    ss_synth_note_on(&synth, 0, 60, 100);
    ss_synth_program_change(&synth, 0, 24);
    ss_synth_note_on(&synth, 0, 60, 100);
    CHECK_EQ(ss_synth_active_strings(&synth), 2);
    CHECK_EQ(synth.stats.strings_started, 2);
}

int main(void) {
    RUN(tuning);
    RUN(amplitude_and_decay);
    RUN(drive_settles_at_its_amplitude);
    RUN(restrike_reuses_the_string);
    RUN(strings_free_themselves);
    RUN(partials_above_nyquist_are_left_out);
    RUN(fixed_frequency_partials);
    RUN(key_blend);
    RUN(stretched_tuning_and_stiffness);
    RUN(steals_the_quietest);
    RUN(cost_budget);
    RUN(sustain_pedal);
    RUN(pitch_bend);
    RUN(drums);
    RUN(all_sound_off);
    RUN(split_render_is_independent);
    RUN(every_patch);
    RUN(classic_bank);
    RUN(program_change_lets_the_old_string_ring);
    return test_summary();
}
