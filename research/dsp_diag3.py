"""Isolate the nibble->value transform: list frames with nonzero header in
low-amplitude context (history ~0, so ref ~= nibble value contribution)."""
import os
import struct

import dsp_formula_probe as P

m = P.parse(P.FILE)
c0 = m["chans"][0]

with open(P.REF_WAV, "rb") as fh:
    w = fh.read(4 * 40000 + 4096)
pos = 12
ref = None
while pos + 8 <= len(w):
    cid = w[pos:pos + 4]
    sz = struct.unpack_from("<I", w, pos + 4)[0]
    if cid == b"data":
        ref = w[pos + 8:pos + 8 + 4 * 40000]
        break
    pos += 8 + sz + (sz & 1)
REF = [struct.unpack_from("<h", ref, i * 4)[0] for i in range(40000)]

chunk = m["data"][m["adpcm_start"]:m["adpcm_start"] + m["block_size"]]
print("history init: %d %d" % (c0["hist1"], c0["hist2"]))

shown = 0
for f in range(len(chunk) // 8):
    b = chunk[f * 8:(f + 1) * 8]
    hdr = b[0]
    if hdr == 0:
        continue
    s0 = f * 14
    win = REF[max(0, s0 - 2):s0 + 14]
    if not win or max(abs(v) for v in win) > 600:
        continue
    if s0 < 2 or abs(REF[s0 - 1]) > 60 or abs(REF[s0 - 2]) > 60:
        continue
    nibs = []
    for i in range(7):
        nibs += [b[1 + i] >> 4, b[1 + i] & 0xF]
    print("frame %5d hdr=0x%02X (hi=%d lo=%d) nibs=%s" %
          (f, hdr, hdr >> 4, hdr & 0xF, "".join("%X" % n for n in nibs)))
    print("   ref =%s" % REF[s0:s0 + 14])
    shown += 1
    if shown >= 12:
        break
print("shown:", shown)
