"""Compare the PCM data of two WAV files byte-for-byte.

Usage: python compare_pcm.py <a.wav> <b.wav> [--max-bad-fraction F]

Both files must be RIFF/WAVE with a fmt chunk and a data chunk. The fmt
fields (channels, sample rate, bits per sample) must match exactly.

By default every byte of the data chunk must match. With --max-bad-fraction
F (e.g. 0.005) up to a fraction F of differing bytes is tolerated; used for
files whose ADPC table holds slightly off states, where per-block decoding
may differ from continuous decoding by a short transient at block boundaries
(see research/adpc_mismatch_probe.py). The first difference and totals are
always printed.

Exit codes: 0 within tolerance, 1 differences beyond tolerance, 2 usage/parse
error.
"""
import os
import struct
import sys


def parse_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 12 or data[0:4] != b'RIFF' or data[8:12] != b'WAVE':
        sys.exit('error: %s is not a RIFF/WAVE file' % path)
    fmt = None
    pcm = None
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csz = struct.unpack_from('<I', data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + csz]
        if cid == b'fmt ':
            if len(body) < 16:
                sys.exit('error: %s has a truncated fmt chunk' % path)
            fmt = {
                'format': struct.unpack_from('<H', body, 0)[0],
                'channels': struct.unpack_from('<H', body, 2)[0],
                'rate': struct.unpack_from('<I', body, 4)[0],
                'bits': struct.unpack_from('<H', body, 14)[0],
            }
        elif cid == b'data':
            pcm = body
        pos += 8 + csz + (csz & 1)
    if fmt is None or pcm is None:
        sys.exit('error: %s is missing fmt or data chunk' % path)
    return fmt, pcm


def main():
    args = sys.argv[1:]
    max_bad = 0.0
    if '--max-bad-fraction' in args:
        i = args.index('--max-bad-fraction')
        try:
            max_bad = float(args[i + 1])
        except (IndexError, ValueError):
            sys.exit('usage: compare_pcm.py <a.wav> <b.wav> '
                     '[--max-bad-fraction F]')
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit('usage: compare_pcm.py <a.wav> <b.wav> '
                 '[--max-bad-fraction F]')
    fa, pa = parse_wav(args[0])
    fb, pb = parse_wav(args[1])

    mismatches = 0
    for key in ('format', 'channels', 'rate', 'bits'):
        if fa[key] != fb[key]:
            print('FMT MISMATCH %s: %r vs %r' % (key, fa[key], fb[key]))
            mismatches += 1
    if mismatches:
        sys.exit(1)

    if len(pa) != len(pb):
        print('LENGTH MISMATCH: %d vs %d bytes' % (len(pa), len(pb)))
        sys.exit(1)
    first = None
    differing = 0
    for i in range(len(pa)):
        if pa[i] != pb[i]:
            if first is None:
                first = i
            differing += 1
    if first is None:
        print('PCM IDENTICAL: %d bytes, %d ch, %d Hz, %d-bit'
              % (len(pa), fa['channels'], fa['rate'], fa['bits']))
        sys.exit(0)

    frame = first // (2 * fa['channels'])
    ch = (first // 2) % fa['channels']
    frac = differing / float(len(pa)) if pa else 1.0
    print('FIRST DIFF at byte %d (frame %d, channel %d): %02X vs %02X'
          % (first, frame, ch, pa[first], pb[first]))
    print('DIFFERING BYTES: %d of %d (%.4f%%)'
          % (differing, len(pa), 100.0 * frac))
    if frac <= max_bad:
        print('WITHIN TOLERANCE: %.4f%% <= %.4f%%' % (100.0 * frac, 100.0 * max_bad))
        sys.exit(0)
    sys.exit(1)


if __name__ == '__main__':
    main()
