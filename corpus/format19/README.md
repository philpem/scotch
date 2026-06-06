# Original Format-19 Cross-Check Corpus

This directory is reserved for payloads produced by the original Acorn
compressor and decoded by the original `Decomp19` implementation. Do not add
payloads produced only by the portable encoder as compatibility evidence.

## File Contract

Each manifest row names:

1. a stable fixture name;
2. width and height in pixels;
3. the raw format-19 payload;
4. the previous reconstructed frame, or `-` for a key frame;
5. the expected decoded frame captured from the Acorn decompressor;
6. a provenance note.

Frames use packed raster-order `6Y5UV`: exactly three bytes per pixel in
`Y,U,V` order. Y is `0..63`; U and V retain their five-bit modulo encoding
`0..31`. There is no header, row padding, or native C structure layout.

Keep source movies or emulator disk images outside this repository when their
licence or size makes inclusion inappropriate. The provenance field should
still identify the compressor version, emulator, extraction method, and a
cryptographic digest of external source material.

Run every populated row with:

```sh
tools/check_format19_corpus.sh build/replay-verify
```

The portable verifier reports the first differing pixel and both `Y,U,V`
triples. A successful comparison proves decoder agreement for the payload; it
does not by itself prove that the portable encoder makes identical decisions.
