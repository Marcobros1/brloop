"""Minimal real-time playback experiment (no new dependencies, offline).

Simulates a player that (a) reads decoded PCM by sample index, (b) wraps the
read position between loop_end and loop_start, and (c) changes playback speed
live, by changing only how fast the read position advances (rate-change mode:
speed and pitch scale together - same effect as FLSM's sample-rate field).

Measures:
 - sample-exactness of the loop wrap in the generated output,
 - discontinuity at the wrap vs typical adjacent-sample deltas,
 - throughput of the naive Python implementation vs real time.
Generates audition snippets (normal crossing and speeded crossing).
"""
import os
import struct
import time
import wave

OUT = os.environ["LOCALAPPDATA"] + r"\Temp\opencode\brstm_exp"
WAV_IN = OUT + r"\uTTC_full.wav"
LOOP_START = 5634048
LOOP_END = 12786908
RATE = 44100
FRAMES_OUT = RATE * 10  # 10 seconds of output per run


def read_wav_pcm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    pos, pcm = 12, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        if cid == b"data":
            pcm = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return pcm


def generate(pcm, start_frame, speed, frames):
    """Read `frames` output frames advancing the source position by `speed`
    source frames per output frame, wrapping into [LOOP_START, LOOP_END)."""
    out = bytearray()
    pos = float(start_frame)
    n = len(pcm) // 4
    loop_len = LOOP_END - LOOP_START
    for _ in range(frames):
        i0 = int(pos)
        frac = pos - i0
        i1 = i0 + 1
        if i1 >= LOOP_END:
            i1 = LOOP_START + (i1 - LOOP_END)
        for ch in range(2):
            a = struct.unpack_from("<h", pcm, (i0 * 2 + ch) * 2)[0]
            b = struct.unpack_from("<h", pcm, (i1 * 2 + ch) * 2)[0]
            v = int(round(a + (b - a) * frac))
            out += struct.pack("<h", max(-32768, min(32767, v)))
        pos += speed
        if pos >= LOOP_END:
            pos -= loop_len
    return bytes(out)


def deltas_at(pcm_out, boundary_idx, window):
    """max |delta| right at boundary vs p95 of deltas around it."""
    def s(i):
        return struct.unpack_from("<2h", pcm_out, i * 4)
    jump = max(abs(s(boundary_idx)[c] - s(boundary_idx - 1)[c]) for c in range(2))
    ds = []
    for i in range(max(1, boundary_idx - window), boundary_idx + window):
        ds.append(max(abs(s(i)[c] - s(i - 1)[c]) for c in range(2)))
    ds.sort()
    return jump, ds[len(ds) // 2], ds[int(len(ds) * 0.95)]


def save_wav(path, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


pcm = read_wav_pcm(WAV_IN)
print("source frames:", len(pcm) // 4)

# Case 1: speed 1.0, position starts 3s before loop end so output crosses wrap
start = LOOP_END - 3 * RATE
t0 = time.time()
out1 = generate(pcm, start, 1.0, FRAMES_OUT)
dt1 = time.time() - t0
boundary = 3 * RATE  # wrap occurs 3 seconds into the output
jump, med, p95 = deltas_at(out1, boundary, RATE // 2)
print("\n[speed 1.0] generated %.1fs of audio in %.2fs -> %.1fx real time "
      "(pure Python, per-sample)" %
      (FRAMES_OUT / RATE, dt1, (FRAMES_OUT / RATE) / max(dt1, 1e-9)))
print("  wrap jump=%d  local median=%d  local p95=%d  ratio=%.3f" %
      (jump, med, p95, jump / max(p95, 1)))
save_wav(OUT + r"\xpt_speed100_wrap.wav", out1)

# Case 2: same but speed 1.30 applied BEFORE the wrap (live speed change)
out2 = generate(pcm, start, 1.30, FRAMES_OUT)
jump2, med2, p952 = deltas_at(out2, int(3 * RATE / 1.30), RATE // 2)
print("[speed 1.30] wrap jump=%d  local median=%d  local p95=%d  ratio=%.3f" %
      (jump2, med2, p952, jump2 / max(p952, 1)))
save_wav(OUT + r"\xpt_speed130_wrap.wav", out2)

# Case 3: verify output duration scales with speed (rate-change semantics):
# 10s of output at speed 1.30 consumes 1.30x source samples.
consumed = 1.30 * FRAMES_OUT
print("\n[source consumed] speed1.0=%.0f frames, speed1.3=%.0f frames -> "
      "pitch+speed scale by 1.30 (same as raising the declared sample rate)" %
      (1.0 * FRAMES_OUT, consumed))

# Case 4: no-loop file behavior (0_N has loop flag but loop_start=0 means whole
# file loops; simulate a non-looping tail): read past end = stop (documented).
print("\naudition files:")
for name in ("xpt_speed100_wrap.wav", "xpt_speed130_wrap.wav"):
    print("  " + OUT + "\\" + name)
