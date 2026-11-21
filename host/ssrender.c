//
// ssrender.c - render a MIDI file (or a built-in audition of one patch) to
// a WAV file with the spring synth core.
//
// It runs exactly the same core code as the firmware, so a render here
// sounds like the Pico (results can differ in the last float bits between
// compilers and FPUs). Use it to hear and tune patches without flashing.
//
// The song is played block by block (SS_BLOCK_SIZE frames), dispatching
// the MIDI events due in each block before rendering it. After the song
// ends, rendering continues until every string has finished (at least 1 s,
// for the reverb tail) or 8 s have passed.
//
// Output is 16-bit stereo PCM.
//
// Usage: ssrender [options] song.mid out.wav
//        ssrender [options] -A program out.wav     audition a GM program
//        ssrender [options] -A drums out.wav       audition the drum kit
//        ssrender -l                               list the patches
//   -M file.mid                     also write the audition as a MIDI file
//                                   (to render it with another synth)
//   -r rate                         sample rate, 8000..SS_MAX_SAMPLE_RATE (44100)
//   -g gain                         master gain (default SS_DEFAULT_GAIN)
//   -d                              dry: reverb and chorus off
//   -t                              render on two threads, as the Pico's two
//                                   cores do (checks the split is safe)
//   -c                              classic: every melodic channel plays the
//                                   original Java spring synth patch
//   -p program                      every melodic channel plays this GM program
//   -R room,damp,width,level        reverb settings, each 0..1
//   -C rate_hz,depth_ms,level       chorus settings
//   -E bass_db,treble_db[,bass_hz,treble_hz]
//                                   master EQ (default flat, 200 / 3000 Hz)
//   -W percent                      stereo width, 0 (mono) .. 100 (default)
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

#include "ss_synth.h"
#include "ss_smf.h"

// Load a whole file into a heap buffer; its size is returned in *size.
static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    rewind(f);
    void *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
    fclose(f);
    *size = (size_t)sz;
    return buf;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

// 44-byte WAV header for 16-bit stereo PCM
static void write_wav_header(FILE *f, uint32_t rate, uint32_t frames) {
    uint8_t h[44];
    const uint32_t data = frames * 4;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, rate);
    put32(h + 28, rate * 4); put16(h + 32, 4); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    fwrite(h, 1, sizeof(h), f);
}

// Threaded rendering (-t): a worker renders parts 1.. of every block
static sem_t work_go, work_done;
static volatile bool work_quit;

static void *worker(void *arg) {
    SsSynth *synth = (SsSynth *)arg;
    for (;;) {
        sem_wait(&work_go);
        if (work_quit) return NULL;
        for (uint32_t p = 1; p < SS_RENDER_PARTS; ++p) ss_synth_render_part(synth, p);
        sem_post(&work_done);
    }
}

static int force_program = -1;

// MIDI file player callback: pass each event to the synth (ctx), with the
// program replaced if -p was given
static void on_midi(void *ctx, const uint8_t *msg, uint32_t len) {
    SsSynth *s = (SsSynth *)ctx;
    if (force_program >= 0 && (msg[0] & 0xf0) == 0xc0 && len == 2) {
        const uint8_t m[2] = { msg[0], (uint8_t)force_program };
        ss_synth_midi(s, m, 2);
        return;
    }
    ss_synth_midi(s, msg, len);
}

// ---------------------------------------------------------------------------
// Auditions: a short built-in sequence instead of a MIDI file
// ---------------------------------------------------------------------------

typedef struct {
    double t;           // Seconds
    uint8_t msg[3];
} Event;

#define MAX_EVENTS 1024
static Event events[MAX_EVENTS];
static uint32_t event_count;

static void ev(double t, uint8_t a, uint8_t b, uint8_t c) {
    if (event_count < MAX_EVENTS) events[event_count++] = (Event){ t, { a, b, c } };
}

static void note(double t, double len, uint8_t ch, uint8_t key, uint8_t vel) {
    ev(t, (uint8_t)(0x90 | ch), key, vel);
    ev(t + len, (uint8_t)(0x80 | ch), key, 64);
}

static int ev_cmp(const void *a, const void *b) {
    const double d = ((const Event *)a)->t - ((const Event *)b)->t;
    return d < 0 ? -1 : d > 0;
}

// Melodic audition: rising arpeggio over four octaves at rising velocity,
// a held chord, a soft-to-loud repeated note (re-striking one string), a
// pitch bend, and a low and a high note left to ring.
static void audition_program(uint8_t program) {
    ev(0, 0xc0, program, 0);
    static const uint8_t arp[] = { 0, 4, 7, 12 };
    double t = 0.1;
    for (int o = 0; o < 4; ++o) {
        for (int i = 0; i < 4; ++i) {
            const uint8_t key = (uint8_t)(36 + 12 * o + arp[i]);
            note(t, 0.22, 0, key, (uint8_t)(40 + o * 25));
            t += 0.25;
        }
    }
    t += 0.5;
    static const uint8_t chord[] = { 48, 55, 60, 64, 67 };
    for (unsigned i = 0; i < sizeof(chord); ++i) note(t, 2.0, 0, chord[i], 90);
    t += 3.0;
    for (int i = 0; i < 8; ++i) {
        note(t, 0.1, 0, 60, (uint8_t)(30 + i * 13));
        t += 0.15;
    }
    t += 0.6;
    note(t, 1.6, 0, 67, 100);
    for (int i = 0; i <= 16; ++i) {
        const uint16_t bend = (uint16_t)(8192 + (i <= 8 ? i : 16 - i) * 1023);
        ev(t + 0.4 + i * 0.05, 0xe0, bend & 0x7f, (uint8_t)(bend >> 7));
    }
    t += 2.2;
    note(t, 0.3, 0, 28, 110);
    note(t + 0.4, 0.3, 0, 96, 110);
}

// Drum audition: two bars of a rock beat with fills, then every mapped key
static void audition_drums(void) {
    const double beat = 0.5, e = beat / 2;
    double t = 0.1;
    for (int bar = 0; bar < 2; ++bar) {
        for (int i = 0; i < 8; ++i) {
            note(t + i * e, 0.05, 9, i == 7 && bar == 1 ? 46 : 42, (uint8_t)(i & 1 ? 70 : 100));
            if (i == 0 || i == 3 || i == 4) note(t + i * e, 0.05, 9, 36, 110);
            if (i == 2 || i == 6) note(t + i * e, 0.05, 9, 38, 105);
        }
        t += 8 * e;
    }
    static const uint8_t fill[] = { 50, 48, 47, 45, 43, 41 };
    for (unsigned i = 0; i < sizeof(fill); ++i) note(t + i * e / 2, 0.05, 9, fill[i], 100);
    t += 3 * e;
    note(t, 0.05, 9, 49, 110);
    note(t, 0.05, 9, 36, 110);
    t += 2.0;
    for (uint8_t k = 27; k <= 87; ++k) {
        if (!ss_drum_map[k].patch) continue;
        note(t, 0.05, 9, k, 100);
        t += 0.35;
    }
}

// Write the audition events as a format 0 MIDI file (480 ticks per
// quarter at 120 bpm: 960 ticks per second)
static void write_midi(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", path); exit(1); }
    static uint8_t trk[MAX_EVENTS * 8 + 16];
    uint32_t n = 0, last = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        const uint32_t tick = (uint32_t)(events[i].t * 960.0 + 0.5);
        uint32_t d = tick - last, v = d & 0x7f;
        last = tick;
        while (d >>= 7) v = (v << 8) | 0x80 | (d & 0x7f);
        for (;;) { trk[n++] = (uint8_t)v; if (v & 0x80) v >>= 8; else break; }
        const uint32_t len = (events[i].msg[0] & 0xf0) == 0xc0 ? 2 : 3;
        memcpy(trk + n, events[i].msg, len);
        n += len;
    }
    static const uint8_t eot[] = { 0, 0xff, 0x2f, 0 };
    memcpy(trk + n, eot, sizeof(eot));
    n += sizeof(eot);
    const uint8_t hdr[] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0,
                            'M', 'T', 'r', 'k', (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    fwrite(hdr, 1, sizeof(hdr), f);
    fwrite(trk, 1, n, f);
    fclose(f);
}

// List the patches and the program map
static void list_patches(void) {
    printf("melodic patches (GM programs):\n");
    for (uint32_t i = 0; i < ss_patch_count; ++i) {
        printf("  %-18s %u partials:", ss_patches[i].name, ss_patches[i].count);
        int n = 0;
        for (int p = 0; p < 128; ++p) {
            if (ss_gm_patch[p] == i) printf("%s%d", n++ ? "," : " ", p);
        }
        if ((uint8_t)i == ss_classic_patch) printf(" (bank 1, any program)");
        printf("\n");
    }
    printf("drum patches (keys on channel 10):\n");
    for (uint32_t i = 0; i < ss_drum_patch_count; ++i) {
        printf("  %-18s %u partials:", ss_drum_patches[i].name, ss_drum_patches[i].count);
        int n = 0;
        for (int k = 0; k < 128; ++k) {
            if (ss_drum_map[k].patch == i + 1) printf("%s%d", n++ ? "," : " ", k);
        }
        printf("\n");
    }
}

int main(int argc, char **argv) {
    uint32_t rate = 44100;
    float gain = SS_DEFAULT_GAIN;
    bool dry = false, threaded = false, classic = false;
    const char *audition = NULL, *midi_out = NULL;
    float rv[4] = { SS_REVERB_DEFAULT_ROOM, SS_REVERB_DEFAULT_DAMP, SS_REVERB_DEFAULT_WIDTH, SS_REVERB_DEFAULT_LEVEL };
    float chs[3] = { SS_CHORUS_DEFAULT_RATE, SS_CHORUS_DEFAULT_DEPTH, SS_CHORUS_DEFAULT_LEVEL };
    float eq[4] = { 200, 0, 3000, 0 };
    int width = 100;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "-r") && argi + 1 < argc) rate = (uint32_t)atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-g") && argi + 1 < argc) gain = (float)atof(argv[++argi]);
        else if (!strcmp(argv[argi], "-d")) dry = true;
        else if (!strcmp(argv[argi], "-t")) threaded = true;
        else if (!strcmp(argv[argi], "-c")) classic = true;
        else if (!strcmp(argv[argi], "-l")) { list_patches(); return 0; }
        else if (!strcmp(argv[argi], "-p") && argi + 1 < argc) force_program = atoi(argv[++argi]) & 127;
        else if (!strcmp(argv[argi], "-A") && argi + 1 < argc) audition = argv[++argi];
        else if (!strcmp(argv[argi], "-M") && argi + 1 < argc) midi_out = argv[++argi];
        else if (!strcmp(argv[argi], "-R") && argi + 1 < argc) sscanf(argv[++argi], "%f,%f,%f,%f", &rv[0], &rv[1], &rv[2], &rv[3]);
        else if (!strcmp(argv[argi], "-C") && argi + 1 < argc) sscanf(argv[++argi], "%f,%f,%f", &chs[0], &chs[1], &chs[2]);
        else if (!strcmp(argv[argi], "-E") && argi + 1 < argc) sscanf(argv[++argi], "%f,%f,%f,%f", &eq[1], &eq[3], &eq[0], &eq[2]);
        else if (!strcmp(argv[argi], "-W") && argi + 1 < argc) width = atoi(argv[++argi]);
        else break;
        ++argi;
    }
    const int files = audition ? 1 : 2;
    if (argc - argi != files || width < 0 || width > 100 || rate < 8000 || rate > SS_MAX_SAMPLE_RATE) {
        fprintf(stderr, "usage: ssrender [options] song.mid out.wav\n"
                "       ssrender [options] -A program|drums [-M audition.mid] out.wav\n"
                "       ssrender -l\n"
                "options: [-r rate (8000..%d)] [-g gain] [-d (dry)] [-t (two threads)]\n"
                "         [-c (classic patch)] [-p program]\n"
                "         [-R room,damp,width,level] [-C rate_hz,depth_ms,level]\n"
                "         [-E bass_db,treble_db[,bass_hz,treble_hz]] [-W width_percent]\n", SS_MAX_SAMPLE_RATE);
        return 2;
    }

    static SsSmf smf;
    void *midi_data = NULL;
    if (audition) {
        if (!strcmp(audition, "drums")) audition_drums();
        else audition_program((uint8_t)(atoi(audition) & 127));
        qsort(events, event_count, sizeof(Event), ev_cmp);
        if (midi_out) write_midi(midi_out);
    } else {
        size_t midi_size;
        midi_data = read_file(argv[argi], &midi_size);
        if (!ss_smf_init(&smf, midi_data, midi_size, rate)) { fprintf(stderr, "%s: not a supported MIDI file\n", argv[argi]); return 1; }
    }
    const char *out_path = argv[argi + files - 1];

    static SsSynth synth; // Large: keep it off the stack
    ss_synth_init(&synth, rate);
    ss_synth_set_gain(&synth, gain);
    ss_synth_set_effects(&synth, !dry, !dry);
    ss_synth_set_reverb(&synth, rv[0], rv[1], rv[2], rv[3]);
    ss_synth_set_chorus(&synth, chs[0], chs[1], chs[2]);
    ss_synth_set_eq(&synth, eq[0], eq[1], eq[2], eq[3]);
    ss_synth_set_width(&synth, (uint8_t)width);
    if (classic) synth.force_bank = 1;
    if (force_program >= 0) {
        for (uint8_t c = 0; c < 16; ++c) ss_synth_program_change(&synth, c, (uint8_t)force_program);
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "cannot create %s\n", out_path); return 1; }
    write_wav_header(out, rate, 0);

    int16_t block[SS_BLOCK_SIZE * 2];
    uint32_t frames = 0, tail_blocks = 0, next_event = 0;
    const uint32_t max_tail = 8 * rate / SS_BLOCK_SIZE;
    const clock_t t0 = clock();
    pthread_t thread;
    if (threaded) {
        sem_init(&work_go, 0, 0);
        sem_init(&work_done, 0, 0);
        pthread_create(&thread, NULL, worker, &synth);
    }

    for (;;) {
        bool playing;
        if (audition) {
            const double now = (double)(frames + SS_BLOCK_SIZE) / rate;
            while (next_event < event_count && events[next_event].t < now) {
                const Event *e = &events[next_event++];
                on_midi(&synth, e->msg, (e->msg[0] & 0xf0) == 0xc0 ? 2 : 3);
            }
            playing = next_event < event_count;
        } else {
            playing = ss_smf_advance(&smf, SS_BLOCK_SIZE, on_midi, &synth);
        }
        if (threaded) {
            ss_synth_render_begin(&synth);
            sem_post(&work_go);
            ss_synth_render_part(&synth, 0);
            sem_wait(&work_done);
            ss_synth_render_end(&synth, block);
        } else {
            ss_synth_render(&synth, block);
        }
        fwrite(block, sizeof(int16_t), SS_BLOCK_SIZE * 2, out);
        frames += SS_BLOCK_SIZE;
        if (!playing) {
            if (ss_synth_active_strings(&synth) == 0 && tail_blocks > rate / SS_BLOCK_SIZE) break;
            if (++tail_blocks > max_tail) break;
        }
    }

    if (threaded) {
        work_quit = true;
        sem_post(&work_go);
        pthread_join(thread, NULL);
    }
    const double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fseek(out, 0, SEEK_SET);
    write_wav_header(out, rate, frames);
    fclose(out);

    const SsSynthStats *st = &synth.stats;
    const double secs = (double)frames / rate;
    uint64_t pb = 0;
    for (int p = 0; p < SS_RENDER_PARTS; ++p) pb += st->partial_blocks[p];
    printf("%s: %.1f s audio, %.2f s CPU (%.0fx realtime)\n", out_path, secs, cpu, cpu > 0 ? secs / cpu : 0.0);
    printf("  notes %u: new strings %u, re-struck %u; peak strings %u, partials %u, cost %u/%u; mean partials %.1f\n",
           st->notes, st->strings_started, st->restrikes, st->peak_strings, st->peak_partials, st->peak_cost, SS_COST_BUDGET,
           st->blocks ? (double)pb / st->blocks : 0.0);
    printf("  stolen %u (cut %u), peak %.2f, clipped samples %u\n", st->steals, st->cuts, (double)st->peak, st->clipped);
    free(midi_data);
    return 0;
}
