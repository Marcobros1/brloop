"""Why does per-block decode from ADPC differ from carried decode in
n_rainbow32_f.brstm and o_Wi-Fi_waiting32.brstm?

Decodes the first blocks of channel 0 with the carried state and compares the
state entering each block against the ADPC table entries, testing the
hypotheses: entry[b] equals the true state (as verified on uTTC/tKD), the
table shifted by one entry, or hist1/hist2 swapped.
"""
import os
import struct
import sys

COL = os.environ.get("MKW_COLLECTION", os.path.join(os.environ["APPDATA"], "CT-MKWII", "Mods", "Metal Kart Wii Retro Rewind"))
N_BLOCKS = 6


def be16(d, o):
    return struct.unpack_from('>h', d, o)[0]


def be32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


def decode_block(data, coefs, hist1, hist2, nsamples):
    """Decode one channel block; returns (samples, hist1, hist2)."""
    out = []
    pos = 0
    produced = 0
    while produced < nsamples:
        header = data[pos]
        pos += 1
        scale = header & 0x0F
        pred = header >> 4
        if pred > 7:
            pred = 7
        c1, c2 = coefs[pred]
        dist = 1 << (11 + scale)
        frame_samples = min(14, nsamples - produced)
        nibble_bytes = (frame_samples + 1) // 2
        for i in range(frame_samples):
            byte = data[pos + i // 2]
            nib = (byte >> 4) if i % 2 == 0 else (byte & 0x0F)
            if nib >= 8:
                nib -= 16
            s = clamp16((c1 * hist1 + c2 * hist2 + nib * dist + 1024) >> 11)
            out.append(s)
            hist2, hist1 = hist1, s
        pos += nibble_bytes
        produced += frame_samples
    return out, hist1, hist2


def probe(name):
    path = os.path.join(COL, name)
    d = open(path, 'rb').read()
    head_off = be32(d, 0x10)
    adpc_off, adpc_size = struct.unpack_from('>II', d, 0x18)
    data_off = be32(d, 0x20)
    rel1 = be32(d, head_off + 0x0C)
    rel3 = be32(d, head_off + 0x1C)
    p1 = head_off + 8 + rel1
    p3 = head_off + 8 + rel3
    ch = d[p1 + 2]
    adpcm_start = be32(d, p1 + 0x10)
    bc, bs, spb = struct.unpack_from('>III', d, p1 + 0x14)
    nopad, finsamp, pad = struct.unpack_from('>III', d, p1 + 0x20)

    c0 = 0  # channel 0
    ent = p3 + 4 + 8 * c0
    coff = be32(d, ent + 4)
    cabs = head_off + 8 + coff
    coefs = [(be16(d, cabs + 8 + 4 * k), be16(d, cabs + 8 + 4 * k + 2))
             for k in range(8)]
    init_h1 = be16(d, cabs + 0x2C)
    init_h2 = be16(d, cabs + 0x2E)

    def entry(b):
        o = adpc_off + 8 + b * ch * 4 + c0 * 4
        return be16(d, o), be16(d, o + 2)

    print('=== %s (ch=%d blocks=%d spb=%d) ===' % (name, ch, bc, spb))
    h1, h2 = init_h1, init_h2
    carried = []
    for b in range(N_BLOCKS):
        carried.append((h1, h2))
        stride = pad if (b == bc - 1 and bc > 1) else bs
        size = nopad if (b == bc - 1 and bc > 1) else bs
        frames = finsamp if (b == bc - 1 and bc > 1) else spb
        off = adpcm_start + b * bs * ch + c0 * stride
        _, h1, h2 = decode_block(d[off:off + size], coefs, h1, h2, frames)

    for b in range(N_BLOCKS):
        e = entry(b)
        c = carried[b]
        flags = []
        if e == c:
            flags.append('MATCH')
        if b + 1 < N_BLOCKS and entry(b + 1) == c:
            flags.append('entry[b+1]==carried[b]')
        if b > 0 and entry(b - 1) == c:
            flags.append('entry[b-1]==carried[b]')
        if (e[1], e[0]) == c:
            flags.append('SWAPPED')
        print('  block %d: carried=%s adpc=%s %s'
              % (b, c, e, ','.join(flags) or 'NO MATCH'))


for n in ('n_rainbow32_f.brstm', 'o_Wi-Fi_waiting32.brstm',
          'uTTC_N.brstm', 'tKD_N.brstm'):
    probe(n)