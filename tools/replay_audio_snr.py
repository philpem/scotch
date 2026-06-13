#!/usr/bin/env python3
"""Decode an Acorn Replay/AE7 movie's sound track and (optionally) measure its
signal-to-noise ratio against the reference PCM it was made from.

Handles the formats this toolchain produces: format 1 VIDC E8 (exponential),
8/16-bit signed linear, and IMA ADPCM (mono and stereo, format 1 SoundA4 or
format 2 "2 adpcm"). Sound is read per chunk straight from the catalogue, the
same way the player does.

    replay_audio_snr.py MOVIE,ae7 [--reference ref.s16le] [--output out.s16le]

The reference must be signed-16 little-endian PCM at the movie's rate and
channel count (e.g. `ffmpeg -i in -vn -ac N -ar R -f s16le ref.s16le`).
"""
import argparse
import math
import struct
import sys

# Acorn's exact VIDC E8 decode table (ELogToLinTable), code -> signed 16-bit.
E_LOG_TO_LIN = [
    0, 0, 8, -8, 16, -16, 24, -24, 32, -32, 40, -40, 48, -48, 56, -56,
    64, -64, 72, -72, 80, -80, 88, -88, 96, -96, 104, -104, 112, -112, 120, -120,
    128, -128, 144, -144, 160, -160, 176, -176, 192, -192, 208, -208, 224, -224,
    240, -240, 256, -256, 272, -272, 288, -288, 304, -304, 320, -320, 336, -336,
    352, -352, 368, -368, 384, -384, 416, -416, 448, -448, 480, -480, 512, -512,
    544, -544, 576, -576, 608, -608, 640, -640, 672, -672, 704, -704, 736, -736,
    768, -768, 800, -800, 832, -832, 864, -864, 896, -896, 960, -960,
    1024, -1024, 1088, -1088, 1152, -1152, 1216, -1216, 1280, -1280, 1344, -1344,
    1408, -1408, 1472, -1472, 1536, -1536, 1600, -1600, 1664, -1664, 1728, -1728,
    1792, -1792, 1856, -1856, 1920, -1920, 2048, -2048, 2176, -2176, 2304, -2304,
    2432, -2432, 2560, -2560, 2688, -2688, 2816, -2816, 2944, -2944, 3072, -3072,
    3200, -3200, 3328, -3328, 3456, -3456, 3584, -3584, 3712, -3712, 3840, -3840,
    3968, -3968, 4224, -4224, 4480, -4480, 4736, -4736, 4992, -4992, 5248, -5248,
    5504, -5504, 5760, -5760, 6016, -6016, 6272, -6272, 6528, -6528, 6784, -6784,
    7040, -7040, 7296, -7296, 7552, -7552, 7808, -7808, 8064, -8064, 8576, -8576,
    9088, -9088, 9600, -9600, 10112, -10112, 10624, -10624, 11136, -11136,
    11648, -11648, 12160, -12160, 12672, -12672, 13184, -13184, 13696, -13696,
    14208, -14208, 14720, -14720, 15232, -15232, 15744, -15744, 16256, -16256,
    17280, -17280, 18304, -18304, 19328, -19328, 20352, -20352, 21376, -21376,
    22400, -22400, 23424, -23424, 24448, -24448, 25472, -25472, 26496, -26496,
    27520, -27520, 28544, -28544, 29568, -29568, 30592, -30592, 31616, -31616,
]
ADPCM_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
ADPCM_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2747, 3022, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
]


def parse_header(data):
    """Return (fields, catalogue offset) from the 21-line text header."""
    lines, pos = [], 0
    for _ in range(21):
        nl = data.index(b"\n", pos)
        lines.append(data[pos:nl].decode("latin1"))
        pos = nl + 1
    return lines


def lead_int(line):
    n = ""
    for c in line.strip():
        if c.isdigit() or (c == "-" and not n):
            n += c
        else:
            break
    return int(n) if n else 0


def catalogue(data, lines):
    cat_off = lead_int(lines[17])
    nchunks = lead_int(lines[14]) + 1
    pos, chunks = cat_off, []
    for _ in range(nchunks):
        nl = data.index(b"\n", pos)
        row = data[pos:nl].decode("latin1")
        pos = nl + 1
        off, rest = row.split(",", 1)
        vid, snd = rest.split(";", 1)
        chunks.append((int(off), int(vid), int(snd.split(";")[0])))
    return chunks


def adpcm_decode(codes, valprev, index):
    out = []
    for c in codes:
        step = ADPCM_STEP[index]
        diff = step >> 3
        if c & 4:
            diff += step
        if c & 2:
            diff += step >> 1
        if c & 1:
            diff += step >> 2
        valprev += -diff if (c & 8) else diff
        valprev = max(-32768, min(32767, valprev))
        index = max(0, min(88, index + ADPCM_INDEX[c]))
        out.append(valprev)
    return out, valprev, index


def decode(data, args_fmt=None):
    lines = parse_header(data)
    sound_fmt = lead_int(lines[9])
    channels = max(1, lead_int(lines[11]))
    label = lines[12].upper()
    name = lines[9].split(None, 1)
    name = name[1].upper() if len(name) > 1 else ""
    is_adpcm = "ADPCM" in label or "ADPCM" in name
    is_lin = "LIN" in label
    bits = lead_int(lines[12])
    chunks = catalogue(data, lines)

    samples = []  # interleaved
    st = [(0, 0)] * channels  # ADPCM per-channel state, runs across chunks
    for off, vid, snd in chunks:
        region = data[off + vid:off + vid + snd]
        if snd == 0:
            continue
        if is_adpcm:
            hdr = 4 * channels
            states = []
            for ch in range(channels):
                v = struct.unpack_from("<h", region, ch * 4)[0]
                i = region[ch * 4 + 2]
                states.append((v, i))
            body = region[hdr:]
            if channels == 1:
                codes = []
                for b in body:
                    codes.append(b & 0xF)
                    codes.append(b >> 4)
                dec, _, _ = adpcm_decode(codes, *states[0])
                samples.extend(dec)
            else:
                lc = [b & 0xF for b in body]
                rc = [b >> 4 for b in body]
                dl, _, _ = adpcm_decode(lc, *states[0])
                dr, _, _ = adpcm_decode(rc, *states[1])
                for a, b in zip(dl, dr):
                    samples.append(a)
                    samples.append(b)
        elif sound_fmt == 1 and not is_lin:  # VIDC E8 exponential
            samples.extend(E_LOG_TO_LIN[b] for b in region)
        elif is_lin and bits == 16:
            samples.extend(struct.unpack("<%dh" % (len(region) // 2), region))
        else:  # 8-bit signed linear
            samples.extend((b - 256 if b > 127 else b) << 8 for b in region)
    return samples, channels


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("movie")
    ap.add_argument("--reference", help="signed-16 LE PCM the movie was made from")
    ap.add_argument("--output", help="write decoded PCM (signed-16 LE) here")
    args = ap.parse_args()

    data = open(args.movie, "rb").read()
    samples, channels = decode(data)
    print("decoded %d samples, %d channel(s)" % (len(samples), channels))
    if args.output:
        with open(args.output, "wb") as f:
            f.write(struct.pack("<%dh" % len(samples),
                                *(max(-32768, min(32767, s)) for s in samples)))
    if args.reference:
        ref = open(args.reference, "rb").read()
        ref = list(struct.unpack("<%dh" % (len(ref) // 2), ref))
        n = min(len(samples), len(ref))
        sig = sum(x * x for x in ref[:n]) / n
        err = sum((a - b) ** 2 for a, b in zip(samples[:n], ref[:n])) / n
        snr = 10 * math.log10(sig / err) if err else float("inf")
        print("SNR vs reference: %.1f dB over %d samples" % (snr, n))


if __name__ == "__main__":
    sys.exit(main())
