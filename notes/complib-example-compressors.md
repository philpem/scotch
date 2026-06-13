# Simple CompLib-Based Compressor Examples

This note signposts small `BatchComp` programs that are useful when learning
the `CompLib` contract.

## Best Minimal Example: Decomp2

Path:

```text
ARMovie_2003/Video/Decomp2/bas/BatchComp,ffb.txt
```

This is the simplest useful CompLib-based compressor found so far. It targets
format 2, 16-bit uncompressed frames.

Why it is useful:

- only 178 lines;
- uses the normal CompLib startup and frame loop;
- has trivial rate control: `framesize = sz%`;
- defines the required `PROCComp*` callback stubs;
- shows how a codec writes to `op%` and advances `bop%`;
- contains small assembler helpers to pack/unpack the frame format.

The core flow is:

```basic
PROCStartup(2)
PROCReadVDUParams
PROCOpenSrcFile
PROCInitDisplayTables
PROCInitDecomp
PROCCheckRestart
PROCOpenLog
PROCAsmLibCode
PROCass

FOR frame%=iframe TO maxframe%-1 STEP fsr
  PROCCheckSuspend
  PROCCheckCheckPoint
  PROCExpandNextSourceFrame(frame%)
  PROCmatch
  PROCCheckChunkFinished(b3%)
NEXT

PROCFinishFinalChunk(b3%)
PROCFinish
```

`PROCmatch` simply converts the canonical expanded source frame `b2%` into the
format-2 packed output in `op%`, decodes it back into `b3%` for preview/key
state, then advances `bop%`.

This is the best first example for understanding the CompLib harness without
also learning a compression algorithm.

## Same Shape With Colour Conversion: Decomp9

Path:

```text
ARMovie_2003/Video/Decomp9/bas/BatchComp,ffb.txt
```

This is almost the same pattern as `Decomp2`, but it sets:

```basic
convertto$="8Y8UV"
```

before `PROCOpenSrcFile`.

That makes it a compact example of how a compressor asks CompLib to normalise
input into a different YUV packing before the codec-specific repacker runs.

## Short But Less Acorn-Specific: Decomp800

Path:

```text
ARMovie_2003/Video/Decomp800/bas/BatchComp,ffb.txt
```

This is only 146 lines and is fairly well signposted, including a comment block
that marks the procedures called by CompLib. It is useful for the callback
contract, but less useful for studying Acorn encoder logic because the real
codec is delegated to separate `Compress` and `Decompress` binaries:

```text
ARMovie_2003/Video/Decomp800/Compress,ffd
ARMovie_2003/Video/Decomp800/Decompress,ffd
```

Use it as a clean example of integrating an external codec with CompLib.

## Not Minimal: Moving Lines and Moving Blocks

These are the important Acorn codecs but not good first examples of CompLib:

- `ARMovie_2003/Video/MovingLine/bas/BatchComp,ffb.txt`: 959 lines.
- `ARMovie_2003/Video/Decomp7/bas/BatchComp,ffb.txt`: 2006 lines.
- `ARMovie_2003/Video/Decomp17/bas/BatchComp,ffb.txt`: 2700 lines.
- `ARMovie_2003/Video/Decomp20/bas/BatchComp,ffb.txt`: 2686 lines.
- `ARMovie_2003/Video/Decomp19/bas/BatchComp,ffb.txt`: 2618 lines.

They follow the same broad harness shape, but most of the file is codec
decision logic and hand-assembled ARM code.
