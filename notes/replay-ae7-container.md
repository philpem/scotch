# Replay AE7 Container Notes

## Header And Catalogue

An ARMovie/AE7 file starts with 21 newline-terminated text fields. Numeric
fields may have explanatory text after the leading number. Field 15, described
as the number of chunks, is actually the highest zero-based chunk index. A
value of 14 therefore selects 15 catalogue entries, numbered 0 through 14.

The catalogue offset names a text table. Each entry has the form:

```text
file_offset,video_bytes;sound_track_0_bytes[;sound_track_1_bytes...]
```

`video_bytes` covers the chunk's video region, but that region is not a table
of length-prefixed frames. The player calls the codec once per frame and uses
the returned source pointer as the next frame's start. Exact frame extraction
therefore requires parsing the bitstream or running the decoder.

## LionFish19 Sample

`LionFish19,ae7` has SHA-256
`e4a6539b19a105e80e3171a4753870b184edafded0ee874bf2f470231b661684`.
Its relevant fields are:

- type 19, Super Moving Blocks;
- 160x128, 16-bit 6Y5UV;
- 12.5 frames/s;
- 25 frames per chunk;
- last chunk index 14, hence 15 chunks and 375 frames;
- no key-frame table (`-1`);
- one 44,100-byte sound region per chunk.

Chunk 0 starts at file offset 83,968 and declares 181,886 video bytes. Calling
compiled Decomp19 25 times consumes 181,885 bytes and leaves one padding byte.
The first frame occupies 9,826 bytes; subsequent chunk-0 frames occupy between
6,460 and 7,730 bytes.

All 25 reconstructed frames from compiled Decomp19 match the portable type 19,
Super Moving Blocks verifier byte-for-byte. Corpus fixtures retain chunk-0
frames 0 and 1 so both an independent first frame and a temporal dependency are
tested continuously.

## Tooling

`replay-inspect` parses and validates the header, catalogue, sound-track totals,
and payload bounds. `tools/decomp19_unicorn.py --frames N` initializes the codec
once, alternates current and previous reconstruction buffers, and records frame
boundaries from returned CodecIf source pointers. This avoids guessing frame
sizes from byte patterns or catalogue averages.
