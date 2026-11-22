#!/bin/sh
# Render the two original MIDI tunes to mono WAVs for the N64 build.
#
# The N64 port has no MIDI synth, so the tunes are pre-rendered on the host
# and committed as music/theme.wav and music/danube.wav; Makefile.n64 then
# converts them to VADPCM .wav64 files. Re-run this only if you want to
# change the soundfont, sample rate or level.
#
# Requires: fluidsynth, sox, and a General MIDI soundfont
#   apt-get install fluidsynth fluid-soundfont-gm sox
#
# Usage: tools/render_music.sh [soundfont.sf2]
set -e

cd "$(dirname "$0")/.."

SF2="${1:-/usr/share/sounds/sf2/FluidR3_GM.sf2}"
RATE=22050
# Peak level in dBFS. Kept below full scale so the music sits under the
# sound effects (the player applies a further channel volume).
PEAK_DB=-3

if [ ! -f "$SF2" ]; then
	echo "soundfont not found: $SF2" >&2
	exit 1
fi

mkdir -p music
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for tune in theme danube; do
	echo "rendering data/$tune.mid"
	# Render in stereo at 44.1 kHz with reverb/chorus off (they smear
	# badly after ADPCM compression and a mono downmix).
	fluidsynth -ni -q -g 0.6 -R 0 -C 0 -r 44100 \
		-F "$TMP/$tune.wav" -O s16 -T wav "$SF2" "data/$tune.mid"
	# Downmix, resample, trim leading/trailing silence, normalise.
	sox "$TMP/$tune.wav" -b 16 -c 1 -r $RATE "music/$tune.wav" \
		silence 1 0.01 0.1% reverse silence 1 0.01 0.1% reverse \
		gain -n $PEAK_DB
	sox --i "music/$tune.wav" | grep -E 'Duration|Sample Rate|Channels'
done
