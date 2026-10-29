"""Verify vgmstream's looped decode is sample-exact vs manual concatenation,
and measure the discontinuity at the loop wrap point."""
import os
import struct
import statistics
import tempfile

OUT = tempfile.mkdtemp(prefix="brstm_exp_")

LOOP_START = 5634048
TOTAL = 12786908
LOOP_END = TOTAL          # BRSTM loop end == total sample count
LOOP_LEN = LOOP_END - LOOP_START


def read_wav(path):
    with open(path, "rb") as fh:
        data = fh.read()
    assert data[0:4] == b"RIFF" and data[8:12] == b"WAVE"
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body, 0)
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    return fmt, pcm


fmt, full = read_wav(OUT + r"\uTTC_full.wav")
fmt2, looped = read_wav(OUT + r"\uTTC_loop2.wav")
rate = fmt[2]
print("rate:", rate, "channels:", fmt[1])
print("full samples/frame:", len(full) // 4, "looped samples/frame:", len(looped) // 4)

# Expected: looped = full + one pass of the loop region appended
expected_len = TOTAL + LOOP_LEN
print("expected looped samples:", expected_len, "actual:", len(looped) // 4)

seg_a = looped[TOTAL * 4:(TOTAL + LOOP_LEN) * 4]
seg_b = full[LOOP_START * 4:LOOP_END * 4]
print("appended pass == original loop region (byte-exact):", seg_a == seg_b)

# Discontinuity at the wrap: last sample of loop region vs first sample of loop start
def samples(buf, n):
    return struct.unpack_from("<%dh" % (len(buf) // 2), buf)


last = struct.unpack_from("<2h", full, (LOOP_END - 1) * 4)
first = struct.unpack_from("<2h", full, LOOP_START * 4)
jump = max(abs(last[i] - first[i]) for i in range(2))
print("wrap boundary: last=%s first=%s max jump=%d" % (last, first, jump))

# Typical adjacent-sample deltas in the same neighborhood (context for 'jump')
region = samples(full, 0)
base = LOOP_START * 2  # halfwords index (stereo interleaved)
deltas = []
for i in range(base, base + 2 * 44100, 2):
    deltas.append(abs(region[i] - region[i - 2]))
print("local median |delta|:", statistics.median(deltas),
      "p95:", sorted(deltas)[int(len(deltas) * 0.95)],
      "max:", max(deltas))
print("wrap jump / p95 delta:", jump / max(1, sorted(deltas)[int(len(deltas) * 0.95)]))
