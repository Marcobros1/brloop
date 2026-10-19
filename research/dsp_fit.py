"""Single-frame fit: given exact history from the reference WAV, find which
(nibble scale shift, predictor nibble, coef shift) reproduces each frame."""
import os
import struct

import dsp_formula_probe as P

m = P.parse(P.FILE)
c0 = m["chans"][0]
COEFS = c0["coefs"]

with open(P.REF_WAV, "rb") as fh:
    w = fh.read(4 * 40000 + 4096)
pos = 12
while pos + 8 <= len(w):
    cid = w[pos:pos + 4]
    sz = struct.unpack_from("<I", w, pos + 4)[0]
    if cid == b"data":
        ref = w[pos + 8:pos + 8 + 4 * 40000]
        break
    pos += 8 + sz + (sz & 1)
REF = [struct.unpack_from("<h", ref, i * 4)[0] for i in range(40000)]

chunk = m["data"][m["adpcm_start"]:m["adpcm_start"] + m["block_size"]]


def sim_frame(b, h1, h2, pred, se, csh):
    out = []
    c1, c2 = COEFS[pred * 2], COEFS[pred * 2 + 1]
    for i in range(7):
        byte = b[1 + i]
        for n in (byte >> 4, byte & 0xF):
            n = P.sign4(n)
            val = (n << 12) >> se
            a = (c1 * h1 + c2 * h2) >> csh
            s = P.clamp16(val + a)
            out.append(s)
            h2, h1 = h1, s
    return out


fits = {}   # (hdr, pred_mode, csh) -> set of se that matched
count = 0
for f in range(1, len(chunk) // 8):
    s0 = f * 14
    if s0 + 14 > len(REF):
        break
    b = chunk[f * 8:(f + 1) * 8]
    h1, h2 = REF[s0 - 1], REF[s0 - 2]
    want = REF[s0:s0 + 14]
    if max(abs(v) for v in want) > 4000:
        continue
    hdr = b[0]
    for pred_mode in ("hi", "lo"):
        pred = (hdr >> 4) if pred_mode == "hi" else (hdr & 0xF)
        for csh in (15, 16):
            for se in range(0, 17):
                if sim_frame(b, h1, h2, pred, se, csh) == want:
                    fits.setdefault((hdr, pred_mode, csh), set()).add(se)
    count += 1

print("frames tested:", count, " fitted frames:", len(fits))
print("\n(hdr, pred_mode, coef_shift) -> scale shifts that reproduce the frame")
for k in sorted(fits):
    print("  hdr=0x%02X pred=%s csh=%d -> se=%s" % (k[0], k[1], k[2], sorted(fits[k])))

# summarize: is one (pred_mode, csh, se-function-of-hdr) consistent for all?
print("\n--- consistency check ---")
from collections import defaultdict
by_mode = defaultdict(list)
for (hdr, mode, csh), ses in fits.items():
    by_mode[(mode, csh)].append((hdr, sorted(ses)))
for k, v in sorted(by_mode.items()):
    unique_se = set()
    for hdr, ses in v:
        unique_se |= set(ses)
    print("  pred=%s csh=%d: frames=%d se-candidates union=%s" % (k[0], k[1], len(v), sorted(unique_se)))
