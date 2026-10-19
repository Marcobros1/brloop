"""Verify ADPC table semantics on uTTC_N: entry b should equal the decoder
hist1/hist2 when entering block b (final two samples of block b-1)."""
import os
import struct

import dsp_formula_probe as P

m = P.parse(P.FILE)
data = m["data"]
ch = m["ch"]


def sign4(n):
    return n - 16 if n > 7 else n


def clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def decode_block(block, c, coefs, hist1, hist2, samples):
    ad = m["adpcm_start"]
    bs = m["block_size"]
    final = block == m["block_count"] - 1
    stride = m["final_size_pad"] if final else bs
    size = m["final_size_pad"] if final else bs
    off = ad + (block * bs * ch) + c * stride
    buf = src[off:off + size]
    out = []
    pos = 0
    done = 0
    while done < samples and pos < len(buf):
        hdr = buf[pos]; pos += 1
        pred, shift = hdr >> 4, hdr & 0xF
        c1, c2 = coefs[pred * 2], coefs[pred * 2 + 1]
        for i in range(14):
            if done >= samples:
                break
            byte = buf[pos + i // 2]
            n = (byte >> 4) if (i % 2 == 0) else (byte & 0xF)
            n = sign4(n)
            corrected = c1 * hist1 + c2 * hist2 + (n * ((1 << shift) * 2048))
            scaled = (corrected + 1024) >> 11
            s = clamp16(scaled)
            out.append(s)
            hist2, hist1 = hist1, s
            done += 1
        pos += 7
    return out, hist1, hist2


def block_samples(b):
    if b != m["block_count"] - 1:
        return m["spb"]
    return m["final_samples"]


src = data
entries = []
adpc = m["adpc_off"] + 8
nent = (m["adpc_size"] - 8) // (ch * 4)
for i in range(nent):
    e = struct.unpack_from(">%dh" % ch, data, adpc + i * ch * 4)
    entries.append(e)

coefs0 = m["chans"][0]["coefs"]
print("init state ch0:", m["chans"][0]["hist1"], m["chans"][0]["hist2"])
print("ADPC entry[0] ch0:", entries[0][0], entries[0][1])

h1, h2 = m["chans"][0]["hist1"], m["chans"][0]["hist2"]
mismatches = 0
for block in range(0, 13):
    ent = entries[block] if block < nent else None
    if ent is not None and (h1, h2) != (ent[0], ent[1]):
        print("block %4d: carried (%d,%d) != ADPC (%d,%d)" % (block, h1, h2, ent[0], ent[1]))
        mismatches += 1
    out, h1, h2 = decode_block(block, 0, coefs0, h1, h2, block_samples(block))
print("blocks 0..12: carried vs ADPC mismatches:", mismatches)

h1, h2 = m["chans"][0]["hist1"], m["chans"][0]["hist2"]
for b in range(0, 299):
    out, h1, h2 = decode_block(b, 0, coefs0, h1, h2, block_samples(b))
print("carried state entering block 299: (%d,%d); ADPC[299]=%s" % (h1, h2, entries[299]))
out_carry, _, _ = decode_block(299, 0, coefs0, h1, h2, block_samples(299))
out_adpc, _, _ = decode_block(299, 0, coefs0, entries[299][0], entries[299][1], block_samples(299))
print("block 299 decode carry vs adpc identical:", out_carry == out_adpc, "samples:", len(out_carry))
