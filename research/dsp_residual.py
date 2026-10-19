"""Residual analysis: r_i = REF_i - predictor(REF_{i-1}, REF_{i-2}) should be
the pure nibble contribution. Find which coefficient pair makes r_i / nibble_i
constant across frames. Also dump HEAD part2 (track volume/pan)."""
import os
import struct

import dsp_formula_probe as P

m = P.parse(P.FILE)
COEFS = m["chans"][0]["coefs"]
data = m["data"]

# --- HEAD part2: tracks ---
p2 = m["p2"]
ntracks = data[p2]
tdesc_type = data[p2 + 1]
print("== part2 ==")
print(" tracks:", ntracks, "desc type:", tdesc_type)
for t in range(ntracks):
    ent = p2 + 4 + 8 * t
    if data[ent] != 0x01:
        print("  t%d unexpected marker 0x%02X" % (t, data[ent]))
        continue
    toff = m["head_off"] + 8 + struct.unpack_from(">I", data, ent + 4)[0]
    if tdesc_type == 1:
        vol, pan = data[toff], data[toff + 1]
        nch, l, r = data[toff + 8], data[toff + 9], data[toff + 10]
        print("  t%d vol=%d pan=%d ch=%d left=%d right=%d" % (t, vol, pan, nch, l, r))
    else:
        nch, l, r = data[toff], data[toff + 1], data[toff + 2]
        print("  t%d (type0) ch=%d left=%d right=%d" % (t, nch, l, r))

# --- ADPC first entries ---
adpc = m["adpc_off"]
print("\n== ADPC ==")
print(" magic:", data[adpc:adpc + 4], "len:", struct.unpack_from(">I", data, adpc + 4)[0])
ent_bytes = m["ch"] * 4
nent = (m["adpc_size"] - 8) // ent_bytes
first = struct.unpack_from(">%dh" % m["ch"], data, adpc + 8)   # interleaved pairs per ch
print(" entries:", nent, " first entry (per-channel hist1,hist2):",
      [struct.unpack_from(">2h", data, adpc + 8 + e * 4) for e in range(m["ch"])])

# --- reference + nibbles ---
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


def sign4(n):
    return n - 16 if n > 7 else n


# residual per pred over frames 218..259
frames = list(range(218, 260))
nibs_all, ref_all, h1_all, h2_all = [], [], [], []
for f in frames:
    s0 = f * 14
    b = chunk[f * 8:(f + 1) * 8]
    ns = []
    for i in range(7):
        ns += [sign4(b[1 + i] >> 4), sign4(b[1 + i] & 0xF)]
    nibs_all.append(ns)
    ref_all.append(REF[s0:s0 + 14])
    h1_all.append(REF[s0 - 1])
    h2_all.append(REF[s0 - 2])

print("\n== residual vs nibble (ratio consistency) ==")
for pred in range(16):
    c1, c2 = COEFS[pred * 2], COEFS[pred * 2 + 1]
    ratios = []
    resid = []
    for k in range(len(frames)):
        h1, h2 = h1_all[k], h2_all[k]
        want = ref_all[k]
        for i in range(14):
            r = want[i] - ((c1 * h1 + c2 * h2) >> 15)
            resid.append(r)
            n = nibs_all[k][i]
            if n != 0 and r != 0:
                ratios.append(r / n)
            h2, h1 = h1, want[i]
        # note: history from REF chain (assuming REF is the model state)
    if not ratios:
        print(" pred %2d (c=%5d,%5d): no ratios" % (pred, c1, c2))
        continue
    lo, hi = min(ratios), max(ratios)
    med = sorted(ratios)[len(ratios) // 2]
    spread = hi - lo
    print(" pred %2d (c=%5d,%5d): ratios min=%8.3f med=%8.3f max=%8.3f spread=%8.3f n=%d"
          % (pred, c1, c2, lo, med, hi, spread, len(ratios)))
