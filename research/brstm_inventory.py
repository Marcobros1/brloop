"""Inventory of the reference BRSTM collection (read-only).

Parses the top-level RSTM header and HEAD chunk part 1 (per wiibrew docs,
verified empirically) and reports codec/channels/rate/loop metadata for every
file, plus a paired comparison of *_f / *_n variants.
"""
import os
import struct
import sys

COL = r"<personal-path> Kart Wii Retro Rewind"


def parse(path):
    with open(path, "rb") as fh:
        head = fh.read(0x140)
        size = fh.seek(0, 2)
    if head[0:4] != b"RSTM":
        return {"error": "bad magic %r" % head[0:4]}
    version = (head[6], head[7])
    declared_size = struct.unpack_from(">I", head, 0x08)[0]
    n_chunks = struct.unpack_from(">H", head, 0x0E)[0]
    head_off = struct.unpack_from(">I", head, 0x10)[0]
    # HEAD chunk part 1: offsets are relative to HEAD+0x08
    part1 = head_off + 0x08 + struct.unpack_from(">I", head, head_off + 0x0C)[0]
    codec = head[part1]
    loopflag = head[part1 + 1]
    channels = head[part1 + 2]
    rate = struct.unpack_from(">H", head, part1 + 4)[0]
    loopstart = struct.unpack_from(">I", head, part1 + 8)[0]
    total = struct.unpack_from(">I", head, part1 + 0x0C)[0]
    return dict(version=version, declared_size=declared_size, real_size=size,
                n_chunks=n_chunks, codec=codec, loopflag=loopflag,
                channels=channels, rate=rate, loopstart=loopstart, total=total)


files = sorted(f for f in os.listdir(COL) if f.lower().endswith(".brstm"))
rows, errors = {}, []
for f in files:
    try:
        rows[f] = parse(os.path.join(COL, f))
    except Exception as exc:  # report, don't hide
        errors.append((f, str(exc)))

print("total brstm files:", len(files), " parse errors:", len(errors))
for f, e in errors:
    print("  ERROR", f, e)

bad_magic = [f for f, r in rows.items() if "error" in r]
print("bad magic:", bad_magic)

no_loop = [f for f, r in rows.items() if "loopflag" in r and r["loopflag"] == 0]
print("\nfiles with loop flag = 0 (no loop):", len(no_loop))
for f in no_loop:
    print("  ", f)

sizes = [r for r in rows.values() if "real_size" in r]
mismatch = [(f, r) for f, r in rows.items()
            if "real_size" in r and r["declared_size"] != r["real_size"]]
print("\nheader-declared size != real size:", len(mismatch))
for f, r in mismatch:
    print("   %s declared=%d real=%d diff=%d" %
          (f, r["declared_size"], r["real_size"], r["declared_size"] - r["real_size"]))

rates = {}
for f, r in rows.items():
    if "rate" in r:
        rates.setdefault(r["rate"], []).append(f)
print("\nsample-rate distribution:")
for rate in sorted(rates):
    print("  %6d Hz : %3d files  e.g. %s" %
          (rate, len(rates[rate]), ", ".join(rates[rate][:3])))

codecs = {}
for r in rows.values():
    if "codec" in r:
        codecs.setdefault((r["codec"], r["channels"]), 0)
        codecs[(r["codec"], r["channels"])] += 1
print("\n(codec, channels) distribution:", dict(sorted(codecs.items())))

# Pair _f / _n variants (final-lap vs normal)
print("\n_f / _n pair comparison:")
names = set(files)
low = {f.lower(): f for f in files}
seen = set()
for f in files:
    fl = f.lower()
    if not (fl.endswith("_f.brstm") or fl.endswith("_f_n.brstm")):
        continue
    base = fl[:-len("_f.brstm")]
    cand = None
    for suffix in ("_n.brstm", ".brstm"):
        c = base + suffix
        if c in low:
            cand = low[c]
            break
    if not cand or cand in seen:
        continue
    seen.add(cand)
    rf, rn = rows[f], rows[cand]
    if "rate" in rf and "rate" in rn and rn["rate"]:
        ratio = rf["rate"] / rn["rate"]
        same_audio_info = (rf["total"] == rn["total"] and rf["loopstart"] == rn["loopstart"]
                           and rf["channels"] == rn["channels"])
        print("  %-28s rate %5d -> %5d  ratio %.6f  same_loop/total=%s" %
              (base, rn["rate"], rf["rate"], ratio, same_audio_info))
    else:
        print("  %-28s (missing data)" % base)
