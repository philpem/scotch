#!/bin/sh
# A sound-only movie (video format 0) has no video track and no Decomp0
# decompressor. replay-transcode must produce the sound track and must NOT try
# to load an external module for codec 0 even when --modules-dir is supplied --
# the regression that made real sound-only movies (CineClips, !ARPlayer's
# DUMMY) fail with "cannot open .../Decomp0/Decompress,ffd" and exit 1.
set -eu

maker=$1
transcode=$2

work=${TMPDIR:-/tmp}/transcode-sound-only-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

"$maker" "$work/m.ae7" "$work/expected.pcm"

# A directory that has no Decomp0 (any path works): the tool must not look there.
modules="$work/modules"
mkdir -p "$modules"

# 1) With audio + --modules-dir: must write the WAV and exit 0, not chase Decomp0.
out=$("$transcode" --input "$work/m.ae7" --output /dev/null \
        --audio-output "$work/out.wav" --modules-dir "$modules" 2>&1)
echo "$out"
if echo "$out" | grep -q "Decomp0"; then
    echo "FAIL transcoder tried to load a Decomp0 module for codec 0" >&2
    exit 1
fi

# signed-16 decode is the identity: the WAV payload must equal the input track.
dd if="$work/out.wav" bs=1 skip=44 of="$work/out.pcm" 2>/dev/null
if cmp -s "$work/expected.pcm" "$work/out.pcm"; then
    echo "ok sound-only audio matches expected"
else
    echo "FAIL sound-only audio PCM differs" >&2
    exit 1
fi

# 2) No audio requested (the DUMMY placeholder case): clean exit 0, no Decomp0.
out=$("$transcode" --input "$work/m.ae7" --output /dev/null \
        --modules-dir "$modules" 2>&1)
echo "$out"
if echo "$out" | grep -q "Decomp0"; then
    echo "FAIL transcoder tried to load a Decomp0 module with no audio" >&2
    exit 1
fi
echo "ok sound-only with no audio exits cleanly"
