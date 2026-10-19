"""Full first-block diagnostic: locate the exact first divergence per variant."""
import os
import struct

import dsp_formula_probe as P

m = P.parse(P.FILE)
c0 = m["chans"][0]

with open(P.REF_WAV, "rb") as fh:
    w = fh.read(4 * 20000 + 4096)
pos = 12
ref = None
while pos + 8 <= len(w):
    cid = w[pos:pos + 4]
    sz = struct.unpack_from("<I", w, pos + 4)[0]
    if cid == b"data":
        ref = w[pos + 8:pos + 8 + 4 * 20000]
        break
    pos += 8 + sz + (sz & 1)
N = m["spb"]  # 14336 frames in block0
ref0 = [struct.unpack_from("<h", ref, i * 4)[0] for i in range(N)]
chunk = m["data"][m["adpcm_start"]:m["adpcm_start"] + m["block_size"]]


def decode(hdr_mode, nib, csh, nf):
    coefs = c0["coefs"]
    hist1, hist2 = c0["hist1"], c0["hist2"]
    out = []
    for f in range(m["block_size"] // 8):
        b = chunk[f * 8:(f + 1) * 8]
        if f == 0 and nf == "init0_hi":
            hdr = (c0["init_ps"] >> 8) & 0xFF
        elif f == 0 and nf == "init0_lo":
            hdr = c0["init_ps"] & 0xFF
        else:
            hdr = b[0]
        pred, shift = (hdr >> 4, hdr & 0xF) if hdr_mode == "hi" else (hdr & 0xF, hdr >> 4)
        c1, c2 = coefs[pred * 2], coefs[pred * 2 + 1]
        for i in range(7):
            byte = b[1 + i]
            ns = ((byte >> 4), (byte & 0xF)) if nib == "hi" else ((byte & 0xF), (byte >> 4))
            for n in ns:
                n = P.sign4(n)
                val = (n << 12) >> shift
                a = (c1 * hist1 + c2 * hist2) >> csh
                s = P.clamp16(val + a)
                out.append(s)
                hist2, hist1 = hist1, s
    return out


results = []
for hdr_mode in ("hi", "lo"):
    for nib in ("hi", "lo"):
        for csh in (15, 16):
            for nf in ("frame", "init0_hi", "init0_lo"):
                got = decode(hdr_mode, nib, csh, nf)
                first = next((i for i in range(N) if got[i] != ref0[i]), -1)
                n_mis = sum(1 for i in range(N) if got[i] != ref0[i])
                results.append((n_mis, first, hdr_mode, nib, csh, nf))
results.sort()
print("%-8s %-6s %-4s %-4s %-4s %-8s" % ("n_mis", "first", "hdr", "nib", "csh", "nf"))
for n_mis, first, a, b_, c, d in results[:8]:
    print("%-8d %-6d %-4s %-4s %-4d %-8s" % (n_mis, first, a, b_, c, d))

best = results[0]
print("\nbest:", best)
n_mis, first, hdr_mode, nib, csh, nf = best
if first > 0:
    got = decode(hdr_mode, nib, csh, nf)
    lo = max(0, first - 6)
    print("\ncontext around first mismatch (index %d):" % first)
    for i in range(lo, min(first + 10, N)):
        print("  [%5d] ref=%7d got=%7d %s" % (i, ref0[i], got[i], "<<<" if i == first else ""))
    # raw bytes of the frame containing that sample
    f = first // 14
    print("frame %d bytes: %s" % (f, chunk[f * 8:(f + 1) * 8].hex()))
    print("prev frame   : %s" % chunk[(f - 1) * 8:f * 8].hex())
    # samples near frame boundary
    bstart = (f * 14)
    print("ref samples at frame start:", ref0[bstart:bstart + 14])
    print("got samples at frame start:", decode(hdr_mode, nib, csh, nf)[bstart:bstart + 14])
