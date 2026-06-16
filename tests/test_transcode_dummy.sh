#!/bin/sh
# Regression for the real !ARPlayer DUMMY placeholder (corpus/container/dummy.ae7):
# a non-conforming sound-only movie (video format 0, but 120x96 dimensions, no
# sound). replay-transcode must recognise format 0 as "no video", not chase a
# non-existent Decomp0 module even with --modules-dir set, and exit cleanly.
set -eu

transcode=$1
movie=$2

work=${TMPDIR:-/tmp}/transcode-dummy-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

# A directory with no Decomp0: the tool must not look there for codec 0.
modules="$work/modules"
mkdir -p "$modules"

# Worker-style invocation: request audio and pass a modules dir. The movie has
# no video and no sound, so the run must succeed (exit 0) with no output and no
# Decomp0 lookup, rather than dying on a missing decompressor.
out=$("$transcode" --input "$movie" --output /dev/null \
        --audio-output "$work/out.wav" --modules-dir "$modules" 2>&1)
status=$?
echo "$out"

if [ "$status" -ne 0 ]; then
    echo "FAIL transcoder exited $status on the DUMMY placeholder" >&2
    exit 1
fi
if echo "$out" | grep -q "Decomp0"; then
    echo "FAIL transcoder tried to load a Decomp0 module for codec 0" >&2
    exit 1
fi
if [ -e "$work/out.wav" ]; then
    echo "FAIL wrote a WAV for a movie with no sound track" >&2
    exit 1
fi

echo "ok DUMMY placeholder transcodes cleanly (no video, no sound, no Decomp0)"
