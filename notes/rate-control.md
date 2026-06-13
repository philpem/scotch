# Rate Control Modes

The encoder offers the same three rate-control modes as Acorn's !AREncode. They
choose how the type 19 encoder trades quality against size; you pick exactly one.

## 1. Quality factor (fixed quality, variable size)

Encode every frame at a fixed quality level and let the size fall where it may.

- `replay-encode --loss-level N` (0..28), or `replay-make --loss-level N`.
- Level 0 requires exact decoder-visible matches; higher levels accept more
  approximate copy/data blocks (source-derived per-level thresholds). Lower =
  cleaner and larger, higher = grittier and smaller.
- No size target: simplest mode, predictable quality, unpredictable bitrate.

## 2. Frame size (fixed size, variable quality)

Aim each frame at a byte target and vary the quality to hit it.

- `replay-encode --target-bytes N` (with `--loss-level N` as the starting
  quality). After the first frame the encoder retries the frame, moving one
  quality level per retry, until the size lands in an acceptance window
  (90% .. 102.5% of the target; types 17/19/20 widen the upper side by ~target/20).
- Unused bytes from earlier frames in a chunk are available to later frames, so
  the encoder aims at a chunk budget rather than forcing every frame identical.
- "Faster matching" in AREncode is a reduced motion search (not yet a separate
  option here); "Limit to ARM2" caps the per-frame size (see `--arm2-max`).

## 3. Device bandwidth (target a playback data rate)

Pick a sustained data rate and IO latency for the playback device; the tool
computes the per-frame byte budget (mode 2) that fits, accounting for sound and
double buffering. This is Decomp19's BatchComp formula:

```
chunktime   = frames_per_chunk / fps                 (seconds per chunk)
sound/chunk = round_up(sound_bytes_per_second * chunktime)
data/chunk  = floor_to_1024( (chunktime*buffers - latency)/buffers
                             * datarate*1024  -  sound/chunk )
bytes/frame = data/chunk / frames_per_chunk          (-> --target-bytes)
```

- `replay-make --data-rate KB [--latency S] [--double] [--arm2-max N]`.
- `--data-rate` is the device's sustained rate in kB/s (AREncode default 150).
- `--latency` is the IO latency allowance in seconds (default 0.4).
- `--double` tells it the player double-buffers chunks (`buffers` = 2), which
  earns more headroom; default is single (`buffers` = 1).
- `--arm2-max N` caps the per-frame budget for slow CPUs ("Limit to ARM2").
- `sound_bytes_per_second` follows the chosen audio format: 8-bit VIDC/signed =
  rate*channels, 16-bit = rate*channels*2, ADPCM = rate*channels/2.

The result fits a `datarate` kB/s device with the given latency margin, so the
movie's *actual* average bitrate is somewhat below `datarate` (the latency and
sector rounding are headroom). Example: 320x180, 12.5 fps, 25 frames/chunk,
11025 Hz mono VIDC-E8, 150 kB/s single-buffer -> 223232 bytes/chunk video ->
8929 bytes/frame.

## How they compose

`--loss-level` is the *starting* quality for modes 2 and 3 (the retry search
begins there). Setting an explicit `--target-bytes` or `--data-rate` overrides a
pure quality-factor encode. Don't combine `--target-bytes` and `--data-rate`;
device bandwidth simply computes a `--target-bytes` for you.
