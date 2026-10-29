"""Compare the two decoded WAVs (normal vs 'final lap' variant)."""
import struct
import os
import tempfile

OUT = tempfile.mkdtemp(prefix="brstm_exp_")


def read_wav(path):
    with open(path, "rb") as fh:
        data = fh.read()
    assert data[0:4] == b"RIFF" and data[8:12] == b"WAVE", "not a wav"
    pos = 12
    fmt = None
    pcm = None
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


fmt_n, pcm_n = read_wav(OUT + r"\n_rainbow32_n.wav")
fmt_f, pcm_f = read_wav(OUT + r"\n_rainbow32_f.wav")
print("fmt _n:", fmt_n)
print("fmt _f:", fmt_f)
print("pcm sizes:", len(pcm_n), len(pcm_f))
print("PCM byte-identical:", pcm_n == pcm_f)
rate_n, rate_f = fmt_n[2], fmt_f[2]
print("rate _n:", rate_n, "rate _f:", rate_f)
print("speed/pitch ratio _f/_n:", rate_f / rate_n)
print("duration _n s:", len(pcm_n) / (rate_n * 4), "duration _f s:", len(pcm_f) / (rate_f * 4))
