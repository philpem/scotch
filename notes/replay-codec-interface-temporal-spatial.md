# Replay Codec Interface: Temporal and Spatial

This note answers the initial question: how the `Temporal` and `Spatial`
compression options affect interaction between the Replay Player and a video
codec.

## Source Documents Read

- `ARMovie_2003/Resources/Documents/AE7doc`
- `ARMovie_2003/Resources/Documents/CodecIf`
- `ARMovie_2003/Resources/Documents/DecompIf`
- `ARMovie_2003/Resources/Documents/ProgIf`
- `ARMovie_2003/Resources/Documents/ToCapture`
- `ARMovie_2003/bas/Player,ffb.txt`
- `ARMovie_2003/bas/CompLib,ffb.txt`
- selected `Decomp*/Resources/Info` files

## Where the Flags Live

The flags are stored in each codec's `Info` file. The `Info` file format
described by `CodecIf` is:

1. Name of format
2. Originator
3. bits per pixel, optionally with `C` for the C-friendly call sequence
4. desired/min/max x size
5. desired/min/max y size
6. temporal/spatial identifier string, or empty
7. supported colour spaces

Examples:

- `ARMovie_2003/Video/MovingLine/Resources/Info`: `Temporal,Spatial`
- `ARMovie_2003/Video/Decomp7/Resources/Info`: `Temporal,Spatial`
- `ARMovie_2003/Video/Decomp17/Resources/Info`: `Temporal,Spatial`
- `ARMovie_2003/Video/Decomp22/Resources/Info`: `Spatial`
- uncompressed formats usually leave this field empty

`ProgIf` says Player caches this information in `ARMovie$Decomp<n>Info`.
The second cached character is documented as:

```text
0/1: source is not temporally compressed
```

The implementation in `Player,ffb.txt` reads the sixth `Info` line and sets
`sourceisquick%=TRUE` if that line does not contain `TEMP`.

## Temporal

`Temporal` means the compressed stream can depend on earlier decoded frame
state. In the decompressor call interface, Player always passes:

```text
r0 - source byte pointer
r1 - output pointer
r2 - previous output pointer
r3 - pixel dither / colour lookup table
```

For temporal codecs, `r2` is significant: it gives the decompressor access to
the previous decoded output buffer. Moving Lines and Moving Blocks use this to
copy pixels/blocks from the previous frame.

The Player-visible consequences are:

- Player cannot treat every compressed frame as independently addressable.
- To start playback at an arbitrary frame in a temporal stream, Player may need
  a key-frame state from the AE7 key-frame area.
- If no independent start point is available, Player has to decode forward
  sequentially until it reaches the requested frame.
- CompLib also has to expand temporal sources sequentially when recompressing.

`AE7doc` describes the key-frame offset as a block of per-chunk uncompressed
state that lets decompression start at a chunk even when the decompressor
cannot start there unaided. `CodecIf` says this was needed for compressors such
as Moving Lines.

In `CompLib,ffb.txt`, non-temporal source codecs are handled by seeking
directly to `frame * frame_size`. Temporal source codecs allocate a whole chunk
buffer and repeatedly call the decompressor until `currentframe%` reaches the
requested frame.

## Spatial

`Spatial` means the compressed stream can refer to already-decoded parts of the
current frame. It is intra-frame prediction/copying.

The important point is that this does not appear to change the Player's formal
decompressor call. Player still provides the same source pointer, output
pointer, previous output pointer, and lookup table. The decompressor itself
knows how to interpret spatial references because it is writing the current
output frame in a deterministic scan order.

The Moving Blocks stream documents describe this explicitly: spatial copies
come from previous parts of the current frame, and the frame is scanned
top-left to bottom-right so the source pixels already exist.

Moving Lines also has a combined temporal/spatial offset table in its
decompressor source:

```text
r5 - temporal table of line offsets (0-&11f) plus spatial offsets (&120-&1cb)
```

So `Spatial` is an advertised bitstream property and an encoder/decompressor
capability, not a separate Player service in the way `Temporal` affects random
access and previous-frame state.

## Combined Effect

For `Temporal,Spatial` codecs such as Moving Lines, Moving Blocks, and Moving
Blocks HQ:

- temporal references copy from the previous decoded frame via `r2`;
- spatial references copy from already-written pixels/blocks in the current
  output frame via offsets from `r1` or the current write pointer;
- Player must preserve and pass previous-frame output buffers;
- arbitrary start/play-for behavior may require key-frame state or sequential
  warm-up decoding.

For `Spatial` without `Temporal`, such as `Decomp22` according to its `Info`
file:

- frames can be independent from previous frames at the Player level;
- the decompressor may still use intra-frame prediction while decoding a frame;
- random access should be closer to an uncompressed/fixed-ratio source, subject
  to chunk layout and frame sizing.

For codecs with an empty identifier field:

- Player treats the source as "quick": not temporally compressed;
- CompLib can seek directly by fixed frame size where the format is fixed-size;
- `r2` is still passed by the interface but need not be used.

## Current Inference

`Temporal` has a clear Player interaction: it affects random access, key-frame
requirements, and sequential expansion. `Spatial` is exposed in the `Info` file
for tools such as !AREncode, but the documents and Player source do not show a
distinct Player-side control path for it. It is handled inside the codec.
