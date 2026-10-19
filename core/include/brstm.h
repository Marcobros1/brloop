/* BRSTM container reader.
 *
 * Big-endian RSTM/HEAD/ADPC/DATA parsing with strict validation.
 * Format reference: wiibrew.org/wiki/BRSTM_file plus empirical checks on
 * 301 collection files (see research/INFORME_TAREA01.md, evidence C1-C16).
 */
#ifndef BRSTM_H
#define BRSTM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRSTM_MAX_CHANNELS 16

/* Each BRSTM channel info block stores 16 int16 coefficients = 8 predictor
 * pairs. Observed max predictor index in real files is 7. */
#define BRSTM_DSP_PREDICTORS 8

typedef enum {
    BRSTM_OK = 0,
    BRSTM_ERR_IO = 1,          /* file open/read failure */
    BRSTM_ERR_TOO_SMALL = 2,   /* smaller than fixed header */
    BRSTM_ERR_MAGIC = 3,       /* not an RSTM file */
    BRSTM_ERR_BYTE_ORDER = 4,  /* byte-swapped RSTM not supported */
    BRSTM_ERR_CHUNK = 5,       /* HEAD/ADPC/DATA chunk out of bounds or bad magic */
    BRSTM_ERR_CODEC = 6,       /* codec other than DSP-ADPCM (2) */
    BRSTM_ERR_PARAMS = 7,      /* implausible header parameters */
    BRSTM_ERR_LOOP = 8,        /* invalid loop metadata */
    BRSTM_ERR_LAYOUT = 9,      /* ADPCM data does not fit the file / header inconsistent */
    BRSTM_ERR_NO_ADPC = 10     /* ADPC table missing or too short for requested block */
} BrstmStatus;

const char *brstm_status_string(BrstmStatus status);

typedef struct {
    uint8_t  codec;                    /* 2 = DSP 4-bit ADPCM */
    uint8_t  loop;                     /* 0/1 */
    uint8_t  channels;                 /* 1..BRSTM_MAX_CHANNELS */
    uint16_t sample_rate;
    uint32_t loop_start;               /* samples; loop section is [loop_start, total) */
    uint32_t total_samples;
    uint32_t block_count;
    uint32_t block_size;               /* interleave block, bytes per channel */
    uint32_t samples_per_block;
    uint32_t final_block_size_nopad;   /* bytes of real data in final block */
    uint32_t final_block_samples;
    uint32_t final_block_size_pad;     /* per-channel stride of final block */
    uint32_t adpcm_start;              /* absolute offset of ADPCM data */

    /* Per-channel ADPCM parameters from HEAD part 3. */
    int16_t  coefs[BRSTM_MAX_CHANNELS][BRSTM_DSP_PREDICTORS][2];
    int16_t  init_hist1[BRSTM_MAX_CHANNELS];
    int16_t  init_hist2[BRSTM_MAX_CHANNELS];
    uint16_t init_predictor_scale[BRSTM_MAX_CHANNELS];
    uint16_t loop_predictor_scale[BRSTM_MAX_CHANNELS];
    int16_t  loop_hist1[BRSTM_MAX_CHANNELS];
    int16_t  loop_hist2[BRSTM_MAX_CHANNELS];

    int      has_adpc;                 /* ADPC chunk present and parsed */
    uint32_t adpc_entry_count;         /* entries = states at block boundaries */
} BrstmInfo;

typedef struct {
    FILE     *file;
    long      file_size;
    uint32_t  adpc_entries_offset;     /* absolute offset of ADPC entry 0 */
    BrstmInfo info;
} Brstm;

/* Open and validate a BRSTM file. On error fills err (if non-NULL) with a
 * human-readable message and returns a status != BRSTM_OK. */
BrstmStatus brstm_open(const char *path, Brstm *out, char *err, size_t errlen);

void brstm_close(Brstm *b);

/* First sample index and sample count of a block. */
uint32_t brstm_block_start_sample(const Brstm *b, uint32_t block);
uint32_t brstm_block_sample_count(const Brstm *b, uint32_t block);

/* Block containing a sample index (clamped to the last block). */
uint32_t brstm_block_for_sample(const Brstm *b, uint32_t sample);

/* Read the raw ADPCM bytes of one channel block into buf (must hold at least
 * block_size bytes). Returns BRSTM_OK and the byte count in *out_len. */
BrstmStatus brstm_read_channel_block(const Brstm *b, uint32_t block,
                                     uint32_t channel, uint8_t *buf,
                                     size_t buflen, size_t *out_len);

/* Decoder state entering a block, from the ADPC table, per channel. Allows
 * independent per-block decoding (needed for loop jumps). CAUTION: 19 of 301
 * collection files carry a fully zeroed ADPC table; callers must be able to
 * fall back to carrying state from an earlier block. */
BrstmStatus brstm_adpc_state(const Brstm *b, uint32_t block, uint32_t channel,
                             int16_t *hist1, int16_t *hist2);

#ifdef __cplusplus
}
#endif

#endif /* BRSTM_H */
