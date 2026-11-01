/* Writes a minimal, valid BRSTM file with silence, so tests can run without the
 * reference collection.
 *
 * Usage: make_synthetic_brstm <out.brstm> [channels 1..2] [loop 0|1]
 *
 * Layout copied from a real single-track file (see core/src/brstm.c for the
 * reader's checks):
 *   0x00  RSTM header (0x40 bytes)
 *   0x40  HEAD: 8-byte chunk header, then part 1 (stream parameters), part 2
 *         (empty region), part 3 (per-channel table and channel infos)
 *   ...   ADPC: magic and size only, no entries
 *   ...   DATA: 0x20-byte header, then one block of silence per channel
 * All fields are big-endian. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_BYTES 8192u         /* bytes per channel block */
#define SAMPLES_PER_BLOCK 14336u  /* samples per channel block */
#define MAX_CHANNELS 4
#define DEFAULT_BLOCKS 1
#define FINAL_BLOCK_SAMPLES 7168u /* half a block: a short, padded final block */

/* HEAD layout, relative to the start of the HEAD chunk. */
/* Large enough for 4 channel infos: the last one ends at
 * CHANNEL_INFO_0 + 3*CHANNEL_INFO_STEP + CHANNEL_INFO_LEN = 342 bytes past
 * HEAD+8, plus the 8-byte chunk header, rounded up to 0x160. */
#define HEAD_CHUNK_SIZE 0x160u
#define PART1_REL 0x18u           /* offsets are relative to HEAD+8 */
#define PART2_REL 0x4Cu
#define PART3_REL 0x64u
#define PART3_LEN 0x04u           /* count + entries */
#define CHANNEL_INFO_LEN 0x36u
#define CHANNEL_INFO_0 0x78u
#define CHANNEL_INFO_STEP 0x38u   /* 56 bytes between channel infos */

#define ADPC_CHUNK_SIZE 8u
#define DATA_HEADER_SIZE 0x20u

static void w16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v >> 8);
    dst[1] = (uint8_t)v;
}

static void w32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);
    dst[3] = (uint8_t)v;
}

int main(int argc, char **argv)
{
    uint32_t channels = 1;
    uint32_t blocks = DEFAULT_BLOCKS;
    int loop = 0;
    uint32_t total_samples, adpc_off, data_off, data_size, file_size, adpcm_start;
    uint8_t *buf, *head, *p1, *p3;
    uint32_t c;
    FILE *f;

    if (argc < 2) {
        fprintf(stderr, "usage: make_synthetic_brstm <out.brstm> [channels 1..4] "
                        "[loop 0|1] [blocks >= 1]\n");
        return 2;
    }
    if (argc > 2) channels = (uint32_t)strtoul(argv[2], NULL, 10);
    if (argc > 3) loop = atoi(argv[3]) != 0;
    if (argc > 4) blocks = (uint32_t)strtoul(argv[4], NULL, 10);
    if (channels < 1 || channels > MAX_CHANNELS) {
        fprintf(stderr, "channels must be 1..%d\n", MAX_CHANNELS);
        return 2;
    }
    if (blocks < 1) {
        fprintf(stderr, "blocks must be at least 1\n");
        return 2;
    }

    /* Every block is full except the last one, which is FINAL_BLOCK_SAMPLES
     * long. With one block the single block is the final one. */
    total_samples = (blocks - 1) * SAMPLES_PER_BLOCK
                  + (blocks > 1 ? FINAL_BLOCK_SAMPLES : SAMPLES_PER_BLOCK);

    /* Chunk placement: HEAD at 0x40, ADPC right after it, DATA after that.
     * DATA holds every block, channel-major within each block. */
    adpc_off = 0x40 + HEAD_CHUNK_SIZE;
    data_off = adpc_off + ADPC_CHUNK_SIZE;
    data_size = DATA_HEADER_SIZE + BLOCK_BYTES * channels * blocks;
    adpcm_start = data_off + DATA_HEADER_SIZE;
    file_size = data_off + data_size;

    buf = calloc(1, file_size);
    if (!buf) return 1;

    /* RSTM header. */
    memcpy(buf, "RSTM", 4);
    buf[4] = 0xFE;
    buf[5] = 0xFF;
    w32(buf + 0x08, file_size);
    w16(buf + 0x0C, 0x40);
    w32(buf + 0x10, 0x40);
    w32(buf + 0x14, HEAD_CHUNK_SIZE);
    w32(buf + 0x18, adpc_off);
    w32(buf + 0x1C, ADPC_CHUNK_SIZE);
    w32(buf + 0x20, data_off);
    w32(buf + 0x24, data_size);

    /* HEAD chunk: magic, size, then the three part offsets. */
    head = buf + 0x40;
    memcpy(head, "HEAD", 4);
    w32(head + 4, HEAD_CHUNK_SIZE);
    w32(head + 0x0C, PART1_REL);
    w32(head + 0x14, PART2_REL);
    w32(head + 0x1C, PART3_REL);

    /* Part 1: stream parameters. Written field by field at fixed offsets. */
    p1 = head + 8 + PART1_REL;
    p1[0x00] = 2;                       /* codec: DSP-ADPCM */
    p1[0x01] = (uint8_t)(loop ? 1 : 0); /* loop flag */
    p1[0x02] = (uint8_t)channels;
    w16(p1 + 0x04, 48000);              /* sample rate */
    w32(p1 + 0x08, 0);                  /* loop start */
    w32(p1 + 0x0C, total_samples);      /* total samples */
    w32(p1 + 0x10, adpcm_start);        /* ADPCM data, absolute offset */
    w32(p1 + 0x14, blocks);             /* block count */
    w32(p1 + 0x18, BLOCK_BYTES);        /* block size */
    w32(p1 + 0x1C, SAMPLES_PER_BLOCK);  /* samples per block */
    w32(p1 + 0x20, BLOCK_BYTES);        /* final block size, no padding */
    w32(p1 + 0x24, blocks > 1 ? FINAL_BLOCK_SAMPLES : SAMPLES_PER_BLOCK);
    w32(p1 + 0x28, BLOCK_BYTES);        /* final block size, padded */

    /* Part 3: channel count, one 8-byte entry per channel, then the infos. */
    p3 = head + 8 + PART3_REL;
    p3[0] = (uint8_t)channels;
    for (c = 0; c < channels; c++) {
        uint8_t *entry = p3 + 4 + 8 * c;
        uint32_t info_rel = CHANNEL_INFO_0 + CHANNEL_INFO_STEP * c;
        uint8_t *info = head + 8 + info_rel;
        entry[0] = 1; /* channel info present */
        w32(entry + 4, info_rel);
        info[0x00] = 0x01; /* channel info marker */
        /* Coefficients and history stay zero: silence needs no predictors. */
    }

    /* ADPC chunk: magic and size, no entries. */
    memcpy(buf + adpc_off, "ADPC", 4);
    w32(buf + adpc_off + 4, ADPC_CHUNK_SIZE);

    /* DATA chunk: header, then one block of zeros per channel. */
    memcpy(buf + data_off, "DATA", 4);
    w32(buf + data_off + 4, data_size);
    w32(buf + data_off + 8, DATA_HEADER_SIZE);

    f = fopen(argv[1], "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        free(buf);
        return 1;
    }
    if (fwrite(buf, 1, file_size, f) != file_size) {
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);
    free(buf);
    return 0;
}
