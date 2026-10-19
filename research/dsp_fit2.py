"""Expanded single-frame fit: search nibble order, nibble scale, coef order,
history order, predictor nibble, coef shift and scale shift per frame."""
import os
import struct

import dsp_formula_probe as P

m = P.parse(P.FILE)
COEFS = m["chans"][0]["coefs"]

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

FRAMES = range(218, 260)


def nibs_of(b, order):
    out = []
    for i in range(7):
        byte = b[1 + i]
        if order == "hi":
            out += [byte >> 4, byte & 0xF]
        else:
            out += [byte & 0xF, byte >> 4]
    return out


def sim(b, h1, h2, pred, se, csh, nl, order, coef_swap, hist_swap):
    c1, c2 = COEFS[pred * 2], COEFS[pred * 2 + 1]
    if coef_swap:
        c1, c2 = c2, c1
    out = []
    for n in nibs_of(b, order):
        n = P.sign4(n)
        val = (n << nl) >> se
        a = ((c1 * h1 + c2 * h2) >> csh) if not hist_swap else ((c1 * h2 + c2 * h1) >> csh)
        s = P.clamp16(val + a)
        out.append(s)
        h2, h1 = h1, s
    return out


space = []
for order in ("hi", "lo"):
    for nl in range(9, 17):
        for csh in (14, 15, 16):
            for coef_swap in (0, 1):
                for hist_swap in (0, 1):
                    for pred_mode in ("hi", "lo"):
                        for se in range(0, 25):
                            space.append((order, nl, csh, coef_swap, hist_swap, pred_mode, se))
print("search space per frame:", len(space))

good = None
per_frame = {}
for f in FRAMES:
    s0 = f * 14
    if s0 + 14 > len(REF):
        break
    b = chunk[f * 8:(f + 1) * 8]
    h1, h2 = REF[s0 - 1], REF[s0 - 2]
    want = REF[s0:s0 + 14]
    hits = []
    for (order, nl, csh, cs, hs, pm, se) in space:
        pred = (b[0] >> 4) if pm == "hi" else (b[0] & 0xF)
        if sim(b, h1, h2, pred, se, csh, nl, order, cs, hs) == want:
            hits.append((order, nl, csh, cs, hs, pm, se))
    per_frame[f] = hits
    print("frame %d hdr=0x%02X: %d hits %s" % (f, b[0], len(hits), hits[:6]))

common = None
for f, hits in per_frame.items():
    s = set(hits)
    common = s if common is None else (common & s)
    if not common:
        break
print("\nframes tested:", len(per_frame))
print("combos that fit ALL frames:", sorted(common) if common else "NONE")
