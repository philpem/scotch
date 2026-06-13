# Replay Codec Input Formats (from ToCapture)

Source: `ARMovie_RO371/Resources/Documents/ToCapture` ("Requirements for an
Acorn Replay Capture program"). This records what *uncompressed* data the
Moving Lines / Moving Blocks compressors actually consume, and confirms several
container-writer design points.

## Uncompressed input pixel formats

The compressors take an uncompressed Acorn Replay file as input. Six forms
exist; all are ARMovie compression types with 16-bit pixel depth (YUV flagged in
the pixel-depth field) except grayscale:

| Type | Name              | Packing                                            | bits/px |
|------|-------------------|----------------------------------------------------|---------|
| 2    | 15-bit RGB        | R 0-4, G 5-9, B 10-14, bit15=0; one halfword/pixel  | 16 |
| 2    | 15-bit YUV        | Y 0-4, U 5-9, V 10-14; halfword/pixel (YUV flag)    | 16 |
| 3    | 20-bit YYUV pairs | Y1 0-4, Y2 5-9, U 10-14, V 15-19; 16 px in 5 words  | 10 |
| 4    | 8-bit grayscale   | one pixel/byte (playable, no compressor)            | 8 |
| 5    | 30-bit YYYYUV quad| 2x2 luma + shared UV, one word                      | 8 |
| 6    | YYYY..UV sixteens | 4x4 luma + shared UV, three words                   | 6 |

Our `replay-extract` already reads the type 2 path (`type19-fields`) and type
23; types 3/5/6 are further subsampled YUV packings we have not needed yet.

Capture guidance worth preserving:

- Occupy the full 0-31 component range (no pedestal); gamma already applied.
- Floyd-Steinberg (low error-propagation) dithering when reducing 8->5 bits;
  for YUV it is acceptable to dither only Y. Sharpening hurts compression.
- Frame width should be a multiple of 8 pixels; 160x128 / 160x120 are typical.
- 25 fps uses all frames; a 12.5 fps master uses only the even frames. A 12.5
  fps capture need only be a multiple of 25 frames long.

## Container points confirmed (relevant to the Join writer)

These directly corroborate `replay-ae7-join-writer.md`:

- **Number-of-chunks field is the count minus one.** "a 2 second long movie
  with 2 second chunks has 0 as the number of chunks."
- **The last chunk may hold fewer frames**; just write its catalogue entry
  correctly. Sound can be cut short by a shorter catalogue entry.
- **Sound samples per chunk vary** "especially if approximating an irrational
  frequency": the catalogue must be written *after* the chunks are laid out.
  This is why our writer time-slices audio per chunk and computes the catalogue
  from the realised chunk offsets rather than assuming a constant sound size.
- **Double-buffered chunk IO**: the Player loads two chunks at once, so chunk
  size drives playback memory. A 25 fps 160x128 type-2 movie with 2 s chunks is
  2 MB/chunk; using 1 s chunks halves the working set. This is the rationale for
  both the even/odd chunk-size buffer hints and a configurable chunk duration.
