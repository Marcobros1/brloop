"""Create mutated BRSTM files that must be rejected with controlled errors.

Usage: python make_invalid_cases.py <base.brstm> <outdir>

Writes one mutant per case plus manifest.json describing, for each case, the
expected brstm_info/brstm_decode exit code and a keyword that must appear in
the error message. A valid unmodified copy is included as a positive control.

The mutants are produced by parsing the real offsets of the base file and
patching individual fields; nothing is assumed about the collection layout
beyond what the reader itself validates.
"""
import json
import os
import struct
import sys

BIG = 0x7FFFFFFF  # clearly out-of-bounds offset value


def be32(data, off):
    return struct.unpack_from('>I', data, off)[0]


def set_be32(data, off, value):
    struct.pack_into('>I', data, off, value & 0xFFFFFFFF)


def set_be16(data, off, value):
    struct.pack_into('>H', data, off, value & 0xFFFF)


def parse_base(data):
    head_off = be32(data, 0x10)
    adpc_off = be32(data, 0x18)
    data_off = be32(data, 0x20)
    rel1 = be32(data, head_off + 0x0C)
    rel3 = be32(data, head_off + 0x1C)
    part1 = head_off + 8 + rel1
    part3 = head_off + 8 + rel3
    return {
        'head_off': head_off,
        'adpc_off': adpc_off,
        'data_off': data_off,
        'part1': part1,
        'part3': part3,
        'channels': data[part1 + 0x02],
        'block_size': be32(data, part1 + 0x18),
        'samples_per_block': be32(data, part1 + 0x1C),
        'final_nopad': be32(data, part1 + 0x20),
        'final_samples': be32(data, part1 + 0x24),
        'total_samples': be32(data, part1 + 0x0C),
        'size': len(data),
    }


def build_cases(data, base):
    """Return list of (name, expected_exit, keyword, description, mutate)."""
    p1 = base['part1']
    p3 = base['part3']
    ch0_info = base['head_off'] + 8 + be32(data, p3 + 4 + 4)
    need_final = (base['final_samples'] + 1) // 2 + (base['final_samples'] + 13) // 14

    def patch(offset, raw):
        return lambda d: d.__setitem__(slice(offset, offset + len(raw)), raw)

    def u32(offset, value):
        return lambda d: set_be32(d, offset, value)

    def u16(offset, value):
        return lambda d: set_be16(d, offset, value)

    def u8(offset, value):
        return lambda d: d.__setitem__(offset, value)

    cases = [
        ('control_valid', 0, '', 'unmodified copy must parse',
         lambda d: None),
        ('bad_magic', 2, "expected 'RSTM'", 'RSTM magic replaced',
         patch(0, b'XSTM')),
        ('bad_bom', 2, 'byte order mark', 'byte-order mark byte-swapped',
         patch(4, b'\xFF\xFE')),
        ('header_size_small', 2, 'header size', 'header size below 0x40',
         u16(0x0C, 8)),
        ('head_off_oob', 2, 'HEAD chunk at', 'HEAD offset past end of file',
         u32(0x10, base['size'] + 0x10)),
        ('adpc_off_oob', 2, 'ADPC chunk at', 'ADPC offset past end of file',
         u32(0x18, base['size'] + 0x10)),
        ('data_off_oob', 2, 'DATA chunk at', 'DATA offset past end of file',
         u32(0x20, base['size'] + 0x10)),
        ('head_magic_bad', 2, 'bad HEAD chunk magic', 'HEAD magic corrupted',
         patch(base['head_off'], b'HEAX')),
        ('data_magic_bad', 2, 'bad DATA chunk magic', 'DATA magic corrupted',
         patch(base['data_off'], b'DATX')),
        ('adpc_magic_bad', 2, 'bad ADPC chunk magic', 'ADPC magic corrupted',
         patch(base['adpc_off'], b'ADPX')),
        ('head_size_tiny', 2, 'HEAD chunk too small', 'HEAD chunk size < 0x20',
         u32(0x14, 8)),
        ('data_size_zero', 2, 'DATA chunk is missing', 'DATA chunk size zero',
         u32(0x24, 0)),
        ('part1_oob', 2, 'HEAD part 1 at', 'HEAD part 1 offset outside HEAD',
         u32(base['head_off'] + 0x0C, BIG)),
        ('part3_oob', 2, 'HEAD part 3 at', 'HEAD part 3 offset outside HEAD',
         u32(base['head_off'] + 0x1C, BIG)),
        ('codec_bad', 2, 'codec 1, only DSP', 'codec != 2',
         u8(p1 + 0x00, 1)),
        ('channels_zero', 2, 'channel count 0', 'channel count 0',
         u8(p1 + 0x02, 0)),
        ('channels_huge', 2, 'channel count 200', 'channel count > 16',
         u8(p1 + 0x02, 200)),
        ('rate_zero', 2, 'sample rate 0', 'sample rate 0',
         u16(p1 + 0x04, 0)),
        ('total_zero', 2, 'total samples is zero', 'total samples 0',
         u32(p1 + 0x0C, 0)),
        ('loop_flag_bad', 2, 'loop flag 2', 'loop flag = 2',
         u8(p1 + 0x01, 2)),
        ('loop_start_oob', 2, 'not below total samples',
         'loop_start == total_samples', u32(p1 + 0x08, base['total_samples'])),
        ('block_count_zero', 2, 'block count is zero', 'block count 0',
         u32(p1 + 0x14, 0)),
        ('block_size_small', 2, 'smaller than one DSP frame',
         'block size 4', u32(p1 + 0x18, 4)),
        ('spb_zero', 2, 'samples per block is zero', 'samples per block 0',
         u32(p1 + 0x1C, 0)),
        ('block_size_huge', 2, 'above the supported maximum',
         'block size above BRSTM_MAX_BLOCK_SIZE',
         u32(p1 + 0x18, 0x40000000)),
        ('spb_huge', 2, 'above the supported maximum',
         'samples per block above BRSTM_MAX_SAMPLES_PER_BLOCK',
         u32(p1 + 0x1C, 0x40000000)),
        ('final_nopad_zero', 2, 'final block size 0',
         'final block nopad size 0', u32(p1 + 0x20, 0)),
        ('final_nopad_huge', 2, 'final block size',
         'final block nopad > block size',
         u32(p1 + 0x20, base['block_size'] + 1)),
        ('final_samples_zero', 2, 'final block samples 0',
         'final block samples 0', u32(p1 + 0x24, 0)),
        ('final_samples_huge', 2, 'final block samples',
         'final block samples > samples per block',
         u32(p1 + 0x24, base['samples_per_block'] + 1)),
        ('final_pad_small', 2, 'padded final block size',
         'padded final block < nopad', u32(p1 + 0x28, base['final_nopad'] - 1)),
        ('final_pad_huge', 2, 'padded final block size',
         'padded final block > block size',
         u32(p1 + 0x28, base['block_size'] + 1)),
        ('final_nopad_too_small', 2, 'final block too small',
         'nopad < ceil(samples/2)+ceil(samples/14)',
         u32(p1 + 0x20, need_final - 1)),
        ('total_mismatch', 2, 'inconsistent with block layout',
         'total != (blocks-1)*spb + final',
         u32(p1 + 0x0C, base['total_samples'] + 1)),
        ('adpcm_start_oob', 2, 'ADPCM data needs', 'adpcm_start past EOF',
         u32(p1 + 0x10, base['size'])),
        ('part3_count_bad', 2, 'part 3 lists', 'part 3 channel count mismatch',
         u8(p3, base['channels'] + 1)),
        ('ch_info_marker', 2, 'info marker', 'channel info marker != 1',
         u8(ch0_info, 2)),
        ('truncated', 2, 'exceeds file size', 'file truncated to 0x200 bytes',
         lambda d: d.__delitem__(slice(0x200, len(d)))),
    ]
    return cases


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: make_invalid_cases.py <base.brstm> <outdir>')
    base_path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)

    with open(base_path, 'rb') as f:
        data = bytearray(f.read())
    base = parse_base(data)

    manifest = []
    for name, exit_code, keyword, desc, mutate in build_cases(data, base):
        mutant = bytearray(data)
        mutate(mutant)
        path = os.path.join(outdir, name + '.brstm')
        with open(path, 'wb') as f:
            f.write(mutant)
        manifest.append({
            'name': name,
            'exit': exit_code,
            'keyword': keyword,
            'description': desc,
        })
        print('%-22s exit=%d keyword=%r' % (name, exit_code, keyword))

    with open(os.path.join(outdir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2)
    print('wrote %d cases to %s' % (len(manifest), outdir))


if __name__ == '__main__':
    main()
