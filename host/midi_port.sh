# shellcheck shell=bash
#
# midi_port.sh - find the synth's MIDI port and send it raw bytes. Sourced
# by the other host/*.sh scripts.
#
#   midi_find_port [PORT]   sets $port, or exits with a message
#   midi_send "HEX BYTES"   sends them to $port
#
# PORT may be
#   hw:2,0,0          a raw MIDI device (amidi -l), e.g. the synth's USB MIDI
#   128:0             an ALSA sequencer port by number or client name
#                     (aseqsend -l)
# Without one: the USB MIDI device named pico-spring-synth.
#
# Raw devices are written with amidi, sequencer ports with aseqsend (both
# alsa-utils; aseqsend needs 1.2.11 or later).

midi_find_port() {
    port=$1
    if [ -z "$port" ]; then
        port=$(amidi -l 2>/dev/null | awk '/pico-spring-synth/ { print $2; exit }')
    fi
    if [ -z "$port" ]; then
        echo "synth not found: no pico-spring-synth USB MIDI device (amidi -l)" >&2
        exit 1
    fi
}

midi_send() {
    case "$port" in
        hw:*) amidi -p "$port" -S "$1" ;;
        *) aseqsend -p "$port" "$1" ;;
    esac
}
