"""Three empirical checks needed before locking the C implementation:

1. Full VGAudio formula reproduces vgmstream samples byte-exactly on a
   non-silent span (frames 218..260, channel 0 of uTTC_N).
2. Maximum predictor index used across whole files (several files incl. 8ch):
   BRSTM channel info stores only 16 coefficient values = 8 predictor pairs,
   so predictors 0-7 are the addressable range.
3. Final block: nopad size >= ceil(samples/2) + ceil(samples/14) invariant
   (nibble bytes + frame headers) across all 301 collection files.
4. ADPC per-channel layout on a 4-channel file: entry = channels x (h1,h2),
   channel-interleaved.
"""
import os
import struct

import dsp_formula_probe as P

COL = os.environ.get("MKW_COLLECTION", os.path.join(os.environ["APPDATA"], "CT-MKWII", "Mods", "Metal Kart Wii Retro Rewind"))

# --- 1. exact formula on non-silent span -------------------------------
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


def decode_vgaudio(chunk, coefs, hist1, hist2, samples):
    out = []
    pos = 0
    done = 0
    while done < samples and pos < len(chunk):
        ps = chunk[pos]; pos += 1
        pred, scale = ps >> 4, ps & 0xF
        c1, c2 = coefs[pred * 2], coefs[pred * 2 + 1]
        dist_base = (1 << scale) * 2048
        for i in range(14):
            if done >= samples:
                break
            byte = chunk[pos + i // 2]
            n = (byte >> 4) if (i % 2 == 0) else (byte & 0xF)
            if n >= 8:
                n -= 16
            corrected = c1 * hist1 + c2 * hist2 + n * dist_base
            s = P.clamp16((corrected + 1024) >> 11)
            out.append(s)
            hist2, hist1 = hist1, s
            done += 1
        pos += 7
    return out, hist1, hist2


# decode frames 0..260, compare span 218..260 (real signal)
samples = 261 * 14
got, h1, h2 = decode_vgaudio(chunk, c0["coefs"], c0["hist1"], c0["hist2"], samples)
span = range(218 * 14, 260 * 14)
mismatch = sum(1 for i in span if got[i] != REF[i])
print("check1 formula exact on frames 218..260 (%d samples): mismatches=%d"
      % (len(list(span)), mismatch))

# --- 2. max predictor across files -------------------------------------
print("\ncheck2 max predictor index per file:")
for name in ("uTTC_N.brstm", "tKD_N.brstm", "STRM_N_KOOPA_N.brstm",
             "STRM_N_DESERT_N.brstm", "o_FanfareGP1_32.brstm", "104_N.brstm"):
    mm = P.parse(os.path.join(COL, name))
    data = mm["data"]
    bs, bc, ch = mm["block_size"], mm["block_count"], mm["ch"]
    ad = mm["adpcm_start"]
    maxpred = 0
    total = 0
    for b in range(bc):
        final = b == bc - 1 and bc > 1
        stride = mm["final_size_pad"] if final else bs
        size = mm["final_size_nopad"] if final else bs
        nsamp = mm["final_samples"] if final else mm["spb"]
        frames = (nsamp + 13) // 14
        for c in range(ch):
            blk = data[ad + b * bs * ch + c * stride:
                       ad + b * bs * ch + c * stride + size]
            for f in range(frames):
                if f * 8 < len(blk):
                    maxpred = max(maxpred, blk[f * 8] >> 4)
        total += nsamp * ch
    print("  %-24s maxpred=%d blocks=%d samples=%d" % (name, maxpred, bc, total))

# --- 3. final block nopad invariant over collection --------------------
print("\ncheck3 final-block nopad >= ceil(sp/2)+ceil(sp/14):")
bad = []
for name in sorted(os.listdir(COL)):
    if not name.lower().endswith(".brstm"):
        continue
    mm = P.parse(os.path.join(COL, name))
    if mm["block_count"] < 2:
        bad.append((name, "single block"))
        continue
    sp = mm["final_samples"]
    need = (sp + 1) // 2 + (sp + 13) // 14
    if mm["final_size_nopad"] < need:
        bad.append((name, sp, mm["final_size_nopad"], need))
print("  files violating invariant:", len(bad), bad[:5])

# --- 4. ADPC layout on 4ch file ----------------------------------------
print("\ncheck4 ADPC per-channel layout (STRM_N_DESERT_N, 4ch):")
mm = P.parse(os.path.join(COL, "STRM_N_DESERT_N.brstm"))
data = mm["data"]
ch = mm["ch"]
adpc = mm["adpc_off"] + 8
coefs0 = mm["chans"][0]["coefs"]
bs, bc = mm["block_size"], mm["block_count"]


def decode_block(mm, block, c, coefs, hist1, hist2):
    data = mm["data"]
    chn = mm["ch"]
    bs2 = mm["block_size"]
    final = block == mm["block_count"] - 1 and mm["block_count"] > 1
    stride = mm["final_size_pad"] if final else bs2
    size = mm["final_size_nopad"] if final else bs2
    nsamp = mm["final_samples"] if final else mm["spb"]
    off = mm["adpcm_start"] + block * bs2 * chn + c * stride
    buf = data[off:off + size]
    return decode_vgaudio(buf, coefs, hist1, hist2, nsamp)


# decode ch0 and ch3 of blocks 0..3, compare final states with ADPC entries
for c in (0, 3):
    coefs = mm["chans"][c]["coefs"]
    h1, h2 = mm["chans"][c]["hist1"], mm["chans"][c]["hist2"]
    ok = True
    for b in range(4):
        entry = struct.unpack_from(">2h", data, adpc + b * ch * 4 + c * 4)
        if (h1, h2) != entry:
            print("  ch%d block%d carried(%d,%d) != ADPC%s" % (c, b, h1, h2, entry))
            ok = False
        out, h1, h2 = decode_block(mm, b, c, coefs, h1, h2)
    print("  ch%d blocks0-3 state==ADPC: %s" % (c, ok))