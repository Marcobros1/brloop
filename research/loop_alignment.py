"""Check loop_start alignment vs ADPCM block granularity for all looped files."""
import os
import struct

COL = r"<personal-path> Kart Wii Retro Rewind"

aligned, misaligned, no_loop = [], [], []
samples = {}
for f in sorted(os.listdir(COL)):
    if not f.lower().endswith(".brstm"):
        continue
    with open(os.path.join(COL, f), "rb") as fh:
        head = fh.read(0x140)
    head_off = struct.unpack_from(">I", head, 0x10)[0]
    part1 = head_off + 0x08 + struct.unpack_from(">I", head, head_off + 0x0C)[0]
    loopflag = head[part1 + 1]
    loopstart = struct.unpack_from(">I", head, part1 + 8)[0]
    spb = struct.unpack_from(">I", head, part1 + 0x1C)[0]  # samples per block
    if loopflag == 0:
        no_loop.append(f)
        continue
    samples[f] = (loopstart, spb)
    (aligned if loopstart % spb == 0 else misaligned).append((f, loopstart, spb))

print("looped files:", len(samples), "| not looped:", len(no_loop), no_loop)
print("loop_start aligned to block start:", len(aligned))
print("loop_start inside a block:", len(misaligned))
for f, ls, spb in misaligned[:20]:
    print("   %-28s loopstart=%d spb=%d rem=%d" % (f, ls, spb, ls % spb))
rems = {}
for f, (ls, spb) in samples.items():
    rems[ls % spb] = rems.get(ls % spb, 0) + 1
print("remainder distribution (top):", sorted(rems.items(), key=lambda kv: -kv[1])[:8])
