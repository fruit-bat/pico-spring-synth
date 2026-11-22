//
// ss_synth.c - the spring synthesizer. See ss_synth.h for the model and
// the allocation policy, ss_patch.h for what a patch describes.
//
// Per block, per string (string_render):
//   1. control: pitch factor = pitch bend x vibrato x glide; if it (or a
//      decay) changed, recompute each spring's rotation (one sin/cos each)
//   2. springs: rotate every spring SS_BLOCK_SIZE times, summing the
//      displacements into a mono block (the only per-sample work)
//   3. mix: pan and volume (ramped over the block) into the part's buses,
//      plus the reverb and chorus sends
//   4. cull: drop springs that have fallen below SS_SILENCE
//
// Everything that runs per note or per block is RAM resident on the Pico
// (SS_RAM_FUNC); setup code stays in flash.
//
#include <string.h>
#include <math.h>
#include "ss_synth.h"
#include "ss_math.h"

#define B SS_BLOCK_SIZE
#define TWO_PI 6.2831853f
#define MIDDLE_C 261.62557f

// Highest rotation per sample allowed (radians, 0.46 x the sample rate): a
// pitch bend that pushes a spring up to it is clamped there. At note on
// springs above SS_MAX_PARTIAL_HZ (or this) are left out.
#define W_MAX 2.9f

_Static_assert((SS_BLOCK_SIZE & (SS_BLOCK_SIZE - 1)) == 0, "SS_BLOCK_SIZE must be a power of 2");


// T60 of a choked drum (s)
#define CHOKE_T60 0.06f

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

// Recompute the derived values of a channel from its controllers.
static void SS_RAM_FUNC(channel_update)(SsChannel *ch) {
    const float vol = (float)ch->cc[7] * (1.0f / 127.0f);
    const float expr = (float)ch->cc[11] * (1.0f / 127.0f);
    ch->gain = vol * vol * expr * expr;
    ch->pan = ((float)ch->cc[10] - 64.0f) * (1.0f / 63.0f);
    if (ch->pan < -1.0f) ch->pan = -1.0f;
    ch->reverb = (float)ch->cc[91] * (1.0f / 127.0f);
    ch->chorus = (float)ch->cc[93] * (1.0f / 127.0f);
    ch->mod_depth = (float)ch->cc[1] * (60.0f / 1200.0f / 127.0f); // Full wheel = 60 cents
    const float semis = ((float)ch->bend - 8192.0f) * (1.0f / 8192.0f) * (float)ch->bend_range;
    ch->bend_factor = ss_exp2f(semis * (1.0f / 12.0f));
    ch->drive_scale = 1.0f + (float)ch->pressure * (1.0f / 127.0f);
}

// Reset all controllers (CC121) as GM recommends: modulation, expression,
// sustain, pitch bend, pressure and RPN. Volume, pan and sends are kept.
static void SS_RAM_FUNC(channel_reset_controllers)(SsChannel *ch) {
    ch->cc[1] = 0;
    ch->cc[11] = 127;
    ch->cc[64] = 0;
    ch->bend = 8192;
    ch->pressure = 0;
    ch->rpn_msb = ch->rpn_lsb = 127;
    channel_update(ch);
}

// GM power up defaults for a channel
static void SS_RAM_FUNC(channel_init)(SsSynth *s, uint8_t chan) {
    SsChannel *ch = &s->channels[chan];
    memset(ch, 0, sizeof(*ch));
    ch->cc[7] = 100;
    ch->cc[10] = 64;
    ch->cc[11] = 127;
    ch->cc[91] = 40;
    ch->bend = 8192;
    ch->bend_range = 2;
    ch->rpn_msb = ch->rpn_lsb = 127;
    ch->drums = chan == SS_DRUM_CHANNEL;
    channel_update(ch);
}

// See ss_synth.h.
const SsPatch *SS_RAM_FUNC(ss_synth_channel_patch)(const SsSynth *s, uint8_t chan, uint8_t key) {
    const SsChannel *ch = &s->channels[chan];
    if (ch->drums) {
        const SsDrumKey *d = &ss_drum_map[key & 127];
        return d->patch && d->patch <= ss_drum_patch_count ? &ss_drum_patches[d->patch - 1] : NULL;
    }
    const uint8_t bank = s->force_bank >= 0 ? (uint8_t)s->force_bank : ch->bank;
    if (bank == 1) return &ss_patches[ss_classic_patch];
    return &ss_patches[ss_gm_patch[ch->program & 127]];
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// Recompute the rotations of all springs for pitch factor `pitch` (and
// their current decays).
static void SS_RAM_FUNC(string_coeffs)(SsString *str, float pitch) {
    for (uint32_t i = 0; i < str->count; ++i) {
        SsSpring *p = &str->sp[i];
        float w = p->w * pitch;
        if (w > W_MAX) w = W_MAX;
        ss_sincos(w, &p->s, &p->c);
        p->a = p->r * p->c;
        p->b = p->r * p->s;
        float rb = p->r;
        for (int n = B; n > 1; n >>= 1) rb *= rb;
        p->rb = rb;
    }
    str->pitch = pitch;
    str->dirty = false;
}

// The drive, applied once per block. Pushing a spring of amplitude A by d
// in its direction of motion every sample adds 2d/pi to the amplitude per
// sample on average while the damping takes (1 - r) A, so the amplitude
// moves towards A_eq = 2d / (pi (1 - r)) as
//   A' = A_eq + (A - A_eq) r^B
// over a block of B samples. The spring is given the per-sample rotation
// magnitude (A'/A)^(1/B) for this block, which gets it there. A spring at
// rest is first given a tiny amplitude (the push would start it moving).
// Once the spring has settled the ratio A'/A is close to 1 and its B-th
// root is 1 + (A'/A - 1) / B to well within what the next block corrects.
// While it swells (or dies down) the root is exp(ln(A'/A) / B) with a
// coarse log2 read from the float's bits (within 0.09): a few % off the
// amplitude aimed for in that block, also corrected by the next.
static void SS_RAM_FUNC(string_drive)(SsString *str, float scale) {
    for (uint32_t i = 0; i < str->count; ++i) {
        SsSpring *p = &str->sp[i];
        if (p->drive == 0.0f) continue;
        const float target = p->drive * scale;
        float a2 = p->re * p->re + p->im * p->im;
        if (a2 < 1e-18f) {
            p->im = -1e-9f;
            a2 = 1e-18f;
        }
        const float amp = sqrtf(a2);
        const float ratio = (target + (amp - target) * p->rb) / amp;
        float q;
        if (ratio > 0.9f && ratio < 1.1f) {
            q = 1.0f + (ratio - 1.0f) * (1.0f / B);
        } else {
            // log2 from the bits: exponent + mantissa - 1, then e^x to second order
            union { float f; int32_t i; } u = { ratio };
            const float l2 = (float)u.i * (1.0f / 8388608.0f) - 127.0f;
            const float x = l2 * (0.69314718f / B);
            q = 1.0f + x + 0.5f * x * x;
        }
        p->a = q * p->c;
        p->b = q * p->s;
    }
}

// Highest rotation per sample of a spring at note on
static inline float note_w_max(const SsSynth *s) {
    const float w = TWO_PI * SS_MAX_PARTIAL_HZ / s->sample_rate;
    return w < W_MAX ? w : W_MAX;
}

// Uniform noise in [-1, 1) from a 32-bit LCG
static inline float SS_RAM_FUNC(noise_next)(uint32_t *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return (float)(int32_t)*seed * (1.0f / 2147483648.0f);
}

// Excite a string: the patch's partials for this note and velocity are
// worked out and added to the string. A spring that is already there (the
// key is still ringing) gets the new impulse on top of its motion and its
// held decay and drive back; missing ones are added unless they would be
// inaudible from the start. `freq` is the note frequency in Hz.
// Returns the number of springs added.
static uint32_t SS_RAM_FUNC(string_excite)(SsSynth *s, SsString *str, const SsPatch *pt, float freq, uint8_t vel) {
    const float fs = s->sample_rate;
    const float v = (float)vel * (1.0f / 127.0f);
    const float vel_curve = pt->vel_curve > 0.0f ? pt->vel_curve : 1.5f;
    const float lv = ss_log2f(v);          // velocity in octaves below full (<= 0)
    const float oct = ss_log2f(freq * (1.0f / MIDDLE_C));
    // Overall amplitude: patch gain, velocity curve, pitch tilt (+-18 dB)
    float amp = pt->gain * ss_exp2f(lv * vel_curve) * ss_clampf(ss_exp2f(-oct * pt->tilt), 0.125f, 8.0f);
    // Decay times scale with pitch
    const float t60_scale = ss_exp2f(-oct * pt->decay_track);
    const float glide = pt->glide > 0.0f ? pt->glide : 1.0f;
    const float w_max = note_w_max(s) / (glide > 1.0f ? glide : 1.0f);
    const float r_damp_t60 = pt->release > 0.0f ? pt->release * t60_scale : 0.0f;
    // Noise burst: per-sample excitation scaled so that a partial reaches
    // about `noise` x its gain over the burst (a random walk: amplitude
    // grows with the square root of the burst length)
    const float noise_samples = (pt->noise_ms > 0.0f ? pt->noise_ms : 1.0f) * 0.001f * fs;
    uint32_t added = 0;
    // Stiffness, hammer and soundboard filters (piano), on absolute frequency
    // Stiffness sharpens partial n by sqrt(1 + B n^2); divided by the
    // fundamental's own sqrt(1 + B), so the note stays at its tuned pitch
    const float inharm = pt->inharm > 0.0f ? pt->inharm * ss_exp2f(oct * pt->inharm_track) : 0.0f;
    const float inharm_norm = 1.0f / sqrtf(1.0f + inharm);
    const float inv_fc = pt->cutoff_hz > 0.0f ? 1.0f / (pt->cutoff_hz * ss_exp2f(lv * pt->cutoff_vel)) : 0.0f;
    const float inv_fb = pt->body_hz > 0.0f ? 1.0f / pt->body_hz : 0.0f;

    // Struck again while ringing: the hammer or plectrum absorbs some of
    // the motion it meets. Like the strike itself this is a force, so it
    // changes the velocity (im) and never the displacement (re): scaling
    // re as well would be a step in the waveform, a click.
    if (pt->restrike_damp > 0.0f) {
        const float keep = 1.0f - pt->restrike_damp;
        float e = 0.0f;
        for (uint32_t i = 0; i < str->count; ++i) {
            SsSpring *p = &str->sp[i];
            p->im *= keep;
            e += p->re * p->re + p->im * p->im;
        }
        str->energy = e;
    }

    for (uint32_t j = 0; j < pt->count; ++j) {
        const SsPartial *pp = &pt->p[j];
        const bool fixed = pp->ratio < 0.0f;
        float ratio = fixed ? -pp->ratio / freq : pp->ratio;
        if (inharm > 0.0f && !fixed) ratio *= sqrtf(1.0f + inharm * ratio * ratio) * inharm_norm;
        const float w = TWO_PI * freq * ratio / fs;
        if (w > w_max || w <= 0.0f) continue; // Inaudible or would alias (or nonsense)
        // Brighter with velocity: octaves above the fundamental lose
        // `bright` x the velocity's octaves
        const float bright = ratio > 1.0f ? ss_exp2f(lv * pt->bright * ss_log2f(ratio)) : 1.0f;
        float g = amp * bright;
        const float hz = freq * ratio;
        if (inv_fc > 0.0f) {
            const float x = hz * inv_fc, x2 = x * x;
            g /= sqrtf(1.0f + x2 * x2);
        }
        if (inv_fb > 0.0f) {
            const float y = hz * inv_fb, y2 = y * y;
            g *= y2 / sqrtf(1.0f + y2 * y2);
        }
        const float pluck = pp->gain * g;
        const float drive_amp = pp->drive * g;
        const float t60 = fixed ? pp->t60 : pp->t60 * t60_scale;
        const float r = ss_t60_to_decay(t60, fs);
        // Release adds the damper's rate to the string's own
        const float r_rel = r * ss_t60_to_decay(r_damp_t60, fs);
        const float drive = drive_amp; // See string_drive
        const float exc = pp->gain * g * pt->noise;

        SsSpring *sp = NULL;
        for (uint32_t i = 0; i < str->count; ++i) {
            if (str->sp[i].idx == j) { sp = &str->sp[i]; break; }
        }
        if (!sp) {
            // New spring: skip it if it would never be heard, or is 50 dB
            // below the note's level (the filters leave many such)
            const float floor = fmaxf(SS_SILENCE, 3e-3f * amp);
            if (fabsf(pluck) < floor && drive == 0.0f && fabsf(exc) < floor) continue;
            if (str->count >= SS_MAX_PARTIALS) continue;
            sp = &str->sp[str->count++];
            memset(sp, 0, sizeof(*sp));
            sp->idx = (uint8_t)j;
            ++added;
        }
        sp->w = w;
        sp->r = r;
        sp->r_rel = r_rel;
        sp->drive = drive;
        sp->exc = exc;
        sp->im -= pluck; // Velocity impulse: im = -v / omega
        str->energy += pluck * pluck + drive_amp * drive_amp;
    }

    if (pt->noise > 0.0f || pt->noise_out > 0.0f) {
        str->noise_env = v;
        str->noise_decay = ss_expf(-1.0f / noise_samples);
        // 0 when the burst only goes to the output: the springs then keep
        // their fast loop and their culling
        str->noise_scale = pt->noise > 0.0f ? sqrtf(6.0f / noise_samples) : 0.0f;
        str->noise_out = pt->noise_out * ss_exp2f(lv * vel_curve) * pt->gain;
        // The direct noise is band passed around 20x the note (600 Hz ..
        // 16 kHz): a bright tick on a high note, a dull knock on a low one,
        // where a broadband tick would stand out from the note as a tap
        const float fc = ss_clampf(20.0f * freq, 600.0f, 16000.0f);
        str->noise_lp = 1.0f - ss_expf(-TWO_PI * fc / fs);
    }
    if (pt->knock > 0.0f) {
        const float kc = 1.0f - ss_expf(-TWO_PI * (pt->knock_hz > 0.0f ? pt->knock_hz : 200.0f) / fs);
        str->knock_c = kc;
        // Scaled so the low passed noise has about the knock's amplitude
        str->knock_env = pt->knock * pt->gain * ss_exp2f(lv * vel_curve) * sqrtf(3.0f / kc);
        str->knock_floor = 0.01f * str->knock_env;
        str->knock_decay = ss_expf(-1.0f / ((pt->knock_ms > 0.0f ? pt->knock_ms : 30.0f) * 0.001f * fs));
    }
    if (glide != 1.0f) {
        str->glide = glide;
        str->glide_k = ss_expf(-(float)B / ((pt->glide_ms > 0.0f ? pt->glide_ms : 20.0f) * 0.001f * fs));
    }
    str->dirty = true;
    return added;
}

// Key released: drive off, release damping on
static void SS_RAM_FUNC(string_release)(SsString *str) {
    str->state = SS_STR_RELEASED;
    for (uint32_t i = 0; i < str->count; ++i) {
        str->sp[i].drive = 0.0f;
        str->sp[i].r = str->sp[i].r_rel;
    }
    str->dirty = true;
}

// Damp every spring to at least per-sample decay r (steals, chokes, sound off)
static void SS_RAM_FUNC(string_damp)(SsString *str, float r) {
    for (uint32_t i = 0; i < str->count; ++i) {
        SsSpring *p = &str->sp[i];
        p->drive = 0.0f;
        if (p->r > r) p->r = r;
        if (p->r_rel > r) p->r_rel = r;
    }
    str->noise_env = 0.0f;
    str->dirty = true;
}

// Take a string off the channel + key map (if it is the one mapped there)
static void SS_RAM_FUNC(string_unmap)(SsSynth *s, uint8_t index) {
    SsString *str = &s->strings[index];
    if (str->mapped && s->key_string[str->chan][str->key] == index + 1) s->key_string[str->chan][str->key] = 0;
    str->mapped = false;
}

// Free the string at active[] position ai: back to the free list, and the
// last active entry moves into its place.
static void SS_RAM_FUNC(string_free)(SsSynth *s, uint32_t ai) {
    const uint8_t index = s->active[ai];
    SsString *str = &s->strings[index];
    string_unmap(s, index);
    if (str->state == SS_STR_DYING) --s->dying_count;
    else s->partials -= str->count;
    str->count = 0;
    s->active[ai] = s->active[--s->active_count];
    s->free_slots[s->free_count++] = index;
}

// Stop the string at active[] position ai immediately: its last output
// goes to the declick so the cut does not click.
static void SS_RAM_FUNC(string_cut)(SsSynth *s, uint32_t ai) {
    SsString *str = &s->strings[s->active[ai]];
    s->declick_l += str->last_l;
    s->declick_r += str->last_r;
    string_free(s, ai);
    ++s->stats.cuts;
}

// Silence a fading string at once: its last output goes to the declick
// and it renders nothing more (it is freed at the end of the block). The
// active list is not changed, so callers may be walking it.
static void SS_RAM_FUNC(string_silence)(SsSynth *s, SsString *str) {
    s->declick_l += str->last_l;
    s->declick_r += str->last_r;
    str->last_l = str->last_r = 0.0f;
    str->count = 0;
    str->noise_env = 0.0f;
    str->fade = 0;
    ++s->stats.cuts;
}

// Make the string at active[] position ai fade out over SS_FADE_BLOCKS in
// its slot. It no longer counts against the limits. If SS_STEAL_RESERVE
// strings are fading already, the one that has faded furthest is silenced
// instead. Does not change the active list.
static void SS_RAM_FUNC(string_kill)(SsSynth *s, uint32_t ai) {
    const uint8_t index = s->active[ai];
    SsString *str = &s->strings[index];
    if (str->state == SS_STR_DYING) return;
    uint32_t fading = 0;
    SsString *furthest = NULL;
    for (uint32_t i = 0; i < s->active_count; ++i) {
        SsString *d = &s->strings[s->active[i]];
        if (d->state != SS_STR_DYING || d->fade == 0) continue;
        ++fading;
        if (!furthest || d->fade < furthest->fade) furthest = d;
    }
    if (fading >= SS_STEAL_RESERVE && furthest) string_silence(s, furthest);
    string_unmap(s, index);
    for (uint32_t i = 0; i < str->count; ++i) str->sp[i].drive = 0.0f;
    str->noise_env = 0.0f;
    s->partials -= str->count;
    str->state = SS_STR_DYING;
    str->fade = SS_FADE_BLOCKS;
    ++s->dying_count;
}

// Steal priority: the string with the lowest score goes first. Energy is
// the sum of the squared spring amplitudes, times the channel's gain
// squared, i.e. how loud it is now. Released strings count 4x less.
static float SS_RAM_FUNC(steal_score)(const SsSynth *s, const SsString *str) {
    const float g = s->channels[str->chan].gain;
    float score = str->energy * g * g + 1e-12f;
    if (str->state == SS_STR_RELEASED) score *= 0.25f;
    return score;
}

// Make room for a new string of `need` partials: steal the quietest
// strings while the string count or the cost budget would be exceeded,
// then return a free slot (cutting the oldest-fading string if all the
// spare slots are busy fading).
static SsString *SS_RAM_FUNC(string_alloc)(SsSynth *s, uint32_t need) {
    while (s->active_count - s->dying_count >= SS_MAX_STRINGS ||
           (ss_synth_cost(s) + need + SS_STRING_COST > SS_COST_BUDGET && s->active_count > s->dying_count)) {
        int32_t victim = -1;
        float best = 0.0f;
        for (uint32_t i = 0; i < s->active_count; ++i) {
            const SsString *str = &s->strings[s->active[i]];
            if (str->state == SS_STR_DYING) continue;
            const float sc = steal_score(s, str);
            if (victim < 0 || sc < best) { victim = (int32_t)i; best = sc; }
        }
        if (victim < 0) break;
        string_kill(s, (uint32_t)victim);
        ++s->stats.steals;
    }
    if (s->free_count == 0) {
        // Every slot is in use, so some strings are dying: reuse one that
        // is already silent, else cut the one that has faded furthest
        int32_t victim = -1;
        for (uint32_t i = 0; i < s->active_count; ++i) {
            const SsString *str = &s->strings[s->active[i]];
            if (str->state != SS_STR_DYING) continue;
            if (victim < 0 || str->fade < s->strings[s->active[victim]].fade) victim = (int32_t)i;
        }
        if (victim < 0) return NULL; // Cannot happen: SS_STEAL_RESERVE > 0
        if (s->strings[s->active[victim]].fade == 0) string_free(s, (uint32_t)victim);
        else string_cut(s, (uint32_t)victim);
    }
    const uint8_t index = s->free_slots[--s->free_count];
    s->active[s->active_count++] = index;
    SsString *str = &s->strings[index];
    memset(str, 0, sizeof(*str));
    return str;
}

// Silence a choke group on a channel (e.g. a closed hi-hat stops the open one)
static void SS_RAM_FUNC(choke_group)(SsSynth *s, uint8_t chan, uint8_t group, const SsString *except) {
    for (uint32_t i = 0; i < s->active_count; ++i) {
        SsString *str = &s->strings[s->active[i]];
        if (str != except && str->chan == chan && str->group == group && str->state != SS_STR_DYING) {
            string_damp(str, s->choke_decay);
            str->state = SS_STR_RELEASED;
        }
    }
}

// The patch a key actually plays: pt itself, its upper patch, or (between
// blend_lo and blend_hi) a per-key interpolation of the two, built in *tmp.
// Upper patches can chain (an upper patch with its own upper).
static const SsPatch *SS_RAM_FUNC(patch_at_key)(const SsPatch *pt, uint8_t key, SsPatch *tmp) {
    const SsPatch *up = pt->upper;
    while (up && key >= pt->blend_hi) { pt = up; up = pt->upper; }
    if (!up || key <= pt->blend_lo) return pt;
    if (key >= pt->blend_hi) return up;
    const float w = (float)(key - pt->blend_lo) / (float)(pt->blend_hi - pt->blend_lo);
#define LERP(f) tmp->f = pt->f + (up->f - pt->f) * w
    *tmp = *pt;
    LERP(gain); LERP(decay_track); LERP(release); LERP(restrike_damp); LERP(tilt); LERP(vel_curve);
    LERP(bright); LERP(inharm); LERP(inharm_track); LERP(cutoff_hz); LERP(cutoff_vel); LERP(body_hz);
    LERP(noise); LERP(noise_out); LERP(noise_ms); LERP(knock); LERP(knock_hz); LERP(knock_ms);
    LERP(vibrato); LERP(vib_rate); LERP(vib_delay); LERP(pan_spread); LERP(stretch);
    for (uint32_t j = 0; j < pt->count; ++j) {
        LERP(p[j].ratio); LERP(p[j].gain); LERP(p[j].t60); LERP(p[j].drive);
    }
#undef LERP
    // The hammer cutoff is interpolated as 1 / cutoff, since 0 means no
    // low pass (an infinite cutoff). (The soundboard corner's 0 is 0 Hz, so
    // its plain interpolation is right.)
    const float inv_lo = pt->cutoff_hz > 0.0f ? 1.0f / pt->cutoff_hz : 0.0f;
    const float inv_up = up->cutoff_hz > 0.0f ? 1.0f / up->cutoff_hz : 0.0f;
    const float inv = inv_lo + (inv_up - inv_lo) * w;
    tmp->cutoff_hz = inv > 0.0f ? 1.0f / inv : 0.0f;
    return tmp;
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_note_on_patch)(SsSynth *s, uint8_t chan, uint8_t key, uint8_t vel,
                                         const SsPatch *pt, float hz, float pan, uint8_t group) {
    if (chan >= SS_MIDI_CHANNELS || key > 127 || !pt || vel == 0) return;
    // What this key plays (pt still identifies the string's patch)
    SsPatch blend;
    const SsPatch *xp = patch_at_key(pt, key, &blend);
    // Equal temperament, sharpened above stretch_key if the patch says so
    float semis = (float)key - 69.0f;
    if (xp->stretch != 0.0f && key > xp->stretch_key) {
        const float o = ((float)key - (float)xp->stretch_key) * (1.0f / 12.0f);
        semis += xp->stretch * o * o * 0.01f;
    }
    const float freq = hz > 0.0f ? hz : 440.0f * ss_exp2f(semis * (1.0f / 12.0f));

    SsString *str = NULL;
    const uint8_t mapped = s->key_string[chan][key];
    if (mapped) {
        str = &s->strings[mapped - 1];
        if (str->patch != pt) {
            // Program changed since: the old string rings on, unreachable
            if (str->state != SS_STR_RELEASED) string_release(str);
            string_unmap(s, (uint8_t)(mapped - 1));
            str = NULL;
        }
    }

    if (str) {
        // Strike the ringing string again
        str->state = SS_STR_HELD;
        s->partials += string_excite(s, str, xp, freq, vel);
        ++s->stats.restrikes;
    } else {
        // Count the partials that will sound, to make room for them
        uint32_t need = 0;
        for (uint32_t j = 0; j < xp->count; ++j) {
            const float hz = xp->p[j].ratio < 0.0f ? -xp->p[j].ratio : freq * xp->p[j].ratio;
            if (TWO_PI * hz / s->sample_rate <= note_w_max(s)) ++need;
        }
        str = string_alloc(s, need);
        if (!str) return;
        const uint8_t index = (uint8_t)(str - s->strings);
        str->chan = chan;
        str->key = key;
        str->state = SS_STR_HELD;
        str->patch = pt;
        str->group = group;
        str->glide = 1.0f;
        str->pitch = 1.0f;
        str->vib_depth = xp->vibrato * (1.0f / 1200.0f);
        str->vib_step = (xp->vib_rate > 0.0f ? xp->vib_rate : 5.5f) * (float)B / s->sample_rate;
        str->vib_env_step = xp->vib_delay > 0.0f ? (float)B / (xp->vib_delay * s->sample_rate) : 1.0f;
        str->vib_phase = (float)(key % 12) * (1.0f / 12.0f); // Strings of a chord do not vibrate in step
        str->pan = ss_clampf(pan + xp->pan_spread * ((float)key - 64.0f) * (1.0f / 48.0f), -1.0f, 1.0f);
        s->noise_seed = s->noise_seed * 69069u + 12345u;
        str->noise_seed = s->noise_seed;
        s->partials += string_excite(s, str, xp, freq, vel);
        str->mapped = true;
        s->key_string[chan][key] = (uint8_t)(index + 1);
        ++s->stats.strings_started;
    }
    if (group) choke_group(s, chan, group, str);
    ++s->stats.notes;
    if (s->active_count > s->stats.peak_strings) s->stats.peak_strings = s->active_count;
    if (s->partials > s->stats.peak_partials) s->stats.peak_partials = s->partials;
    if (ss_synth_cost(s) > s->stats.peak_cost) s->stats.peak_cost = ss_synth_cost(s);
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_note_on)(SsSynth *s, uint8_t chan, uint8_t key, uint8_t vel) {
    if (chan >= SS_MIDI_CHANNELS || key > 127) return;
    if (vel == 0) {
        ss_synth_note_off(s, chan, key);
        return;
    }
    const SsChannel *ch = &s->channels[chan];
    const SsPatch *pt = ss_synth_channel_patch(s, chan, key);
    if (!pt) return;
    if (ch->drums) {
        const SsDrumKey *d = &ss_drum_map[key];
        ss_synth_note_on_patch(s, chan, key, vel, pt, d->hz > 0.0f ? d->hz : pt->hz, d->pan, d->group);
    } else {
        ss_synth_note_on_patch(s, chan, key, vel, pt, pt->hz, 0.0f, 0);
    }
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_note_off)(SsSynth *s, uint8_t chan, uint8_t key) {
    if (chan >= SS_MIDI_CHANNELS || key > 127) return;
    const uint8_t mapped = s->key_string[chan][key];
    if (!mapped) return;
    SsString *str = &s->strings[mapped - 1];
    if (str->state != SS_STR_HELD || (str->patch->flags & SS_PATCH_ONE_SHOT)) return;
    if (s->channels[chan].cc[64] >= 64) str->state = SS_STR_SUSTAINED;
    else string_release(str);
}

// Pedal up: release the channel's sustained strings
static void SS_RAM_FUNC(release_sustained)(SsSynth *s, uint8_t chan) {
    for (uint32_t i = 0; i < s->active_count; ++i) {
        SsString *str = &s->strings[s->active[i]];
        if (str->chan == chan && str->state == SS_STR_SUSTAINED) string_release(str);
    }
}

// All notes off (CC123-127): as a note off for every held key
static void SS_RAM_FUNC(all_notes_off)(SsSynth *s, uint8_t chan) {
    const bool sustain = s->channels[chan].cc[64] >= 64;
    for (uint32_t i = 0; i < s->active_count; ++i) {
        SsString *str = &s->strings[s->active[i]];
        if (str->chan != chan || str->state != SS_STR_HELD || (str->patch->flags & SS_PATCH_ONE_SHOT)) continue;
        if (sustain) str->state = SS_STR_SUSTAINED;
        else string_release(str);
    }
}

// All sound off (CC120) for one channel: fade everything out fast
static void SS_RAM_FUNC(channel_sound_off)(SsSynth *s, uint8_t chan) {
    for (uint32_t i = 0; i < s->active_count; ++i) {
        if (s->strings[s->active[i]].chan == chan) string_kill(s, i);
    }
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_control_change)(SsSynth *s, uint8_t chan, uint8_t cc, uint8_t value) {
    if (chan >= SS_MIDI_CHANNELS || cc > 127) return;
    SsChannel *ch = &s->channels[chan];
    value &= 0x7f;
    switch (cc) {
        case 0:
            ch->bank = value;
            // GM2 / GS style: bank 120 (or 127) is a drum kit
            if (value == 120 || value == 127) ch->drums = true;
            else if (chan != SS_DRUM_CHANNEL) ch->drums = false;
            break;
        case 6: // Data entry MSB
            if (ch->rpn_msb == 0 && ch->rpn_lsb == 0) {
                ch->bend_range = value > 24 ? 24 : value;
                channel_update(ch);
            }
            break;
        case 64:
            ch->cc[64] = value;
            if (value < 64) release_sustained(s, chan);
            break;
        case 98: case 99: // NRPN select: deselect RPN
            ch->rpn_msb = ch->rpn_lsb = 127;
            break;
        case 100: ch->rpn_lsb = value; break;
        case 101: ch->rpn_msb = value; break;
        case 120: channel_sound_off(s, chan); break;
        case 121: channel_reset_controllers(ch); release_sustained(s, chan); break;
        case 123: case 124: case 125: case 126: case 127: all_notes_off(s, chan); break;
        default:
            ch->cc[cc] = value;
            channel_update(ch);
            break;
    }
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_program_change)(SsSynth *s, uint8_t chan, uint8_t program) {
    if (chan >= SS_MIDI_CHANNELS) return;
    s->channels[chan].program = program & 0x7f;
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_pitch_bend)(SsSynth *s, uint8_t chan, uint16_t value) {
    if (chan >= SS_MIDI_CHANNELS) return;
    s->channels[chan].bend = value > 16383 ? 16383 : value;
    channel_update(&s->channels[chan]);
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_channel_pressure)(SsSynth *s, uint8_t chan, uint8_t value) {
    if (chan >= SS_MIDI_CHANNELS) return;
    s->channels[chan].pressure = value & 0x7f;
    channel_update(&s->channels[chan]);
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_all_sound_off)(SsSynth *s) {
    for (uint32_t i = 0; i < s->active_count; ++i) string_kill(s, i);
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_reset)(SsSynth *s) {
    ss_synth_all_sound_off(s);
    for (uint8_t c = 0; c < SS_MIDI_CHANNELS; ++c) channel_init(s, c);
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_midi)(SsSynth *s, const uint8_t *msg, uint32_t len) {
    if (len == 0) return;
    const uint8_t status = msg[0];
    const uint8_t chan = status & 0x0f;
    const uint8_t d1 = len > 1 ? msg[1] & 0x7f : 0;
    const uint8_t d2 = len > 2 ? msg[2] & 0x7f : 0;
    switch (status & 0xf0) {
        case 0x80: ss_synth_note_off(s, chan, d1); break;
        case 0x90: ss_synth_note_on(s, chan, d1, d2); break;
        case 0xb0: ss_synth_control_change(s, chan, d1, d2); break;
        case 0xc0: ss_synth_program_change(s, chan, d1); break;
        case 0xd0: ss_synth_channel_pressure(s, chan, d1); break;
        case 0xe0: ss_synth_pitch_bend(s, chan, (uint16_t)(d1 | (d2 << 7))); break;
        case 0xf0:
            if (status != 0xf0) break;
            if (len >= 6 && msg[1] == 0x7e && msg[3] == 0x09 && (msg[4] == 0x01 || msg[4] == 0x03)) {
                ss_synth_reset(s); // GM / GM2 system on
            } else if (len >= 10 && msg[1] == 0x41 && msg[3] == 0x42 && msg[4] == 0x12 &&
                       msg[5] == 0x40 && msg[6] == 0x00 && msg[7] == 0x7f) {
                ss_synth_reset(s); // GS reset
            } else if (len >= 10 && msg[1] == 0x41 && msg[3] == 0x42 && msg[4] == 0x12 &&
                       msg[5] == 0x40 && msg[6] == 0x02 && msg[7] <= 0x03) {
                // GS master EQ: F0 41 dev 42 12 40 02 0p vv sum F7
                if (((0x40 + 0x02 + msg[7] + msg[8] + msg[9]) & 0x7f) == 0) ss_synth_set_eq_gs(s, msg[7], msg[8]);
            } else if (len >= 7 && msg[1] == 0x7f && msg[3] == 0x04 && msg[4] == 0x01) {
                ss_synth_set_master_volume(s, (uint16_t)((msg[5] & 0x7f) | ((msg[6] & 0x7f) << 7)));
            } else if (len == 7 && msg[1] == 0x7d && msg[2] == 0x53 && msg[3] == 0x53 && msg[4] == 0x03) {
                ss_synth_set_width(s, msg[5]); // Stereo width: F0 7D 53 53 03 ww F7
            }
            break;
        default: break; // Poly pressure not supported
    }
}

// ---------------------------------------------------------------------------
// Set up and settings
// ---------------------------------------------------------------------------

// See ss_synth.h.
void ss_synth_init(SsSynth *s, uint32_t sample_rate) {
    memset(s, 0, sizeof(*s));
    if (sample_rate > SS_MAX_SAMPLE_RATE) sample_rate = SS_MAX_SAMPLE_RATE;
    s->sample_rate = (float)sample_rate;
    s->gain = SS_DEFAULT_GAIN;
    s->master_volume = 1.0f;
    s->reverb_enabled = SS_ENABLE_REVERB;
    s->chorus_enabled = SS_ENABLE_CHORUS;
    s->force_bank = -1;
    s->declick_decay = expf(-1.0f / (0.001f * s->sample_rate));
    s->choke_decay = ss_t60_to_decay(CHOKE_T60, s->sample_rate);
    s->noise_seed = 0x5eed1234u;
    s->dc_coef = 1.0f - TWO_PI * 10.0f / s->sample_rate; // 10 Hz
    for (uint32_t i = 0; i < SS_TOTAL_STRINGS; ++i) s->free_slots[i] = (uint8_t)(SS_TOTAL_STRINGS - 1 - i);
    s->free_count = SS_TOTAL_STRINGS;
    for (uint8_t c = 0; c < SS_MIDI_CHANNELS; ++c) channel_init(s, c);
    s->width_pct = 100;
    s->width = 1.0f;
    s->eq_gs[1] = s->eq_gs[3] = 0x40;
    ss_eq_init(&s->eq, s->sample_rate);
#if SS_ENABLE_REVERB
    ss_reverb_init(&s->reverb, s->sample_rate);
#endif
#if SS_ENABLE_CHORUS
    ss_chorus_init(&s->chorus, s->sample_rate);
#endif
}

// See ss_synth.h. Clamps every value to 0..1.
void ss_synth_set_reverb(SsSynth *s, float room, float damp, float width, float level) {
#if SS_ENABLE_REVERB
    ss_reverb_set(&s->reverb, ss_clampf(room, 0.0f, 1.0f), ss_clampf(damp, 0.0f, 1.0f),
                  ss_clampf(width, 0.0f, 1.0f), ss_clampf(level, 0.0f, 1.0f));
#else
    (void)s; (void)room; (void)damp; (void)width; (void)level;
#endif
}

// See ss_synth.h. The base delay is fixed.
void ss_synth_set_chorus(SsSynth *s, float rate_hz, float depth_ms, float level) {
#if SS_ENABLE_CHORUS
    ss_chorus_set(&s->chorus, s->sample_rate, ss_clampf(rate_hz, 0.01f, 10.0f), SS_CHORUS_DEFAULT_BASE,
                  ss_clampf(depth_ms, 0.0f, 20.0f), ss_clampf(level, 0.0f, 4.0f));
#else
    (void)s; (void)rate_hz; (void)depth_ms; (void)level;
#endif
}

// See ss_synth.h. Gains are clamped to +-24 dB, beyond the GS range, for
// host use.
void ss_synth_set_eq(SsSynth *s, float low_hz, float low_db, float high_hz, float high_db) {
    ss_eq_set(&s->eq, s->sample_rate, low_hz, ss_clampf(low_db, -24.0f, 24.0f), high_hz, ss_clampf(high_db, -24.0f, 24.0f));
}

// See ss_synth.h. Stores the GS parameter (clamped to its legal range; it
// is what the firmware's settings save) and rebuilds the EQ from all four.
void ss_synth_set_eq_gs(SsSynth *s, uint8_t param, uint8_t value) {
    if (param > 3) return;
    if (param == 1 || param == 3) value = value < 0x34 ? 0x34 : value > 0x4c ? 0x4c : value;
    else value = value ? 1 : 0;
    s->eq_gs[param] = value;
    ss_synth_set_eq(s, s->eq_gs[0] ? 400.0f : 200.0f, (float)((int)s->eq_gs[1] - 0x40),
                    s->eq_gs[2] ? 6000.0f : 3000.0f, (float)((int)s->eq_gs[3] - 0x40));
}

// See ss_synth.h.
void ss_synth_set_width(SsSynth *s, uint8_t percent) {
    if (percent > 100) percent = 100;
    s->width_pct = percent;
    s->width = (float)percent * 0.01f;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Rotate every spring of a string through one block, adding the
// displacements to mono[]. noise[] (or NULL) is the block's burst, already
// scaled; each spring takes it with its own weight (exc).
//
// This is the inner loop of the whole synth. Springs go four (or two) at a
// time so they share the load and store of mono[k] (and noise[k]) and the
// loop overhead. springs_n() is written once and inlined into two
// variants, with and without the noise, so the common case does no extra
// work.
static inline __attribute__((always_inline)) uint32_t springs_n(SsSpring *sp, uint32_t n, float *mono,
                                                                const float *noise, const bool with_noise) {
    uint32_t i = 0;
    for (; i + 3 < n; i += 4) {
        SsSpring *p1 = &sp[i], *p2 = p1 + 1, *p3 = p1 + 2, *p4 = p1 + 3;
        float re1 = p1->re, im1 = p1->im, re2 = p2->re, im2 = p2->im;
        float re3 = p3->re, im3 = p3->im, re4 = p4->re, im4 = p4->im;
        const float a1 = p1->a, b1 = p1->b, a2 = p2->a, b2 = p2->b;
        const float a3 = p3->a, b3 = p3->b, a4 = p4->a, b4 = p4->b;
        const float e1 = p1->exc, e2 = p2->exc, e3 = p3->exc, e4 = p4->exc;
        for (int k = 0; k < B; ++k) {
            const float t1 = a1 * re1 - b1 * im1;
            const float t2 = a2 * re2 - b2 * im2;
            const float t3 = a3 * re3 - b3 * im3;
            const float t4 = a4 * re4 - b4 * im4;
            im1 = b1 * re1 + a1 * im1;
            im2 = b2 * re2 + a2 * im2;
            im3 = b3 * re3 + a3 * im3;
            im4 = b4 * re4 + a4 * im4;
            if (with_noise) {
                const float nz = noise[k];
                im1 += nz * e1;
                im2 += nz * e2;
                im3 += nz * e3;
                im4 += nz * e4;
            }
            re1 = t1;
            re2 = t2;
            re3 = t3;
            re4 = t4;
            mono[k] += (t1 + t2) + (t3 + t4);
        }
        p1->re = re1; p1->im = im1; p2->re = re2; p2->im = im2;
        p3->re = re3; p3->im = im3; p4->re = re4; p4->im = im4;
    }
    for (; i + 1 < n; i += 2) {
        SsSpring *p1 = &sp[i], *p2 = p1 + 1;
        float re1 = p1->re, im1 = p1->im, re2 = p2->re, im2 = p2->im;
        const float a1 = p1->a, b1 = p1->b, a2 = p2->a, b2 = p2->b;
        const float e1 = p1->exc, e2 = p2->exc;
        for (int k = 0; k < B; ++k) {
            const float t1 = a1 * re1 - b1 * im1;
            const float t2 = a2 * re2 - b2 * im2;
            im1 = b1 * re1 + a1 * im1;
            im2 = b2 * re2 + a2 * im2;
            if (with_noise) {
                im1 += noise[k] * e1;
                im2 += noise[k] * e2;
            }
            re1 = t1;
            re2 = t2;
            mono[k] += t1 + t2;
        }
        p1->re = re1; p1->im = im1; p2->re = re2; p2->im = im2;
    }
    if (i < n) {
        SsSpring *p = &sp[i];
        float re = p->re, im = p->im;
        const float a = p->a, b = p->b, e = p->exc;
        for (int k = 0; k < B; ++k) {
            const float t = a * re - b * im;
            im = b * re + a * im;
            if (with_noise) im += noise[k] * e;
            re = t;
            mono[k] += re;
        }
        p->re = re;
        p->im = im;
    }
    return n;
}

static void SS_RAM_FUNC(render_springs)(SsString *str, float *mono, const float *noise) {
    if (noise) springs_n(str->sp, str->count, mono, noise, true);
    else springs_n(str->sp, str->count, mono, NULL, false);
}

// The part's output buses
typedef struct {
    float *l, *r, *reverb, *chorus;
} SsBuses;

// Render one block of a string into the buses. Returns false when the
// string has finished (no springs left and no noise burst running).
static bool SS_RAM_FUNC(string_render)(SsString *str, const SsChannel *ch, const SsBuses *bus) {
    // 1. Control: pitch = bend x vibrato x glide
    float pitch = ch->bend_factor;
    if (str->glide != 1.0f) {
        str->glide = 1.0f + (str->glide - 1.0f) * str->glide_k;
        if (fabsf(str->glide - 1.0f) < 1e-5f) str->glide = 1.0f;
        pitch *= str->glide;
    }
    const float vib = str->vib_depth * str->vib_env + ch->mod_depth;
    if (vib > 0.0f) {
        str->vib_phase += str->vib_step;
        if (str->vib_phase >= 1.0f) str->vib_phase -= 1.0f;
        pitch *= ss_exp2f(vib * ss_sin_turns(str->vib_phase));
    }
    if (str->vib_env < 1.0f) {
        str->vib_env += str->vib_env_step;
        if (str->vib_env > 1.0f) str->vib_env = 1.0f;
    }
    if (str->dirty || pitch != str->pitch) string_coeffs(str, pitch);
    if (str->state <= SS_STR_SUSTAINED) string_drive(str, ch->drive_scale);


    // 2. Springs (and the noise burst)
    float mono[B];
    memset(mono, 0, sizeof(mono));
    if (str->noise_env > 0.0f) {
        float nz[B];
        float env = str->noise_env;
        const float nd = str->noise_decay, ns = str->noise_scale, no = str->noise_out;
        float prev = str->noise_prev, y = str->noise_y;
        const float lp = str->noise_lp;
        uint32_t seed = str->noise_seed;
        for (int k = 0; k < B; ++k) {
            const float n = noise_next(&seed) * env;
            nz[k] = n * ns;
            y += lp * ((n - prev) - y);  // Direct: first difference (high pass), then low pass
            mono[k] = y * no;
            prev = n;
            env *= nd;
        }
        str->noise_seed = seed;
        str->noise_prev = prev;
        str->noise_y = y;
        // Stop 40 dB down: the springs have taken in nearly all of the
        // burst, and what is left of it is far below the springs' ringing
        str->noise_env = env < 1e-2f ? 0.0f : env;
        render_springs(str, mono, ns != 0.0f ? nz : NULL);
    } else {
        render_springs(str, mono, NULL);
    }
    if (str->knock_env > 0.0f) {
        // The knock: noise through two one pole low passes, decaying
        float env = str->knock_env, y1 = str->knock_y1, y2 = str->knock_y2;
        const float kc = str->knock_c, kd = str->knock_decay;
        uint32_t seed = str->noise_seed;
        for (int k = 0; k < B; ++k) {
            y1 += kc * (noise_next(&seed) * env - y1);
            y2 += kc * (y1 - y2);
            mono[k] += y2;
            env *= kd;
        }
        str->noise_seed = seed;
        str->knock_y1 = y1;
        str->knock_y2 = y2;
        str->knock_env = env < str->knock_floor ? 0.0f : env;
    }

    // 3. Mix: equal power pan, volume ramped over the block
    float sn, cs;
    const float pan = ss_clampf(ch->pan + str->pan, -1.0f, 1.0f);
    ss_sincos((pan + 1.0f) * 0.78539816f, &sn, &cs);
    float tgl = cs * ch->gain, tgr = sn * ch->gain;
    if (str->state == SS_STR_DYING) {
        // Stolen: the gain ramps down to 0 over the remaining fade blocks
        const float f = str->fade ? (float)(str->fade - 1) * (1.0f / SS_FADE_BLOCKS) : 0.0f;
        tgl *= f;
        tgr *= f;
        if (str->fade) --str->fade;
    }
    float gl = str->gl, gr = str->gr;
    const float dgl = (tgl - gl) * (1.0f / B), dgr = (tgr - gr) * (1.0f / B);
    float *bl = bus->l, *br = bus->r;
    if (bus->reverb && ch->reverb > 0.0f) {
        // One pass for the dry mix and the reverb send
        const float g = ch->gain * ch->reverb;
        float *rv = bus->reverb;
        for (int k = 0; k < B; ++k) {
            const float m = mono[k];
            gl += dgl;
            gr += dgr;
            bl[k] += m * gl;
            br[k] += m * gr;
            rv[k] += m * g;
        }
    } else {
        for (int k = 0; k < B; ++k) {
            const float m = mono[k];
            gl += dgl;
            gr += dgr;
            bl[k] += m * gl;
            br[k] += m * gr;
        }
    }
    str->gl = tgl;
    str->gr = tgr;
    str->last_l = mono[B - 1] * tgl;
    str->last_r = mono[B - 1] * tgr;
    if (bus->chorus && ch->chorus > 0.0f) {
        const float g = ch->gain * ch->chorus;
        for (int k = 0; k < B; ++k) bus->chorus[k] += mono[k] * g;
    }

    // 4. Cull springs that are no longer heard. A driven spring of a held
    // key is never culled (it may be starting from rest), nor is anything
    // while a noise burst is still feeding the springs.
    // A spring is also dropped once it is 50 dB below the string's
    // strongest: the string masks it (this is what keeps the many quiet
    // upper partials of a piano note cheap once the attack has passed).
    float energy = 0.0f, loudest = 0.0f;
    const bool bursting = str->noise_env > 0.0f && str->noise_scale != 0.0f;
    for (uint32_t i = 0; i < str->count; ++i) {
        const SsSpring *p = &str->sp[i];
        loudest = fmaxf(loudest, p->re * p->re + p->im * p->im);
    }
    const float floor2 = fmaxf(SS_SILENCE * SS_SILENCE, 1e-5f * loudest);
    for (uint32_t i = 0; i < str->count;) {
        SsSpring *p = &str->sp[i];
        const float m = p->re * p->re + p->im * p->im;
        if (!bursting && p->drive == 0.0f && m < floor2) {
            *p = str->sp[--str->count];
            continue;
        }
        energy += m;
        ++i;
    }
    str->energy = energy;
    if (str->state == SS_STR_DYING && str->fade == 0) return false; // Faded out
    return str->count > 0 || str->noise_env > 0.0f || str->knock_env > 0.0f;
}

// See ss_synth.h. Clears the buses and divides the strings between the
// parts so each gets about the same number of partials.
void SS_RAM_FUNC(ss_synth_render_begin)(SsSynth *s) {
    memset(s->bus_l, 0, sizeof(s->bus_l));
    memset(s->bus_r, 0, sizeof(s->bus_r));
    memset(s->bus_reverb, 0, sizeof(s->bus_reverb));
    memset(s->bus_chorus, 0, sizeof(s->bus_chorus));

    const bool effects = (SS_ENABLE_REVERB && s->reverb_enabled) || (SS_ENABLE_CHORUS && s->chorus_enabled);
    const uint32_t handicap = effects && SS_RENDER_PARTS > 1 ? SS_RENDER_EFFECTS_COST : 0;
    uint32_t total = handicap;
    for (uint32_t i = 0; i < s->active_count; ++i) total += s->strings[s->active[i]].count + SS_STRING_COST;
    // Walk the strings, starting a new part each time the running total
    // (part 0 starts with the handicap) passes the next share
    uint32_t part = 1, acc = handicap;
    s->part_first[0] = 0;
    for (uint32_t i = 0; i < s->active_count && part < SS_RENDER_PARTS; ++i) {
        while (part < SS_RENDER_PARTS && acc * SS_RENDER_PARTS >= total * part) s->part_first[part++] = i;
        acc += s->strings[s->active[i]].count + SS_STRING_COST;
    }
    while (part <= SS_RENDER_PARTS) s->part_first[part++] = s->active_count;
}

// See ss_synth.h. Only touches the part's strings, buses and finished[]
// flags (part 0 also the effects state).
void SS_RAM_FUNC(ss_synth_render_part)(SsSynth *s, uint32_t part) {
    const uint32_t t0 = SS_CYCLE_COUNT();
    const bool reverb = SS_ENABLE_REVERB && s->reverb_enabled;
    const bool chorus = SS_ENABLE_CHORUS && s->chorus_enabled;
    if (part == 0) {
#if SS_ENABLE_REVERB
        if (reverb) ss_reverb_process(&s->reverb, s->send_reverb, s->bus_l[0], s->bus_r[0], B);
#endif
#if SS_ENABLE_CHORUS
        if (chorus) ss_chorus_process(&s->chorus, s->send_chorus, s->bus_l[0], s->bus_r[0], B);
#endif
    }
    const SsBuses bus = {
        s->bus_l[part], s->bus_r[part],
        reverb ? s->bus_reverb[part] : NULL,
        chorus ? s->bus_chorus[part] : NULL,
    };
    uint32_t partials = 0;
    for (uint32_t i = s->part_first[part]; i < s->part_first[part + 1]; ++i) {
        SsString *str = &s->strings[s->active[i]];
        partials += str->count;
        s->finished[i] = !string_render(str, &s->channels[str->chan], &bus);
    }
    s->stats.partial_blocks[part] += partials;
    s->stats.part_cycles[part] += SS_CYCLE_COUNT() - t0;
}

// Soft clipper: the cubic x - 4x^3/27, which meets +-1 with zero slope at
// |x| = 1.5 and is limited to +-1 beyond. No divide. Close to linear for
// quiet signals (-0.1 dB at 0.25, -0.3 dB at 0.5).
static inline float soft_clip(float x) {
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - 0.14814815f * x * x * x;
}

// See ss_synth.h. Frees finished strings, sums the parts, then declick,
// EQ, width, master gain and the soft clipper.
void SS_RAM_FUNC(ss_synth_render_end_float)(SsSynth *s, float *left, float *right) {
    // Free finished strings, going backwards so the entry swapped into
    // position i has already been checked. Then recount the partials.
    for (uint32_t i = s->active_count; i-- > 0;) {
        if (s->finished[i]) string_free(s, i);
    }
    uint32_t partials = 0;
    for (uint32_t i = 0; i < s->active_count; ++i) {
        const SsString *str = &s->strings[s->active[i]];
        if (str->state != SS_STR_DYING) partials += str->count;
    }
    s->partials = partials;

    for (int k = 0; k < B; ++k) {
        float l = s->bus_l[0][k], r = s->bus_r[0][k];
        float rv = s->bus_reverb[0][k], ch = s->bus_chorus[0][k];
        for (uint32_t p = 1; p < SS_RENDER_PARTS; ++p) {
            l += s->bus_l[p][k];
            r += s->bus_r[p][k];
            rv += s->bus_reverb[p][k];
            ch += s->bus_chorus[p][k];
        }
        s->bus_l[0][k] = l;
        s->bus_r[0][k] = r;
        s->send_reverb[k] = rv;
        s->send_chorus[k] = ch;
    }
    float *bus_l = s->bus_l[0], *bus_r = s->bus_r[0];

    if (s->declick_l != 0.0f || s->declick_r != 0.0f) {
        float dl = s->declick_l, dr = s->declick_r;
        const float d = s->declick_decay;
        for (int k = 0; k < B; ++k) {
            bus_l[k] += dl;
            bus_r[k] += dr;
            dl *= d;
            dr *= d;
        }
        s->declick_l = fabsf(dl) < 1e-7f ? 0.0f : dl;
        s->declick_r = fabsf(dr) < 1e-7f ? 0.0f : dr;
    }

    // DC blocker (10 Hz high pass): plucks always push the same way first,
    // so fast repeated low notes leave a slowly varying offset
    {
        const float c = s->dc_coef;
        float xl = s->dc_x[0], yl = s->dc_y[0], xr = s->dc_x[1], yr = s->dc_y[1];
        for (int k = 0; k < B; ++k) {
            yl = bus_l[k] - xl + c * yl;
            xl = bus_l[k];
            bus_l[k] = yl;
            yr = bus_r[k] - xr + c * yr;
            xr = bus_r[k];
            bus_r[k] = yr;
        }
        s->dc_x[0] = xl; s->dc_y[0] = yl; s->dc_x[1] = xr; s->dc_y[1] = yr;
    }

    ss_eq_process(&s->eq, bus_l, bus_r, B);

    if (s->width_pct < 100) {
        const float w = s->width;
        for (int k = 0; k < B; ++k) {
            const float mid = (bus_l[k] + bus_r[k]) * 0.5f;
            const float side = (bus_l[k] - bus_r[k]) * 0.5f * w;
            bus_l[k] = mid + side;
            bus_r[k] = mid - side;
        }
    }

    const float g = s->gain * s->master_volume;
    float peak = s->stats.peak;
    uint32_t clipped = 0;
    for (int k = 0; k < B; ++k) {
        const float l = bus_l[k] * g, r = bus_r[k] * g;
        const float m = fmaxf(fabsf(l), fabsf(r));
        peak = fmaxf(peak, m);
        clipped += (fabsf(l) > 1.0f) + (fabsf(r) > 1.0f);
        left[k] = soft_clip(l);
        right[k] = soft_clip(r);
    }
    s->stats.peak = peak;
    s->stats.clipped += clipped;
    s->stats.blocks++;
    if (s->active_count > s->stats.peak_strings) s->stats.peak_strings = s->active_count;
    if (s->partials > s->stats.peak_partials) s->stats.peak_partials = s->partials;
    if (ss_synth_cost(s) > s->stats.peak_cost) s->stats.peak_cost = ss_synth_cost(s);
}

// See ss_synth.h. All parts in turn on the calling thread.
void SS_RAM_FUNC(ss_synth_render_float)(SsSynth *s, float *left, float *right) {
    ss_synth_render_begin(s);
    for (uint32_t p = 0; p < SS_RENDER_PARTS; ++p) ss_synth_render_part(s, p);
    ss_synth_render_end_float(s, left, right);
}

// See ss_synth.h.
void SS_RAM_FUNC(ss_synth_render_end)(SsSynth *s, int16_t *out) {
    float left[B], right[B];
    ss_synth_render_end_float(s, left, right);
    for (int k = 0; k < B; ++k) {
        out[2 * k] = (int16_t)(left[k] * 32767.0f);       // Soft clipped: never beyond +-1
        out[2 * k + 1] = (int16_t)(right[k] * 32767.0f);
    }
}

// See ss_synth.h. All parts in turn on the calling thread.
void SS_RAM_FUNC(ss_synth_render)(SsSynth *s, int16_t *out) {
    ss_synth_render_begin(s);
    for (uint32_t p = 0; p < SS_RENDER_PARTS; ++p) ss_synth_render_part(s, p);
    ss_synth_render_end(s, out);
}
