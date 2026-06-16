# Container-level regression fixtures

Real ARMovie/Replay files kept for container-parsing and transcode regressions,
where the value is in a real header/catalogue quirk rather than codec output.
Unlike the codec cross-check corpora, these are not paired with expected decoded
frames.

## `dummy.ae7`

`!ARPlayer`'s placeholder movie (RISC OS file `ACORN.!ARPLAYER.DUMMY`, filetype
&AE7) from the *Acorn Replay Videoclip Collection Two*.

- size: 398 bytes
- SHA-256: `0f719c757105311e146194d901cd5b35552b94a51fe394cc2fa48815476f7fa8`

It is a **non-conforming sound-only movie**: video format 0 ("no video track")
but with non-zero dimensions on lines 6–9 (120×96, 16bpp, 12.5 fps), no sound
(sound format 0; the `666 channels` / `12000 Hz` lines are ignored leftovers),
and a single empty chunk (`000000398,0;0`). Per
[`../../docs/spec/ae7-armovie-container.md`](../../docs/spec/ae7-armovie-container.md)
§6.5 a reader must key on the format number, not the geometry, to recognise it
as having no video.

This is the file that exposed the `replay-transcode` regression where format 0
was mistaken for an unknown codec and the tool tried (and failed) to open a
non-existent `Decomp0/Decompress,ffd`. `test_transcode_dummy.sh` decodes it and
asserts a clean exit with no `Decomp0` lookup.
