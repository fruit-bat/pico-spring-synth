# pico-spring-synth

[![Build](https://github.com/fruit-bat/pico-spring-synth/actions/workflows/build.yml/badge.svg)](https://github.com/fruit-bat/pico-spring-synth/actions/workflows/build.yml)

A General MIDI synthesizer for the RP2350 that makes every sound from banks of
simulated damped springs. It grew from
[java-spring-synth](https://github.com/fruit-bat/java-spring-synth), with the
firmware plumbing (audio out, MIDI in, settings, effects) from pico-fsynth.
It needs no sample data or SoundFont.

## Layout

- `core/` portable synth: springs and strings, patches, effects, MIDI file player
- `firmware/` RP2350 specific: audio output (I2S / PWM), MIDI input (UART / USB), main
- `host/` PC tools: MIDI to WAV renderer, patch auditions, MIDI control scripts
- `tests/` automated tests of the core and the firmware's MIDI parsers

## How it works

### Springs

Each partial is a damped spring. The Java version stepped every spring with
Euler integration at 88.2 kHz. That is only stable while
omega x dt < 2, and its pitch drifts as omega x dt grows. Here a spring's state
is a complex number z = x + i y (displacement and scaled velocity, so |z| is
the amplitude). Each sample z is multiplied by r e^(i omega T), a rotation
that also decays: 4 multiplies and 2 adds. It is exact, unconditionally stable
and in tune at 44.1 kHz (the tests check A4 to 20 ppm). Up to four springs
share each pass of the inner loop. Measured on the RP2350, a spring costs
about 10 cycles per sample.

A spring can be excited in three ways (see `core/ss_patch.h`):

- **pluck**: a velocity impulse at note on (hammer, plectrum, mallet). The spring
  starts at rest and swings out, so a note starts without a click.
- **drive**: the Java synth's trick. While the key is held, the spring is pushed a
  little in the direction it is moving (negative friction, like a bow or a
  reed). The push is constant, so the spring settles where push and damping
  balance. Driven springs start by themselves and swell with a time constant
  set by their decay time (organs, strings, brass, pads). The push's effect
  over a block has a closed form, so it is applied once per block, as the
  rotation's magnitude.
- **noise**: a short burst into the springs, and optionally straight to the output
  (hammer thump, snare wires, breath).

### Strings and allocation

A **string** is what one key of one channel sounds: up to 28 springs. Strings
come from a pool, found by channel and key:

- **Striking a key that is still ringing** (even after note off) excites the same
  springs again instead of starting another voice, as on a real string. A
  hammer or plectrum absorbs part of the motion it meets (`restrike_damp`), so
  fast repeated notes do not pile up. Bells and bars keep it all.
- **No envelopes**: a spring is dropped once its amplitude falls below -90 dB, and a
  string is freed when it has none left. Upper partials die first, so a
  decaying string gets cheaper as it fades.
- **Limits**: a cap on strings, and a CPU budget counted in partials, with each
  string counting as 3 (its panning and mixing cost about as much as three
  springs). A note that would exceed either steals the string with the least
  energy. The energy is known exactly from the spring states, and released
  strings count 4x less. A stolen string fades out over 3 ms, at most 4 at a
  time, so steals do not click. Beyond that, in a flood of notes, the string
  that has faded furthest is cut, with a declick.

Dense orchestral MIDI files reach the default budget at 150 MHz only at their
peaks. Re-striking typically handles a third to two thirds of
all notes.

### Instruments

One spring patch per GM family, with some families split (33 patches,
`ssrender -l` lists them), and a spring drum kit on channel 10 (18 patches:
kick, snare, toms, hats with choke, cymbals, hand percussion...). Patch levels
are calibrated against FluidSynth with a GM SoundFont, so GM files keep their
balance between instruments.

The piano is four patches blended per key, so there is no seam: up to F3 a
fitted patch (16 consecutive partials from the hammer's strike position,
unison pairs that beat and give the two-stage decay, a velocity-dependent
hammer filter, a soundboard filter that weakens the bass fundamental, decay
set by each partial's frequency and an unpitched knock for the case); from
F4 to C5 the original 8-partial piano; from G5 to C6 the same harmonics with a
two-stage decay (most of the sound dies within 0.3 s, leaving a quiet
tail); and in the top octave a nearly pure fundamental with a hammer low
pass and knock, decaying the same way. Like a real piano it is tuned stretched
towards the top (+13 cents at C7, +30 at C8, as measured on FluidR3's
grand), with string stiffness to match, so octaves stay beat free.

The harpsichord is plucked near the end of the string, so it stays bright
with about equal energy per octave up to 8 kHz and beyond. Its 24 springs
are harmonics 1-8 and then sparser ones up to the 128th (8 kHz at C2), each
standing for its neighbours. Those too high to sound are dropped, so only
the bass pays for them. A rasp (the string's output through a gentle cubic curve,
scaled to the note so it fades with it) fills in the harmonics between them.
Products of harmonics are harmonics, so it adds nothing out of tune. As with
a real quill, the string sounds 5 ms after the key, with a quiet bright
scrape before it.

Partials more than 50 dB below a string's strongest are dropped, so the
upper ones cost little once the attack has passed.

Bank select MSB 1 selects **classic**, a replica of the Java `ShoSynQ`: springs at
1, 1.998 and 4.001 x the note, the same pluck ratios, decay and drive. Driven
side by side with the original Java code, its held envelope matches to 0.2 dB
and its partial levels to 2 dB. The Java played key 60 at 440 Hz; here keys
are in tune (69 = 440 Hz).

The MIDI implementation covers note on/off, velocity, program change, bank
select (1 = classic, 120/127 = drums), volume, expression, pan, modulation
(vibrato), sustain, reverb and chorus sends, pitch bend with RPN 0 range,
channel pressure (scales the drive, like bow pressure), all notes / sound off,
reset controllers, and GM / GM2 / GS reset.

## Rendering on the PC

```sh
cmake -S host -B build-host && cmake --build build-host
./build-host/ssrender song.mid out.wav              # -d dry, -t two threads, -g gain
./build-host/ssrender -A 0 piano.wav                # audition GM program 0
./build-host/ssrender -A drums drums.wav            # audition the drum kit
./build-host/ssrender -c song.mid classic.wav       # everything on the classic patch
./build-host/ssrender -p 19 song.mid organ.wav      # everything on GM program 19
./build-host/ssrender -A 40 -M strings.mid s.wav    # also write the audition as MIDI
./build-host/ssrender -l                            # list patches and the GM map
```

`ssrender` runs the same core code as the firmware. To tune a patch, edit
`core/ss_patch.c` and audition it. `-M` writes the audition as a MIDI file, so
you can compare it with another synth, e.g.
`fluidsynth -n -i -F ref.wav some.sf2 strings.mid`.

## Tests

```sh
cmake -S host -B build-host && cmake --build build-host
ctest --test-dir build-host --output-on-failure

# The same under AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S host -B build-host-asan -DSS_SANITIZE=ON && cmake --build build-host-asan
ctest --test-dir build-host-asan --output-on-failure
```

| Test | Covers |
|------|--------|
| `synth` | tuning, decay times, drive amplitude, re-strike, self freeing, stealing, cost budget, sustain, bend, drums and chokes, sound off, split render, every patch finite and audible |
| `smf` | MIDI file player: timing, tempo, running status, SysEx, truncated files |
| `midi_parse` | firmware UART / USB MIDI parsers and SysEx recognition |

## Firmware

```sh
cmake -S . -B build -DSS_AUDIO_OUT=I2S        # or PWM
cmake --build build
picotool load -x build/pico_spring_synth.uf2
```

Once this firmware is running, opening its serial console at 1200 baud
reboots it into BOOTSEL, so later updates need no button press. For example:
`python3 -c "import serial; serial.Serial('/dev/ttyACM0', 1200).close()"`,
then copy the UF2 to the RP2350 drive.

Options (all `-D...`): `SS_AUDIO_OUT`, `SS_SAMPLE_RATE` (44100),
`SS_SYS_CLOCK_KHZ` (150000, the RP2350's rated clock; above it the core voltage
is raised to 1.2 V), `SS_COST_BUDGET` and `SS_MAX_STRINGS` (by default 280
and 48 per 150 MHz), `SS_DUAL_CORE` (ON), pin numbers below,
`SS_DEMO_MIDI=/path/song.mid` to embed a song that plays at power up until
MIDI input arrives, and `SS_BENCH=ON` to print render benchmarks at boot.

### Ready-made firmware

GitHub Actions (`.github/workflows/build.yml`) runs the host tests (plain and
with the sanitizers) and builds a UF2 for each audio variant: `pico-audio`,
`pico-audio-v21`, `i2s` and `pwm`, for the default board and pins. Each push
and pull request leaves them as run artifacts. A `v*` tag also attaches them
to a GitHub release. **Run workflow** on the Actions tab (also on a fork)
builds for another `PICO_BOARD` or with extra cmake options, such as
`-DSS_SYS_CLOCK_KHZ=250000`. The licences of the code linked into the
firmware come with it, in `licenses/`.

### Performance

Measured on an RP2350 at 150 MHz with `SS_BENCH`. A core has 3401 cycles per
sample at 44.1 kHz.

| Cost | Cycles per sample |
|---|---|
| a partial (spring) | 10 |
| a driven partial, extra | 2 |
| a string (pan, mix, cull) | 35 |
| vibrato, extra per string | 18 |
| reverb + chorus | 310 |
| final mix, DC blocker, soft clip | 130 |
| a note on (8 partial piano) | 5400 per note |

The default budget keeps the worst block of a dense song below about 75% of
each core. That leaves room for the effects and for bursts of MIDI (at most
15% of a block). On a Waveshare Pico-Audio, Queen of Sheba (orchestral,
peaks at the budget) and a dense piano piece play with no underruns or
dropped MIDI. So does a deliberate overload: 60 sustained organ notes at
once (2.4x the budget), then a sustained 88-note glissando.

The console prints the CPU load per core, the peak strings, partials and
cost, underruns, and the worst block broken down into MIDI, core 1's render
part, waiting for core 0 and the final mix. At 250 MHz
(`-DSS_SYS_CLOCK_KHZ=250000`) the budget scales to 466.

### Connections (defaults)

| Function | GPIO |
|---|---|
| I2S data (DIN) | 12 |
| I2S BCLK | 13 |
| I2S LRCLK (WS) | 14 |
| PWM left / right | 20 / 21 (RC low pass on each, e.g. 1k + 10 nF) |
| MIDI in (UART1 RX, via opto-isolator) | 5 |

`-DSS_PICO_AUDIO=ON` selects the pins of a stacked Waveshare Pico-Audio
(PCM5101A: DIN 26, BCK 27, LRCK 28). `-DSS_PICO_AUDIO_V21=ON` selects the
Rev2.1 board (CS4344: DIN 22, MCLK 26, LRCK 27, SCLK 28, with MCLK).
`-DSS_I2S_MCLK_PIN=n` turns MCLK on for other wiring. With MCLK on,
`SS_I2S_CLOCK_PIN_BASE` is the LRCLK pin and BCLK is the next GPIO.

USB shows up as a serial console plus a USB MIDI device. The console prints a
status line every second: CPU load, peak strings and partials, audio
underruns, MIDI counts, output peak and clipping, master volume, EQ and width.

### Device settings

The same SysEx as pico-fsynth, with the ID `7D 53 53` ("SS"):

- **Master volume**: GM Master Volume (`F0 7F 7F 04 01 lsb msb F7`). From the PC:
  `host/master_volume.sh 50`.
- **Master EQ**: the Roland GS master EQ (address `40 02 00..03`). From the PC:
  `host/master_eq.sh BASS_DB TREBLE_DB [200|400 3000|6000]`.
- **Stereo width**: `F0 7D 53 53 03 ww F7`. From the PC: `host/stereo_width.sh PERCENT`.
- **Save and erase**: `host/settings.sh save` (`F0 7D 53 53 01 F7`) stores volume,
  EQ and width in the last flash sector. `host/settings.sh erase`
  (`... 02 F7`) goes back to the defaults.
- **Reset**: `host/reset.sh [-g]` silences everything and resets the controllers
  (`-g` also sends GM System On).

## Credits

pico-spring-synth is MIT licensed (`LICENSE`). It builds on, or learned from,
the work of others:

- **java-spring-synth** (https://github.com/fruit-bat/java-spring-synth), by
  the same author. The spring synthesis idea, the drive and the patch that
  bank 1 ("classic") replicates.
- **pico-fsynth**, by the same author (MIT). The firmware plumbing: audio
  output, MIDI input, settings, effects and the device SysEx.
- **Freeverb** by Jezar at Dreampoint (public domain). The reverb's
  structure, delay lengths and scale factors (`core/ss_effects.c`).
- **Audio EQ Cookbook** by Robert Bristow-Johnson. The master EQ shelf
  formulas.
- **FluidSynth** (https://www.fluidsynth.org/, LGPL-2.1) and the
  **FluidR3_GM** SoundFont by Frank Wen (MIT). The reference renders that
  patch levels, effect levels and the piano's stretched tuning were matched
  against. No FluidSynth code or SoundFont data is used.
- **Raspberry Pi Pico SDK** and **picotool** (BSD-3-Clause,
  https://github.com/raspberrypi/pico-sdk), including **TinyUSB** by Ha Thach
  (MIT, https://github.com/hathach/tinyusb) for USB MIDI.
  `pico_sdk_import.cmake` is copied from the Pico SDK under its licence.
- **ALSA** and alsa-utils (`amidi`, `aplaymidi`, `aseqsend`) for the PC scripts.
- Waveshare for the RP2350-Plus and Pico-Audio boards and their
  documentation.
