#!/bin/sh
# The direct-to-container encoder (replay-encode --output) must produce exactly
# the same movie as encoding to per-frame payloads and assembling them with
# replay-join. Checks a video-only movie and a movie with audio.
set -eu

encode=$1
join=$2
work=${TMPDIR:-/tmp}/direct-container-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

W=32
H=16
FPS=10
FPC=4

# Deterministic RGB24 frames and signed-16 PCM, no external tools.
python3 - "$work" "$W" "$H" <<'PY'
import sys, struct
work, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
frames = 6
with open(work + "/vid.rgb", "wb") as f:
    for n in range(frames):
        for y in range(h):
            for x in range(w):
                f.write(bytes(((x * 7 + n * 11) & 255,
                               (y * 5 + n * 3) & 255,
                               (x * y + n) & 255)))
with open(work + "/aud.pcm", "wb") as f:
    for i in range(2000):
        f.write(struct.pack("<h", ((i * 137) % 20000) - 10000))
PY

# replay-join uses --sound-pcm; replay-encode --output uses --audio-input, so
# the audio arguments differ between the two tools.
check_video_only()
{
    "$encode" --codec 19 --input "$work/vid.rgb" --size "${W}x${H}" \
        --payload-prefix "$work/f-" --loss-level 4 --pad-to-multiple "$FPC" \
        >/dev/null
    frames=$(find "$work" -name 'f-*.mb19' | wc -l)
    "$join" --codec 19 --size "${W}x${H}" --fps "$FPS" \
        --frames-prefix "$work/f-" --frames "$frames" --frame-suffix .mb19 \
        --frames-per-chunk "$FPC" --pixel-label 6Y5UV --no-poster \
        --output "$work/join.ae7" >/dev/null
    "$encode" --codec 19 --output "$work/direct.ae7" --size "${W}x${H}" \
        --fps "$FPS" --frames-per-chunk "$FPC" --pixel-label 6Y5UV \
        --no-poster --loss-level 4 --pad-to-multiple "$FPC" \
        --input "$work/vid.rgb" >/dev/null
    cmp -s "$work/direct.ae7" "$work/join.ae7" || {
        echo "FAIL (video-only): direct differs from encode+join" >&2; exit 1; }
    echo "ok: video-only"
    rm -f "$work"/f-*.mb19 "$work/join.ae7" "$work/direct.ae7"
}

check_with_audio()
{
    "$encode" --codec 19 --input "$work/vid.rgb" --size "${W}x${H}" \
        --payload-prefix "$work/f-" --loss-level 4 --pad-to-multiple "$FPC" \
        >/dev/null
    frames=$(find "$work" -name 'f-*.mb19' | wc -l)
    "$join" --codec 19 --size "${W}x${H}" --fps "$FPS" \
        --frames-prefix "$work/f-" --frames "$frames" --frame-suffix .mb19 \
        --frames-per-chunk "$FPC" --pixel-label 6Y5UV --no-poster \
        --sound-pcm "$work/aud.pcm" --sound-encode vidc-e8 \
        --sound-rate 8000 --sound-channels 1 \
        --output "$work/join.ae7" >/dev/null
    "$encode" --codec 19 --output "$work/direct.ae7" --size "${W}x${H}" \
        --fps "$FPS" --frames-per-chunk "$FPC" --pixel-label 6Y5UV \
        --no-poster --loss-level 4 --pad-to-multiple "$FPC" \
        --audio-input "$work/aud.pcm" --sound-encode vidc-e8 \
        --sound-rate 8000 --sound-channels 1 \
        --input "$work/vid.rgb" >/dev/null
    cmp -s "$work/direct.ae7" "$work/join.ae7" || {
        echo "FAIL (with-audio): direct differs from encode+join" >&2; exit 1; }
    echo "ok: with-audio"
}

check_video_only
check_with_audio
echo "direct-container: all checks passed"
