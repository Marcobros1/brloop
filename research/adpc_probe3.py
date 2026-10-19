"""Two follow-ups on the zeroed-ADPC finding:

A. HEAD part 3 loop fields (loop predictor/scale + loop hist1/hist2) should
   give the decoder state at loop_start (block-aligned in all looped files).
   Verify on uTTC_N and STRM_N_DESERT_N against carried decode.

B. Count collection files whose ADPC table is entirely zero (or shorter than
   block_count+1 entries) -> ADPC cannot be trusted as universal seek source.
"""
import os
import struct

import dsp_formula_probe as P
from dsp_final_checks import decode_vgaudio, decode_block

COL = r"<personal-path> Kart Wii Retro Rewind"

# --- A. loop state from part 3 -----------------------------------------
for name in ("uTTC_N.brstm", "STRM_N_DESERT_N.brstm", "tKD_N.brstm"):
    mm = P.parse(os.path.join(COL, name))
    data = mm["data"]
    ch = mm["ch"]
    ls = mm["loopstart"]
    spb = mm["spb"]
    loop_block = ls // spb
    aligned = (ls % spb == 0)
    print("\n%s: loop_start=%d spb=%d -> block %d aligned=%s"
          % (name, ls, spb, loop_block, aligned))
    for c in range(min(ch, 2)):
        coefs = mm["chans"][c]["coefs"]
        h1, h2 = mm["chans"][c]["hist1"], mm["chans"][c]["hist2"]
        for b in range(loop_block):
            out, h1, h2 = decode_block(mm, b, c, coefs, h1, h2)
        ci = mm["chans"][c]
        print("  ch%d carried@loop=(%d,%d) part3 loop_hist=(%d,%d) loop_ps=0x%04X %s"
              % (c, h1, h2, ci["lhist1"], ci["lhist2"], ci["loop_ps"] & 0xFFFF,
                 "MATCH" if (h1, h2) == (ci["lhist1"], ci["lhist2"]) else "MISMATCH"))

# --- B. collection-wide ADPC status ------------------------------------
print("\ncollection ADPC status:")
zeroed, short, ok, noloop = [], [], [], 0
for name in sorted(os.listdir(COL)):
    if not name.lower().endswith(".brstm"):
        continue
    mm = P.parse(os.path.join(COL, name))
    ch, bc = mm["ch"], mm["block_count"]
    adpc = mm["adpc_off"] + 8
    nent = (mm["adpc_size"] - 8) // (ch * 4) if mm["adpc_size"] >= 8 else 0
    if nent < bc + 1:
        short.append((name, nent, bc))
    # check whether all entries are zero
    entries = [struct.unpack_from(">%dh" % ch, mm["data"], adpc + i * ch * 4)
               for i in range(min(nent, 400))]
    if all(all(v == 0 for v in e) for e in entries):
        zeroed.append(name)
    else:
        ok.append(name)
print("  files:", len(ok) + len(zeroed) + len(short))
print("  ADPC has real data:", len(ok))
print("  ADPC fully zeroed (first 400 entries):", len(zeroed))
for z in zeroed:
    print("    -", z)
print("  ADPC shorter than block_count+1:", len(short))
for s in short[:10]:
    print("    -", s)
