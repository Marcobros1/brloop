"""T0 analysis: compare FLSM output against its input BRSTM.

Reports header fields (codec/loop/rate/loop_start/total), every differing byte
range, and whether the audio DATA chunk is untouched.
"""
import struct
import sys
import os

# MKW_WORKDIR: optional folder with working copies (5_1_N.brstm and 5_1_F.brstm).
# Without it, the input comes from the collection (MKW_COLLECTION) and the FLSM
# output is not compared, so the script needs both files to run.
WORKDIR = os.environ.get("MKW_WORKDIR", "")
COLLECTION = os.environ.get("MKW_COLLECTION", os.path.join(os.environ["APPDATA"], "CT-MKWII", "Mods", "Metal Kart Wii Retro Rewind"))
A = os.path.join(WORKDIR, "5_1_N.brstm") if WORKDIR else ""
B = os.path.join(WORKDIR, "5_1_F.brstm") if WORKDIR else ""
if not B or not os.path.exists(B):
    sys.exit("flsm_compare: set MKW_WORKDIR to the folder holding 5_1_F.brstm (the FLSM output)")
if not A or not os.path.exists(A):
    A = os.path.join(COLLECTION, "5_1_N.brstm")


def parse(path):
    with open(path, "rb") as fh:
        data = fh.read()
    head_off = struct.unpack_from(">I", data, 0x10)[0]
    data_off = struct.unpack_from(">I", data, 0x20)[0]
    part1 = head_off + 0x08 + struct.unpack_from(">I", data, head_off + 0x0C)[0]
    return dict(
        size=len(data), declared=struct.unpack_from(">I", data, 0x08)[0],
        magic=data[0:4], ver=data[6:8].hex(),
        head_off=head_off, data_off=data_off,
        codec=data[part1], loopflag=data[part1 + 1], ch=data[part1 + 2],
        rate=struct.unpack_from(">H", data, part1 + 4)[0],
        loopstart=struct.unpack_from(">I", data, part1 + 8)[0],
        total=struct.unpack_from(">I", data, part1 + 0x0C)[0],
        part1=part1,
    )


a = open(A, "rb").read()
b = open(B, "rb").read()
fa, fb = parse(A), parse(B)

print("input :", A, len(a), "bytes")
print("output:", B, len(b), "bytes")
for k in ("declared", "ver", "codec", "loopflag", "ch", "rate", "loopstart", "total", "data_off"):
    mark = "  <-- DIFF" if fa[k] != fb[k] else ""
    print("  %-10s in=%-10s out=%-10s%s" % (k, fa[k], fb[k], mark))
if fa["rate"] and fb["rate"]:
    print("  ratio out/in rate: %.6f" % (fb["rate"] / fa["rate"]))

# byte-level diff: contiguous differing ranges
n = min(len(a), len(b))
ranges = []
i = 0
while i < n:
    if a[i] != b[i]:
        j = i
        while j < n and a[j] != b[j]:
            j += 1
        ranges.append((i, j))
        i = j
    else:
        i += 1
print("\ndiffering ranges:", len(ranges), " length-delta:", len(b) - len(a))
shown = 0
for s, e in ranges:
    if shown >= 25:
        print("  ...")
        break
    print("  0x%06X..0x%06X (%d bytes): in=%s out=%s" %
          (s, e, e - s, a[s:e][:16].hex(), b[s:e][:16].hex()))
    shown += 1

# is any byte after the ADPCM audio data start different?
data_off = fa["data_off"]
after = [r for r in ranges if r[0] >= data_off]
print("\nranges inside/after DATA chunk (audio bytes):", len(after))
if len(b) >= data_off and len(a) >= data_off:
    print("audio payload (from DATA+8) byte-identical:",
          a[data_off + 8:] == b[data_off + 8:][:len(a) - data_off - 8] and len(a) == len(b))
