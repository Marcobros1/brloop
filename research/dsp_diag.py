"""Diagnose the DSP frame formula: print reference vs candidate decodes."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from dsp_formula_probe import parse, FILE, REF_WAV, sign4, clamp16  # noqa: E402

m = parse(FILE)
c0 = m["chans"][0]

with open(REF_WAV, "rb") as fh:
    w = fh.read(4096)
pos = 12
while pos + 8 <= len(w):
    cid = w[pos:pos + 4]
    sz = struct.unpack_from("<I", w, pos + 4)[0]
    if cid == b"data":
        ref = w[pos + 8:pos + 8 + 4 * 40]
        break
    pos += 8 + sz + (sz & 1)
ref0 = [struct.unpack_from("<h", ref, i * 4)[0] for i in range(40)]

# raw first 32 bytes of ch0 block0
chunk = m["data"][m["adpcm_start"]:m["adpcm_start"] + m["block_size"]]
print("block0 ch0 first 32 bytes:", chunk[:32].hex())
print("init_ps=0x%04X hist1=%d hist2=%d" % (c0["init_ps"] & 0xFFFF, c0["hist1"], c0["hist2"]))
print("reference ch0 first 40:", ref0)


def decode(hdr_mode, nibble_order, coef_shift, n_from, limit):
    """hdr_mode: 'hi' pred=hi,shift=lo | 'lo' pred=lo,shift=hi
    n_from: 'frame' all frames use own header | 'init0' frame0 uses init byte
    returns list"""
    coefs = c0["coefs"]
    hist1, hist2 = c0["hist1"], c0["hist2"]
    out = []
    frames = m["block_size"] // 8
    for f in range(frames):
        b = chunk[f * 8:(f + 1) * 8]
        if f == 0 and n_from == "init0_hi":
            hdr = (c0["init_ps"] >> 8) & 0xFF
        elif f == 0 and n_from == "init0_lo":
            hdr = (c0["init_ps"] & 0xFF)
        else:
            hdr = b[0]
        if hdr_mode == "hi":
            pred, shift = hdr >> 4, hdr & 0xF
        else:
            pred, shift = hdr & 0xF, hdr >> 4
        c1, c2 = coefs[pred * 2], coefs[pred * 2 + 1]
        for i in range(7):
            byte = b[1 + i]
            ns = ((byte >> 4), (byte & 0xF)) if nibble_order == "hi" else ((byte & 0xF), (byte >> 4))
            for n in ns:
                n = sign4(n)
                val = (n << 12) >> shift
                a = (c1 * hist1 + c2 * hist2) >> coef_shift
                s = clamp16(val + a)
                out.append(s)
                hist2, hist1 = hist1, s
                if len(out) >= limit:
                    return out
    return out


for hdr_mode in ("hi", "lo"):
    for nib in ("hi", "lo"):
        for cs in (15, 16):
            for nf in ("frame", "init0_hi", "init0_lo"):
                got = decode(hdr_mode, nib, cs, nf, 40)
                mism = [i for i in range(40) if got[i] != ref0[i]]
                first = mism[0] if mism else -1
                print("hdr=%s nib=%s csh=%d nf=%-7s first_mismatch=%-3s n_mismatch=%d  first16 got=%s"
                      % (hdr_mode, nib, cs, nf, first, len(mism), got[:16]))
