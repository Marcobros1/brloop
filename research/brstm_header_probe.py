"""Empirical probe of BRSTM top-level and HEAD chunk fields (read-only).

Locates known values reported by vgmstream (sample rate, loop start, total
samples) inside the file header to determine their exact offsets.
"""
import struct
import sys

COL = r"<personal-path> Kart Wii Retro Rewind"

TESTS = {
    "uTTC_N.brstm": {"sr": 44100, "loopstart": 5634048, "total": 12786908},
    "0_N.brstm": {"sr": 44100, "loopstart": 0, "total": 8228864},
    "o_FanfareGP1_32.brstm": {"sr": 48000, "loopstart": 860160, "total": 1303244,
                              "sr_alt": 48000},
    "n_rainbow32_n.brstm": {"sr": 48000, "loopstart": 0, "total": 8571425},
    "sw2RR_N.brstm": {"sr": 44100, "loopstart": 2250752, "total": 10658184},
}


def find_all(hay, pat):
    out = []
    i = hay.find(pat)
    while i >= 0:
        out.append(i)
        i = hay.find(pat, i + 1)
    return out


for name, vals in TESTS.items():
    with open(COL + "\\" + name, "rb") as fh:
        d = fh.read(0x300)
    print("==", name, "size", len(open(COL + "\\" + name, "rb").read()))
    for off in range(0, 0x80, 16):
        print("  %04X" % off, d[off:off + 16].hex(" "))
    for k, v in vals.items():
        v = int(v)
        pats = [("BE32", struct.pack(">I", v)), ("LE32", struct.pack("<I", v))]
        if v < 65536:
            pats += [("BE16", struct.pack(">H", v)), ("LE16", struct.pack("<H", v))]
        for tag, pat in pats:
            hits = [hex(i) for i in find_all(d, pat)]
            if hits:
                print("   ", k, v, tag, "->", hits)
