"""Inspect the ADPC table of STRM_N_DESERT_N (4ch): is it zeroed, or mapped
differently? Compare against carried decoder states for several blocks."""
import os
import struct

import dsp_formula_probe as P
from dsp_final_checks import decode_vgaudio, decode_block

COL = r"<personal-path> Kart Wii Retro Rewind"
mm = P.parse(os.path.join(COL, "STRM_N_DESERT_N.brstm"))
data = mm["data"]
ch = mm["ch"]
adpc = mm["adpc_off"] + 8
nent = (mm["adpc_size"] - 8) // (ch * 4)
print("adpc_size=0x%X entries=%d blocks=%d channels=%d"
      % (mm["adpc_size"], nent, mm["block_count"], ch))

entries = []
for i in range(nent):
    entries.append(struct.unpack_from(">%dh" % ch, data, adpc + i * ch * 4))

nonzero = [i for i, e in enumerate(entries) if any(v != 0 for v in e)]
print("non-zero entries:", len(nonzero), "first:", nonzero[:10])

# carried states for ch0 at blocks 0..8
c = 0
coefs = mm["chans"][c]["coefs"]
h1, h2 = mm["chans"][c]["hist1"], mm["chans"][c]["hist2"]
print("\nch0 carried vs ADPC per block:")
for b in range(10):
    e = struct.unpack_from(">2h", data, adpc + b * ch * 4 + c * 4)
    print("  block %3d: carried=(%6d,%6d) adpc=(%6d,%6d) %s"
          % (b, h1, h2, e[0], e[1], "==" if (h1, h2) == e else "!="))
    out, h1, h2 = decode_block(mm, b, c, coefs, h1, h2)

# maybe ADPC entries index every samples_per_adpc_entry samples, not blocks?
print("\nadpc_samples_per_entry=%d spb=%d" % (mm["adpc_samples"], mm["spb"]))
