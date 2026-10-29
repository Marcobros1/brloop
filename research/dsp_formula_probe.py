"""Dump full BRSTM header structures (uTTC_N) and brute-force the DSP-ADPCM
frame formula against vgmstream's reference decode (uTTC_full.wav, block 0).

Doc source: wiibrew.org/wiki/BRSTM_file (fetched this session). The frame
layout nibble/header conventions are verified empirically here.
"""
import os
import struct
import sys
import tempfile

COL = r"<personal-path> Kart Wii Retro Rewind"
FILE = os.path.join(COL, "uTTC_N.brstm")
REF_WAV = os.path.join(tempfile.mkdtemp(prefix="brstm_exp_"), "uTTC_full.wav")


def parse(path):
    with open(path, "rb") as fh:
        data = fh.read()
    m = {}
    m["data"] = data
    assert data[0:4] == b"RSTM"
    m["bom"] = data[4:6].hex()
    m["ver"] = (data[6], data[7])
    m["fsize"] = struct.unpack_from(">I", data, 0x08)[0]
    m["hsize"] = struct.unpack_from(">H", data, 0x0C)[0]
    m["nchunks"] = struct.unpack_from(">H", data, 0x0E)[0]
    m["head_off"], m["head_size"] = struct.unpack_from(">II", data, 0x10)
    m["adpc_off"], m["adpc_size"] = struct.unpack_from(">II", data, 0x18)
    m["data_off"], m["data_size"] = struct.unpack_from(">II", data, 0x20)

    h = m["head_off"]
    assert data[h:h + 4] == b"HEAD"
    m["head_len"] = struct.unpack_from(">I", data, h + 4)[0]
    p1 = h + 8 + struct.unpack_from(">I", data, h + 0x0C)[0]
    p2 = h + 8 + struct.unpack_from(">I", data, h + 0x14)[0]
    p3 = h + 8 + struct.unpack_from(">I", data, h + 0x1C)[0]
    m["p1"], m["p2"], m["p3"] = p1, p2, p3

    def p1u32(o): return struct.unpack_from(">I", data, p1 + o)[0]
    m["codec"] = data[p1]
    m["loopflag"] = data[p1 + 1]
    m["ch"] = data[p1 + 2]
    m["rate"] = struct.unpack_from(">H", data, p1 + 4)[0]
    m["loopstart"] = p1u32(0x08)
    m["total"] = p1u32(0x0C)
    m["adpcm_start"] = p1u32(0x10)
    m["block_count"] = p1u32(0x14)
    m["block_size"] = p1u32(0x18)
    m["spb"] = p1u32(0x1C)
    m["final_size_nopad"] = p1u32(0x20)
    m["final_samples"] = p1u32(0x24)
    m["final_size_pad"] = p1u32(0x28)
    m["adpc_samples"] = p1u32(0x2C)
    m["adpc_entry_bytes"] = p1u32(0x30)

    # part 3: per-channel ADPCM info
    n3 = data[p3]
    m["p3_ch"] = n3
    chans = []
    for c in range(n3):
        ent = p3 + 4 + 8 * c
        off = h + 8 + struct.unpack_from(">I", data, ent + 4)[0]
        assert data[off:off + 4] == b"\x01\x00\x00\x00", "channel marker"
        coef_ptr_field = struct.unpack_from(">I", data, off + 4)[0]
        coefs = struct.unpack_from(">" + "h" * 16, data, off + 8)
        gain, inits, hist1, hist2, loops, lhist1, lhist2 = struct.unpack_from(
            ">7h", data, off + 0x28)
        chans.append(dict(info_off=off, coef_field=coef_ptr_field, coefs=coefs,
                          gain=gain, init_ps=inits, hist1=hist1, hist2=hist2,
                          loop_ps=loops, lhist1=lhist1, lhist2=lhist2))
    m["chans"] = chans
    return m


m = parse(FILE)
print("== header dump uTTC_N ==")
for k in ("bom", "ver", "fsize", "hsize", "nchunks", "head_off", "head_size",
          "adpc_off", "adpc_size", "data_off", "data_size", "head_len",
          "p1", "p2", "p3"):
    print(" %-14s %s (0x%X)" % (k, m[k], m[k] if isinstance(m[k], int) else 0))
for k in ("codec", "loopflag", "ch", "rate", "loopstart", "total", "adpcm_start",
          "block_count", "block_size", "spb", "final_size_nopad", "final_samples",
          "final_size_pad", "adpc_samples", "adpc_entry_bytes", "p3_ch"):
    print(" %-14s %s (0x%X)" % (k, m[k], m[k]))
c0 = m["chans"][0]
print(" ch0: coef_field=0x%X gain=%d init_ps=0x%04X hist=%d,%d loop_ps=0x%04X lhist=%d,%d" %
      (c0["coef_field"], c0["gain"], c0["init_ps"] & 0xFFFF, c0["hist1"], c0["hist2"],
       c0["loop_ps"] & 0xFFFF, c0["lhist1"], c0["lhist2"]))
print(" ch0 coefs:", c0["coefs"])
print(" all channel coef tables equal:",
      all(ch["coefs"] == c0["coefs"] for ch in m["chans"]))

# layout math
need = m["adpcm_start"] + (m["block_count"] - 1) * m["block_size"] * m["ch"] \
    + m["ch"] * m["final_size_pad"]
print("\n== layout ==")
print(" expected end (padded final): 0x%X  file size: 0x%X  diff: %d" %
      (need, len(m["data"]), len(m["data"]) - need))
need2 = m["adpcm_start"] + (m["block_count"] - 1) * m["block_size"] * m["ch"] \
    + m["ch"] * m["final_size_nopad"]
print(" expected end (unpadded final): 0x%X  diff: %d" % (need2, len(m["data"]) - need2))
print(" DATA chunk end: 0x%X  ADPC end: 0x%X" %
      (m["data_off"] + m["data_size"], m["adpc_off"] + m["adpc_size"]))
print(" total blocks: %d -> full-1 blocks of %d spb + final %d samples = %d (total=%d)" %
      (m["block_count"], m["spb"], m["final_samples"],
       (m["block_count"] - 1) * m["spb"] + m["final_samples"], m["total"]))
adpc_entries = (m["adpc_size"] - 8) // (m["ch"] * 4)
print(" ADPC entries: %d (entry_bytes=%d, samples/entry=%d) vs block_count=%d" %
      (adpc_entries, m["adpc_entry_bytes"], m["adpc_samples"], m["block_count"]))

# ---- reference samples from vgmstream ----
with open(REF_WAV, "rb") as fh:
    w = fh.read(4 * 22000 + 4096)
pos = 12
while pos + 8 <= len(w):
    cid = w[pos:pos + 4]
    sz = struct.unpack_from("<I", w, pos + 4)[0]
    if cid == b"data":
        ref = w[pos + 8:pos + 8 + min(sz, 4 * 22000)]
        break
    pos += 8 + sz + (sz & 1)
NFRAMES = len(ref) // (2 * m["ch"])
print("\nreference frames loaded:", NFRAMES)


def sign4(n):
    return n - 16 if n > 7 else n


def clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def decode_block_variant(src, ch_index, init_ps, hist1, hist2, coefs,
                         nibble_hi_first, pred_is_hi, use_frame0_header,
                         coef_shift, nibble_shift_mode):
    """Decode one channel block; returns list of samples."""
    bs = m["block_size"]
    inter = bs
    chunk = src[m["adpcm_start"] + ch_index * inter:
                m["adpcm_start"] + ch_index * inter + bs]
    samples = []
    if not use_frame0_header:
        hdr = (init_ps >> 8) & 0xFF
        f0 = 0
    else:
        f0 = -1  # marker: first frame uses its own header (default anyway)
    frames = bs // 8
    for f in range(frames):
        b = chunk[f * 8:(f + 1) * 8]
        hdr = b[0]
        pred = (hdr >> 4) if pred_is_hi else (hdr & 0xF)
        shift = (hdr & 0xF) if pred_is_hi else (hdr >> 4)
        if pred > 15:
            pred = 15
        c1, c2 = coefs[pred * 2], coefs[pred * 2 + 1]
        for i in range(7):
            byte = b[1 + i]
            if nibble_hi_first:
                ns = (byte >> 4, byte & 0xF)
            else:
                ns = (byte & 0xF, byte >> 4)
            for n in ns:
                n = sign4(n)
                if nibble_shift_mode == "sh12":
                    val = (n << 12) >> shift
                else:
                    val = (n * (1 << (12 - shift))) if (12 - shift) >= 0 else 0
                a = (c1 * hist1 + c2 * hist2) >> coef_shift
                s = clamp16(val + a)
                samples.append(s)
                hist2, hist1 = hist1, s
    return samples


# brute force over documented/likely conventions
best = []
for nhf in (True, False):
    for phi in (True, False):
        for uf0 in (True, False):
            for csh in (15, 14, 16):
                for nmode in ("sh12", "mul"):
                    got = decode_block_variant(
                        m["data"], 0, c0["init_ps"], c0["hist1"], c0["hist2"],
                        c0["coefs"], nhf, phi, uf0, csh, nmode)
                    # compare ch0 (even samples of WAV)
                    ref0 = [struct.unpack_from("<h", ref, i * 4)[0]
                            for i in range(min(len(got), NFRAMES))]
                    match = sum(1 for i in range(len(ref0)) if got[i] == ref0[i])
                    best.append((match, dict(nibble_hi_first=nhf, pred_is_hi=phi,
                                             use_frame0=uf0, coef_shift=csh,
                                             nibble_mode=nmode)))
best.sort(key=lambda x: -x[0])
print("\ntop variants (matched first %d samples of ch0):" % len(ref0))
for match, v in best[:6]:
    print("  %5d/%d  %s" % (match, len(ref0), v))
