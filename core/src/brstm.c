#include "brstm.h"

#include "dsp_adpcm.h" /* DSP_ADPCM_FRAME_SIZE */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Stable human-readable text for each status code. Kept separate from the
 * fail() messages so callers can label an error even when no detailed
 * message was produced (e.g. errors returned by the read helpers below). */
const char *brstm_status_string(BrstmStatus status)
{
    switch (status) {
    case BRSTM_OK:             return "ok";
    case BRSTM_ERR_IO:         return "I/O error";
    case BRSTM_ERR_TOO_SMALL:  return "file too small";
    case BRSTM_ERR_MAGIC:      return "not an RSTM file";
    case BRSTM_ERR_BYTE_ORDER: return "byte-swapped RSTM not supported";
    case BRSTM_ERR_CHUNK:      return "invalid chunk table";
    case BRSTM_ERR_CODEC:      return "unsupported codec";
    case BRSTM_ERR_PARAMS:     return "invalid header parameters";
    case BRSTM_ERR_LOOP:       return "invalid loop metadata";
    case BRSTM_ERR_LAYOUT:     return "ADPCM data layout inconsistent with file";
    case BRSTM_ERR_NO_ADPC:    return "ADPC table missing or short";
    }
    return "unknown error";
}

/* Single error exit for the whole parser: writes the formatted detail into
 * the caller's buffer (err may be NULL when no detail is wanted) and returns
 * the status, so every failure site is one statement instead of two. */
static BrstmStatus fail(char *err, size_t errlen, BrstmStatus status,
                        const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return status;
}

/* BRSTM is big-endian (Wii): these read helpers are the only place that
 * knows the byte order, so parsing code never shifts bytes by hand. */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Seek+read as one operation; returns 0 if either step fails. All reads in
 * this file go through it so IO failures are detected uniformly. */
static int read_at(FILE *f, long offset, void *buf, size_t len)
{
    return fseek(f, offset, SEEK_SET) == 0
        && fread(buf, 1, len, f) == len;
}

/* Fixed 0x40-byte RSTM header. Offsets are absolute in the file; sizes are
 * the declared chunk payload lengths. */
typedef struct {
    uint32_t file_size;
    uint16_t header_size;
    uint32_t head_off, head_size;
    uint32_t adpc_off, adpc_size;
    uint32_t data_off, data_size;
} RstmHeader;

/* Parse and validate the fixed header: magic, byte order, chunk table bounds
 * and HEAD/DATA (and ADPC when present) magic bytes. Everything that could
 * make later offsets unsafe is rejected here, so the parse_head_* functions
 * can trust h. */
static BrstmStatus parse_rstm_header(FILE *f, long fsize, RstmHeader *h,
                                     char *err, size_t errlen)
{
    uint8_t buf[0x40];
    if (fsize < (long)sizeof buf)
        return fail(err, errlen, BRSTM_ERR_TOO_SMALL,
                    "file is %ld bytes, smaller than the 64-byte RSTM header",
                    fsize);
    if (!read_at(f, 0, buf, sizeof buf))
        return fail(err, errlen, BRSTM_ERR_IO, "cannot read RSTM header");
    if (memcmp(buf, "RSTM", 4) != 0)
        return fail(err, errlen, BRSTM_ERR_MAGIC,
                    "magic is '%c%c%c%c', expected 'RSTM'",
                    buf[0], buf[1], buf[2], buf[3]);
    if (buf[4] != 0xFE || buf[5] != 0xFF)
        return fail(err, errlen, BRSTM_ERR_BYTE_ORDER,
                    "byte order mark 0x%02X%02X, expected big-endian FE FF",
                    buf[4], buf[5]);

    h->file_size   = be32(buf + 0x08);
    h->header_size = be16(buf + 0x0C);
    h->head_off    = be32(buf + 0x10);
    h->head_size   = be32(buf + 0x14);
    h->adpc_off    = be32(buf + 0x18);
    h->adpc_size   = be32(buf + 0x1C);
    h->data_off    = be32(buf + 0x20);
    h->data_size   = be32(buf + 0x24);

    if (h->header_size < 0x40 || h->header_size > (uint32_t)fsize)
        return fail(err, errlen, BRSTM_ERR_CHUNK,
                    "header size %u out of range", h->header_size);

    struct { uint32_t off, size; const char *name; } chunks[] = {
        { h->head_off, h->head_size, "HEAD" },
        { h->adpc_off, h->adpc_size, "ADPC" },
        { h->data_off, h->data_size, "DATA" },
    };
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        if (chunks[i].size == 0)
            continue; /* ADPC may legitimately be absent */
        if (chunks[i].off < 0x40
            || (uint64_t)chunks[i].off + chunks[i].size > (uint64_t)fsize)
            return fail(err, errlen, BRSTM_ERR_CHUNK,
                        "%s chunk at %u + %u exceeds file size %ld",
                        chunks[i].name, chunks[i].off, chunks[i].size, fsize);
    }
    if (h->head_size < 0x20)
        return fail(err, errlen, BRSTM_ERR_CHUNK,
                    "HEAD chunk too small (%u bytes)", h->head_size);
    if (h->data_size == 0)
        return fail(err, errlen, BRSTM_ERR_CHUNK, "DATA chunk is missing");

    uint8_t magic[4];
    if (!read_at(f, h->head_off, magic, 4) || memcmp(magic, "HEAD", 4) != 0)
        return fail(err, errlen, BRSTM_ERR_CHUNK, "bad HEAD chunk magic");
    if (!read_at(f, h->data_off, magic, 4) || memcmp(magic, "DATA", 4) != 0)
        return fail(err, errlen, BRSTM_ERR_CHUNK, "bad DATA chunk magic");
    if (h->adpc_size) {
        if (!read_at(f, h->adpc_off, magic, 4) || memcmp(magic, "ADPC", 4) != 0)
            return fail(err, errlen, BRSTM_ERR_CHUNK, "bad ADPC chunk magic");
    }
    return BRSTM_OK;
}

/* HEAD part 1: fixed 0x34-byte stream parameter block.
 * Reads the stream fields and then validates every one of them against the
 * ranges the decoder relies on (codec, channels, rate, loop, block geometry).
 * The cross-checks at the end (final-block byte need, total = (blocks-1)*spb
 * + final) hold for all 301 collection files, so a mismatch means a corrupt
 * header rather than an exotic-but-valid variant. */
static BrstmStatus parse_head_part1(FILE *f, const RstmHeader *h,
                                    uint32_t rel, BrstmInfo *info,
                                    char *err, size_t errlen)
{
    uint8_t buf[0x34];
    uint64_t abs = (uint64_t)h->head_off + 8 + rel;
    if (abs + sizeof buf > (uint64_t)h->head_off + h->head_size)
        return fail(err, errlen, BRSTM_ERR_CHUNK,
                    "HEAD part 1 at +%u lies outside the HEAD chunk", rel);
    if (!read_at(f, (long)abs, buf, sizeof buf))
        return fail(err, errlen, BRSTM_ERR_IO, "cannot read HEAD part 1");

    info->codec  = buf[0x00];
    info->loop   = buf[0x01];
    info->channels = buf[0x02];
    info->sample_rate = be16(buf + 0x04);
    info->loop_start = be32(buf + 0x08);
    info->total_samples = be32(buf + 0x0C);
    info->adpcm_start = be32(buf + 0x10);
    info->block_count = be32(buf + 0x14);
    info->block_size = be32(buf + 0x18);
    info->samples_per_block = be32(buf + 0x1C);
    info->final_block_size_nopad = be32(buf + 0x20);
    info->final_block_samples = be32(buf + 0x24);
    info->final_block_size_pad = be32(buf + 0x28);

    if (info->codec != 2)
        return fail(err, errlen, BRSTM_ERR_CODEC,
                    "codec %u, only DSP 4-bit ADPCM (2) is supported",
                    info->codec);
    if (info->channels == 0 || info->channels > BRSTM_MAX_CHANNELS)
        return fail(err, errlen, BRSTM_ERR_PARAMS,
                    "channel count %u out of range 1..%d",
                    info->channels, BRSTM_MAX_CHANNELS);
    if (info->sample_rate == 0 || info->sample_rate > 192000)
        return fail(err, errlen, BRSTM_ERR_PARAMS,
                    "sample rate %u out of range", info->sample_rate);
    if (info->total_samples == 0)
        return fail(err, errlen, BRSTM_ERR_PARAMS, "total samples is zero");
    if (info->loop > 1)
        return fail(err, errlen, BRSTM_ERR_LOOP,
                    "loop flag %u, expected 0 or 1", info->loop);
    if (info->loop && info->loop_start >= info->total_samples)
        return fail(err, errlen, BRSTM_ERR_LOOP,
                    "loop start %u not below total samples %u",
                    info->loop_start, info->total_samples);
    if (info->block_count == 0)
        return fail(err, errlen, BRSTM_ERR_PARAMS, "block count is zero");
    if (info->block_size < DSP_ADPCM_FRAME_SIZE)
        return fail(err, errlen, BRSTM_ERR_PARAMS,
                    "block size %u smaller than one DSP frame",
                    info->block_size);
    if (info->samples_per_block == 0)
        return fail(err, errlen, BRSTM_ERR_PARAMS, "samples per block is zero");
    if (info->block_count > 1) {
        if (info->final_block_size_nopad == 0
            || info->final_block_size_nopad > info->block_size)
            return fail(err, errlen, BRSTM_ERR_PARAMS,
                        "final block size %u out of range",
                        info->final_block_size_nopad);
        if (info->final_block_samples == 0
            || info->final_block_samples > info->samples_per_block)
            return fail(err, errlen, BRSTM_ERR_PARAMS,
                        "final block samples %u out of range",
                        info->final_block_samples);
        if (info->final_block_size_pad < info->final_block_size_nopad
            || info->final_block_size_pad > info->block_size)
            return fail(err, errlen, BRSTM_ERR_PARAMS,
                        "padded final block size %u out of range",
                        info->final_block_size_pad);
        /* Final block must physically hold its samples: nibble bytes plus
         * one header byte per 14-sample frame (holds for all 301 files;
         * see research/dsp_final_checks.py check3). */
        uint32_t need = (info->final_block_samples + 1) / 2
                      + (info->final_block_samples + 13) / 14;
        if (info->final_block_size_nopad < need)
            return fail(err, errlen, BRSTM_ERR_LAYOUT,
                        "final block too small: %u bytes, need %u for %u samples",
                        info->final_block_size_nopad, need,
                        info->final_block_samples);
    }
    /* total == (blocks-1) * spb + final: holds for all 301 collection
     * files; a mismatch indicates a corrupt header. */
    uint64_t total = (uint64_t)(info->block_count - 1) * info->samples_per_block
                   + (info->block_count > 1 ? info->final_block_samples
                                            : info->samples_per_block);
    if (total != info->total_samples)
        return fail(err, errlen, BRSTM_ERR_LAYOUT,
                    "total samples %u inconsistent with block layout "
                    "(expected %llu)", info->total_samples,
                    (unsigned long long)total);
    return BRSTM_OK;
}

/* HEAD part 3: per-channel ADPCM parameters. Each channel info struct holds
 * 8 predictor coefficient pairs (16 x int16), gain, initial and loop
 * predictor/scale and history (wiibrew BRSTM, "ADPCM Channel Information").
 * Observed max predictor index in the collection is 7, matching the 8
 * stored pairs; the decoder clamps the nibble to the valid range. */
static BrstmStatus parse_head_part3(FILE *f, const RstmHeader *h,
                                    uint32_t rel, BrstmInfo *info,
                                    char *err, size_t errlen)
{
    uint8_t count;
    uint64_t abs = (uint64_t)h->head_off + 8 + rel;
    if (abs + 4 > (uint64_t)h->head_off + h->head_size)
        return fail(err, errlen, BRSTM_ERR_CHUNK,
                    "HEAD part 3 at +%u lies outside the HEAD chunk", rel);
    if (!read_at(f, (long)abs, &count, 1))
        return fail(err, errlen, BRSTM_ERR_IO, "cannot read HEAD part 3");
    if (count != info->channels)
        return fail(err, errlen, BRSTM_ERR_PARAMS,
                    "part 3 lists %u channels, header says %u",
                    count, info->channels);

    for (uint32_t c = 0; c < info->channels; c++) {
        uint8_t entry[8];
        uint64_t eabs = abs + 4 + 8 * (uint64_t)c;
        if (eabs + 8 > (uint64_t)h->head_off + h->head_size)
            return fail(err, errlen, BRSTM_ERR_CHUNK,
                        "part 3 channel %u entry outside HEAD chunk", c);
        if (!read_at(f, (long)eabs, entry, 8))
            return fail(err, errlen, BRSTM_ERR_IO,
                        "cannot read part 3 channel %u entry", c);

        uint32_t coff = be32(entry + 4);
        uint64_t cabs = (uint64_t)h->head_off + 8 + coff;
        uint8_t cbuf[0x36];
        if (cabs + sizeof cbuf > (uint64_t)h->head_off + h->head_size)
            return fail(err, errlen, BRSTM_ERR_CHUNK,
                        "channel %u info at +%u outside HEAD chunk", c, coff);
        if (!read_at(f, (long)cabs, cbuf, sizeof cbuf))
            return fail(err, errlen, BRSTM_ERR_IO,
                        "cannot read channel %u info", c);
        if (cbuf[0] != 0x01)
            return fail(err, errlen, BRSTM_ERR_CHUNK,
                        "channel %u info marker 0x%02X, expected 1",
                        c, cbuf[0]);

        /* 8 coefficient pairs at +0x08, big-endian int16 each. */
        for (int k = 0; k < BRSTM_DSP_PREDICTORS; k++) {
            info->coefs[c][k][0] = (int16_t)be16(cbuf + 8 + 4 * k);
            info->coefs[c][k][1] = (int16_t)be16(cbuf + 8 + 4 * k + 2);
        }
        info->init_predictor_scale[c] = be16(cbuf + 0x2A);
        info->init_hist1[c] = (int16_t)be16(cbuf + 0x2C);
        info->init_hist2[c] = (int16_t)be16(cbuf + 0x2E);
        info->loop_predictor_scale[c] = be16(cbuf + 0x30);
        info->loop_hist1[c] = (int16_t)be16(cbuf + 0x32);
        info->loop_hist2[c] = (int16_t)be16(cbuf + 0x34);
    }
    return BRSTM_OK;
}

/* Open sequence: size the file -> fixed header -> HEAD parts 1 and 3 ->
 * layout arithmetic -> ADPC table location. Every early return closes the
 * FILE* so a failed open never leaks a handle (the winmm handle-leak
 * measurements in MEMORY.md made leak discipline a hard rule here). The
 * output struct is zeroed first so brstm_close is safe on any error path. */
BrstmStatus brstm_open(const char *path, Brstm *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);

    FILE *f = fopen(path, "rb");
    if (!f)
        return fail(err, errlen, BRSTM_ERR_IO, "cannot open '%s'", path);
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return fail(err, errlen, BRSTM_ERR_IO, "cannot seek '%s'", path);
    }
    long fsize = ftell(f);
    if (fsize < 0) {
        fclose(f);
        return fail(err, errlen, BRSTM_ERR_IO, "cannot size '%s'", path);
    }

    RstmHeader h;
    BrstmStatus st = parse_rstm_header(f, fsize, &h, err, errlen);
    if (st != BRSTM_OK) {
        fclose(f);
        return st;
    }

    /* HEAD part offsets are relative to HEAD+8. */
    uint8_t head[0x20];
    if (!read_at(f, h.head_off, head, sizeof head)) {
        fclose(f);
        return fail(err, errlen, BRSTM_ERR_IO, "cannot read HEAD header");
    }
    uint32_t rel1 = be32(head + 0x0C);
    uint32_t rel3 = be32(head + 0x1C);

    BrstmInfo *info = &out->info;
    st = parse_head_part1(f, &h, rel1, info, err, errlen);
    if (st != BRSTM_OK) {
        fclose(f);
        return st;
    }
    st = parse_head_part3(f, &h, rel3, info, err, errlen);
    if (st != BRSTM_OK) {
        fclose(f);
        return st;
    }

    /* ADPCM data must fit: (blocks-1) full blocks + padded final block,
     * interleaved per channel. */
    uint32_t final_stride = info->block_count > 1
                          ? info->final_block_size_pad
                          : info->block_size;
    uint64_t need = (uint64_t)info->adpcm_start
                  + (uint64_t)(info->block_count - 1) * info->block_size
                        * info->channels
                  + (uint64_t)final_stride * info->channels;
    if (need > (uint64_t)fsize) {
        fclose(f);
        return fail(err, errlen, BRSTM_ERR_LAYOUT,
                    "ADPCM data needs %llu bytes, file has %ld",
                    (unsigned long long)need, fsize);
    }

    /* ADPC table: entry b, channel c = decoder state (hist1, hist2)
     * entering block b. NOTE: 19 of 301 collection files carry a fully
     * zeroed table; treat it as a hint, not as ground truth. */
    info->has_adpc = 0;
    info->adpc_entry_count = 0;
    out->adpc_entries_offset = 0;
    if (h.adpc_size >= 8) {
        uint32_t entry_bytes = info->channels * 4;
        uint32_t count = (h.adpc_size - 8) / entry_bytes;
        if (count > 0) {
            info->has_adpc = 1;
            info->adpc_entry_count = count;
            out->adpc_entries_offset = h.adpc_off + 8;
        }
    }

    out->file = f;
    out->file_size = fsize;
    return BRSTM_OK;
}

/* Close and release the file. Safe to call twice: brstm_open zeroes the
 * struct first, and this nulls the handle, so *b is always in a known state
 * after either an error path or a successful open. */
void brstm_close(Brstm *b)
{
    if (b->file) {
        fclose(b->file);
        b->file = NULL;
    }
}

/* Block b starts at b * samples_per_block: all blocks except the last have
 * identical size, so no per-block table is needed. Out-of-range blocks clamp
 * to the last one instead of failing, because callers use these to translate
 * a position (which can be at total_samples exactly) and clamping is the
 * correct answer there, not an error. */
uint32_t brstm_block_start_sample(const Brstm *b, uint32_t block)
{
    const BrstmInfo *info = &b->info;
    if (block >= info->block_count)
        block = info->block_count - 1;
    return (uint32_t)((uint64_t)block * info->samples_per_block);
}

/* Samples in one block: full blocks all carry samples_per_block, only the
 * (possibly single) final block can be shorter; clamping as above. */
uint32_t brstm_block_sample_count(const Brstm *b, uint32_t block)
{
    const BrstmInfo *info = &b->info;
    if (block >= info->block_count)
        block = info->block_count - 1;
    if (block == info->block_count - 1 && info->block_count > 1)
        return info->final_block_samples;
    return info->samples_per_block;
}

/* Inverse of brstm_block_start_sample for seeking. Blocks are uniform, so
 * division is exact; a sample past the end clamps to the last block (the
 * caller then trims at total_samples). */
uint32_t brstm_block_for_sample(const Brstm *b, uint32_t sample)
{
    uint32_t block = sample / b->info.samples_per_block;
    if (block >= b->info.block_count)
        block = b->info.block_count - 1;
    return block;
}

/* Address of channel c in block b = adpcm_start + b*(block_size*channels)
 * + c*stride: channels are interleaved one block at a time (channel-major
 * within a block). The final block uses its padded stride as the channel
 * spacing but only its unpadded size as the payload, so short trailing
 * padding of the file is never read. */
BrstmStatus brstm_read_channel_block(const Brstm *b, uint32_t block,
                                     uint32_t channel, uint8_t *buf,
                                     size_t buflen, size_t *out_len)
{
    const BrstmInfo *info = &b->info;
    if (block >= info->block_count || channel >= info->channels)
        return BRSTM_ERR_PARAMS;

    int final = (block == info->block_count - 1) && info->block_count > 1;
    uint32_t stride = final ? info->final_block_size_pad : info->block_size;
    uint32_t size = final ? info->final_block_size_nopad : info->block_size;
    if (size > buflen)
        return BRSTM_ERR_PARAMS;

    uint64_t offset = (uint64_t)info->adpcm_start
                    + (uint64_t)block * info->block_size * info->channels
                    + (uint64_t)channel * stride;
    if (offset + size > (uint64_t)b->file_size)
        return BRSTM_ERR_LAYOUT;
    if (!read_at(b->file, (long)offset, buf, size))
        return BRSTM_ERR_IO;
    *out_len = size;
    return BRSTM_OK;
}

/* ADPC entry layout: entry b = b*(channels*4) bytes after the table start,
 * channel c at +c*4, each entry a big-endian (hist1, hist2) pair. Returns
 * BRSTM_ERR_NO_ADPC for a missing/short table or out-of-range index so the
 * caller can distinguish "no usable hint" from a real IO error and fall back
 * to carried state. */
BrstmStatus brstm_adpc_state(const Brstm *b, uint32_t block, uint32_t channel,
                             int16_t *hist1, int16_t *hist2)
{
    const BrstmInfo *info = &b->info;
    if (!info->has_adpc)
        return BRSTM_ERR_NO_ADPC;
    if (block >= info->adpc_entry_count || channel >= info->channels)
        return BRSTM_ERR_NO_ADPC;

    uint8_t entry[4];
    uint64_t offset = (uint64_t)b->adpc_entries_offset
                    + (uint64_t)block * info->channels * 4
                    + (uint64_t)channel * 4;
    if (!read_at(b->file, (long)offset, entry, 4))
        return BRSTM_ERR_IO;
    *hist1 = (int16_t)be16(entry);
    *hist2 = (int16_t)be16(entry + 2);
    return BRSTM_OK;
}
