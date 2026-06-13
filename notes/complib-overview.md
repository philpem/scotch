# CompLib Overview

`ARMovie_2003/bas/CompLib,ffb.txt` is a shared BBC BASIC library used by most
Acorn Replay `BatchComp,ffb.txt` compressors.

It identifies itself as:

```text
ARMovie Compression Library v 0.081 3rd February 1999
```

Compressors include it with:

```basic
LIBRARY "<ARMovie$Dir>.Tools.CompLib"
```

Examples include Moving Lines, Moving Blocks, Moving Blocks HQ, Moving Blocks
Beta, and most uncompressed/YUV repacking formats. Some older compressors, such
as the older `Decomp5` and `Decomp6` batch compressors, predate or duplicate
parts of this support instead of using the library.

## Role

CompLib is the batch-compression harness. It is not a codec in itself.

It provides:

- common command-line parsing;
- destination header reading;
- source ARMovie opening and chunk catalogue handling;
- source decompressor loading and frame expansion;
- temporal-source handling;
- optional scaling/trimming/filtering of source frames;
- optional display preview support;
- output chunk buffer management;
- key-frame file writing;
- checkpoint/restart support;
- common assembler helpers for painting, copying, source expansion, colour
  conversion, and dithering.

The codec-specific `BatchComp` program provides:

- the compression algorithm;
- frame-size/quality policy;
- output chunk sizing;
- codec-specific restart/checkpoint state;
- dummy-frame generation;
- optional per-chunk actions before saving.

## Typical Compressor Shape

A typical `BatchComp` starts like this:

```basic
LIBRARY "<ARMovie$Dir>.Tools.CompLib"
DEF FNversion="..."
PROCStartup(<compression-number>)
ON ERROR PROCError
```

Then it usually:

1. computes `framesize`, `qual%`, and data-rate policy;
2. allocates codec work buffers;
3. calls `PROCReadVDUParams`;
4. calls `PROCOpenSrcFile`;
5. calls `PROCInitDisplayTables`;
6. calls `PROCInitDecomp`;
7. calls `PROCCheckRestart`;
8. calls `PROCOpenLog`;
9. calls `PROCAsmLibCode`;
10. loops over source frames:
    - `PROCCheckSuspend`;
    - `PROCCheckCheckPoint`;
    - `PROCExpandNextSourceFrame(frame%)`;
    - codec-specific compression;
    - `PROCCheckChunkFinished(key_buffer)`;
11. calls `PROCFinishFinalChunk(key_buffer)`;
12. calls `PROCFinish`.

Moving Lines and Moving Blocks both follow this pattern.

## Command-Line Interface

`PROCStartup(c%)` parses the command line and sets shared variables. Important
options include:

- `-source <file>`: source ARMovie, default `Capture`.
- `-dest <dir>`: destination prefix/directory.
- `-index <n>`: use numbered `Header`, `Images`, and `Keys` set.
- `-startat <cs>`: start time in centiseconds.
- `-data <kB/s>`: target data rate, default `150`.
- `-latency <seconds>`: IO latency allowance, default `0.4`.
- `-double`: assume double-buffered data.
- `-size <bytes>`: override chosen frame size.
- `-quality <n>`: quality-driven mode.
- `-dirty`: quick-and-dirty frame-size/quality search mode.
- `-hiq <n>`: higher quality parameter used by some codecs.
- `-arm2`: clamp output for ARM2-class playback.
- `-nokeys`: do not write key-frame data.
- `-restart`: resume from a previous checkpoint.
- `-batch`: disable space-bar suspension.
- `-display <mode>`: show preview while compressing.
- `-notext`: suppress text output.
- `-nodither`: disable dithering during colour-depth reduction.
- `-ccir`: expand CCIR-range YUV values to full range.
- `-convert <colourspace>`: convert source frames to another colour layout,
  e.g. `8Y8UV`, `6Y5UV`, or `6Y6UV`.
- `-xsize`, `-ysize`, `-xtrim`, `-ytrim`: override or trim dimensions.
- `-small`: force small display magnification.
- `-filter <list>`: load one or more prefilters from `Tools.Filters`, separated
  by semicolons.

The help text does not mention every parsed option. For example, `-convert`,
`-ccir`, `-xsize`, `-ysize`, `-xtrim`, `-ytrim`, `-small`, `-filter`, `-hiq`,
and `-nodither` are parsed but not all listed in `PROChelp`.

## Destination Header Handling

`PROCReadHeader(dest$+index$+"Header", c%)` reads the destination ARMovie header
template. It checks but does not strictly require that the header compression
number matches `c%`.

It derives:

- `sx%`, `sy%`: destination dimensions;
- `sz%`: destination uncompressed frame size, normally `sx% * sy% * 2`;
- `fps`: destination frame rate;
- `fpf`: frames per output chunk;
- `chunktime`: `fpf / fps`;
- `soundbytes%`: estimated sound bytes per output chunk for type-1 sound tracks.

The codec compressor uses these values to choose a target video frame/chunk
size.

## Source ARMovie Handling

`PROCOpenSrcFile` opens the source ARMovie and reads its header. It handles:

- source compression type;
- source dimensions;
- source colour-space hints;
- source frame rate;
- source frames per chunk;
- source chunk count;
- maximum chunk size;
- chunk catalogue;
- optional fetcher information;
- key-frame offset;
- decompressor `Info` data.

It chooses the source decompressor directory:

- compression type `1` maps to `MovingLine`;
- other types map to `Decomp<n>`.

It inspects the source codec `Info` file. If the sixth line does not contain
`TEMP`, it sets `sourceisquick%=TRUE`; otherwise the source must be expanded
sequentially because frames may depend on previous-frame state.

For quick/fixed-size sources, CompLib can seek directly to a frame inside the
source chunk catalogue. For temporal sources, it loads chunks and repeatedly
calls the decompressor until it reaches the desired frame.

## Source Frame Expansion

`PROCInitDecomp` loads the source decompressor:

- `Dec24` if present, treated as an 8-bit-per-component source path;
- otherwise `Decompress`.

It calls the decompressor init entry and sets `decl` to the frame decompression
entry point.

`PROCExpandNextSourceFrame(frame%)` expands source frame `frame%` into `b2%`,
the canonical source frame seen by the codec compressor.

Expansion can include:

- direct read and decompression for quick sources;
- sequential decompression for temporal sources;
- copying/scaling from original source size to destination size;
- `ytrim`/`xtrim` style dimension overrides;
- optional source preview via `PROCextrapic`;
- filter-chain application via `FNFilters_Apply`.

For temporal input, the library maintains:

- `prevframe%`: previous expanded frame;
- `origprevframe%`: previous original-size frame;
- `currentframe%`: the current expanded source frame number.

## Colour Conversion and Dithering

`PROCAsmLibCode` emits ARM helper routines used by the compressors. One of the
larger jobs is converting 8-bit-component input down to the codec's expected
packed format.

The conversion path can:

- convert RGB to YUV using fixed coefficients;
- sign-extend U/V where needed;
- expand CCIR-range YUV if `-ccir` is used;
- dither Y using a Floyd-Steinberg-like error distribution;
- quantise to:
  - default 5:5:5 YUV/RGB style packed values;
  - `8Y8UV`;
  - `6Y5UV`;
  - `6Y6UV`.

`convertto$` is set by the compressor or `-convert`. Several codecs set it
before calling the library:

- `Decomp9`, `Decomp10`, `Decomp16`, `Decomp21`, `Decomp22`: `8Y8UV`;
- `Decomp23`, `Decomp24`: `6Y5UV`;
- `Decomp20`, `Decomp25`, `Decomp26`: `6Y6UV`.

## Chunk Output

CompLib owns the output chunk buffer:

- `PROCInitChunkMaker` allocates `op%` using codec callback `FNCompChunkSize`.
- `PROCZeroChunkMaker` resets chunk counters.
- `PROCCheckChunkFinished(KeyBuffer%)` increments `chunker%` and saves a chunk
  once `chunker% = fpf`.
- `PROCFinishFinalChunk(KeyBuffer%)` pads an incomplete final chunk with
  codec-provided `PROCOneDummyFrame`.

Primitive write helpers:

- `PROCopw(A%)`: write a halfword-sized quantity and advance `bop%` by 2 bytes.
- `PROCopdw(A%)`: write a word and advance by 4 bytes.
- `PROCopbw(A%)`: write a word-aligned bit/packed quantity and advance
  `bop%` by 16 bits.
- `PROCopb(A%)`: insert a 15-bit quantity at bit position `bop%`.

The bit helpers are unusual: `bop%` is sometimes used as a byte count and
sometimes as a bit count by codecs. Moving Blocks-family compressors often
manage their own bit pointer directly as well.

`PROCsavechunk(KeyBuffer%)` writes:

- compressed video chunks under `Images<n>.<nn>`;
- optional key-frame data under `Keys<n>.<nn>`.

The directory split uses groups of 77 files:

```text
Images<chunk DIV 77>.<two-digit chunk MOD 77>
Keys<chunk DIV 77>.<two-digit chunk MOD 77>
```

Before saving a chunk, it calls codec callback `PROCCompChunkActions`.

## Key Frames

If `dokeys` is true and the codec passes a non-zero key buffer,
`PROCsavechunk` writes a key-frame file.

The key buffer is passed through `squash`, an assembler helper that packs the
expanded frame down from word-per-pixel storage into halfword-like storage,
then saves `sz%` bytes.

For temporal codecs, these key files provide starting state for Player or
tools that need to start decompression at a chunk boundary.

`-nokeys` disables key writing.

## Checkpoint and Restart

CompLib supports manual and periodic checkpointing:

- `PROCCheckSuspend` watches for the space bar unless `-batch` is set.
- `PROCStartCheckCheckPoint` starts a 20-minute checkpoint timer.
- `PROCCheckCheckPoint` checkpoints once the timer expires.
- `PROCCheckPoint` saves current output bytes to `StoppedC` and metadata to
  `StoppedD`.

It calls codec callback `PROCCompCheckPoint(prefix$)` so the compressor can
save codec-specific state.

On restart, `PROCCheckRestart` reads `StoppedD`, restores generic state, calls
`PROCCompRestartInit(prefix$)`, allocates the chunk buffer, then reloads
`StoppedC`.

## Display Preview

If `-display` is used, CompLib can show frames as compression runs.

Display support includes:

- `PROCReadVDUParams`: reads current mode parameters and computes preview
  magnification;
- `PROCInitDisplayTables`: asks Player to claim a colour table for the source
  file and display configuration;
- `PROCPaintRectangle`, `PROCPaintRectangleL`, `PROCPaintRectangleR`: paint
  preview frames;
- `PROCextrapic`: paint original input at the top right when `-extrapic` is
  enabled.

The painter is assembled dynamically by `PROCAsmLibCode` and uses colour-table
lookup through `FNplook`.

## Filters

CompLib can load filter binaries from:

```text
<ARMovie$Dir>.Tools.Filters.<name>
```

`-filter` accepts a semicolon-separated list. Each filter is loaded into a
linked list. The filter interface appears to have at least two entry points:

- `filter + 4`: initialisation, called with `sx%`, `sy%`, and a source format
  flag;
- `filter + 8`: per-frame application, called with new frame, old frame, and
  output buffer pointers.

If filters are present, CompLib allocates `FBuff%` and `FBuff2%` as intermediate
frame buffers.

Temporal filters receive the previous frame pointer; on frame zero, old-frame
input is passed as zero.

## Codec Callback Contract

CompLib assumes the including `BatchComp` defines these names:

- `FNversion`: returns compressor version text.
- `PROCOneDummyFrame`: emits or simulates a frame used to pad the final chunk.
- `FNCompChunkSize`: returns maximum compressed chunk buffer size.
- `PROCCompNonRestartInit`: initialise codec-specific state for a fresh run.
- `PROCCompRestartInit(prefix$)`: restore codec-specific state for restart.
- `PROCCompClose`: close/free codec-specific resources.
- `PROCCompFinish`: final codec-specific completion.
- `PROCCompCheckPoint(prefix$)`: save codec-specific checkpoint data.
- `PROCCompChunkActions`: update logs/stats or finalise per-chunk state before
  the chunk is saved.

Some compressors define `PROCCompCheckPoint` twice, or provide empty callbacks,
which suggests the contract was informal and evolved over time.

## Important Shared Variables

The library exposes many globals to codec code. Commonly used ones include:

- `sx%`, `sy%`, `sz%`: destination frame geometry.
- `fps`, `fpf`, `chunktime`: destination timing/chunking.
- `soundbytes%`: estimated sound bytes per output chunk.
- `srcfile$`, `dest$`, `index$`: source and destination naming.
- `iframe`, `sframe%`, `maxframe%`: source frame range.
- `sourcefps`, `sourcefpf%`, `sourcenchunks%`: source timing/chunking.
- `sourceisquick%`: source frames are independently addressable.
- `b2%`: current expanded source frame for the compressor.
- `prevframe%`: previous expanded frame.
- `op%`, `bop%`, `bopl%`: current output chunk buffer, current position, and
  allocated size.
- `chunker%`, `chunk%`: frame count within current chunk and current chunk
  number.
- `framesize`, `framesizeset`, `datarate`, `latency`, `double`: rate-control
  inputs used by codecs.
- `qual%`, `qd%`, `hiq%`, `totq%`: quality-control inputs/state.
- `display`, `rowbytes%`, `screenstart%`, `magx%`, `magy%`: display preview
  state.
- `convertto$`, `ccir`, `dither`: colour conversion controls.

## Implications for Portable C Tooling

For new C tooling, CompLib separates into reusable concepts:

- a common ARMovie reader;
- a source-frame decoder/normaliser;
- optional scaling/trimming/filtering;
- colour conversion and quantisation;
- a chunk writer;
- key-frame writer;
- codec-specific encoder callbacks.

The compressor-specific algorithms should be documented separately, but their
existing BASIC implementations rely heavily on CompLib's conventions and global
state. Reimplementing Moving Lines or Moving Blocks in C will be easier if we
first define C equivalents of:

- the canonical expanded frame (`b2%`-style word-per-pixel YUV/RGB);
- previous-frame handling;
- chunk/key output layout;
- rate-control inputs (`datarate`, `latency`, `framesize`, `quality`);
- colour conversion targets (`5:5:5`, `6Y5UV`, `6Y6UV`, `8Y8UV`).
