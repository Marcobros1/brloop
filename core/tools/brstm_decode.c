/* brstm_decode: decode a BRSTM file to a PCM16 WAV, block by block.
 *
 * Usage: brstm_decode <in.brstm> <out.wav> [--state carry|adpc]
 *
 *   --state carry  (default) decoder state is carried across blocks.
 *   --state adpc   every block starts from the ADPC table state; used to
 *                  verify that per-block decoding equals continuous
 *                  decoding. Fails (exit 4) if the ADPC table is missing,
 *                  short, or mostly zeroed (19 of 301 collection files
 *                  carry a fully zeroed table; single zero entries can be
 *                  legitimate silence, so the check is table-wide).
 *
 * Exit codes: 0 ok, 1 usage, 2 invalid BRSTM, 3 I/O error, 4 ADPC unusable.
 */
#include "brstm.h"
#include "dsp_adpcm.h"
#include "wav_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *state_mode = "carry";
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--state") == 0 && a + 1 < argc) {
            state_mode = argv[++a];
        } else if (!in) {
            in = argv[a];
        } else if (!out) {
            out = argv[a];
        } else {
            fprintf(stderr, "error: unexpected argument '%s'\n", argv[a]);
            return 1;
        }
    }
    if (!in || !out) {
        fprintf(stderr, "usage: %s <in.brstm> <out.wav> [--state carry|adpc]\n",
                argc > 0 ? argv[0] : "brstm_decode");
        return 1;
    }
    int use_adpc = strcmp(state_mode, "adpc") == 0;
    if (!use_adpc && strcmp(state_mode, "carry") != 0) {
        fprintf(stderr, "error: --state must be carry or adpc\n");
        return 1;
    }

    Brstm b;
    char err[256];
    BrstmStatus st = brstm_open(in, &b, err, sizeof err);
    if (st != BRSTM_OK) {
        fprintf(stderr, "error: %s (%s)\n", err, brstm_status_string(st));
        return st == BRSTM_ERR_IO ? 3 : 2;
    }

    const BrstmInfo *info = &b.info;

    /* Load ADPC states once; a mostly-zero table means the encoder never
     * filled it and per-block states cannot be trusted. */
    int16_t (*adpc)[2] = NULL;
    if (use_adpc) {
        size_t cells = (size_t)info->adpc_entry_count * info->channels;
        adpc = malloc(cells * sizeof *adpc);
        if (!adpc) {
            fprintf(stderr, "error: out of memory\n");
            brstm_close(&b);
            return 3;
        }
        uint32_t zero_entries = 0, checked = 0;
        for (uint32_t blk = 0; blk < info->adpc_entry_count
                                && blk < info->block_count; blk++) {
            for (uint32_t c = 0; c < info->channels; c++) {
                st = brstm_adpc_state(&b, blk, c, &adpc[blk * info->channels + c][0],
                                      &adpc[blk * info->channels + c][1]);
                if (st != BRSTM_OK) {
                    fprintf(stderr, "error: ADPC state unavailable for block "
                                    "%u channel %u (%s)\n",
                            blk, c, brstm_status_string(st));
                    free(adpc);
                    brstm_close(&b);
                    return 4;
                }
            }
            if (blk > 0) {
                int all_zero = 1;
                for (uint32_t c = 0; c < info->channels; c++)
                    if (adpc[blk * info->channels + c][0] != 0
                        || adpc[blk * info->channels + c][1] != 0)
                        all_zero = 0;
                if (all_zero)
                    zero_entries++;
                checked++;
            }
        }
        if (info->adpc_entry_count < info->block_count) {
            fprintf(stderr, "error: ADPC table has %u entries, need at least "
                            "%u blocks\n", info->adpc_entry_count,
                    info->block_count);
            free(adpc);
            brstm_close(&b);
            return 4;
        }
        if (checked > 0 && zero_entries > checked / 2) {
            fprintf(stderr, "error: ADPC table is mostly zeroed (%u of %u "
                            "entries); encoder never filled it\n",
                    zero_entries, checked);
            free(adpc);
            brstm_close(&b);
            return 4;
        }
    }

    uint8_t *raw = malloc(info->block_size);
    int16_t *pcm = malloc((size_t)info->samples_per_block * info->channels
                          * sizeof(int16_t));
    int16_t *chbuf = malloc((size_t)info->samples_per_block * sizeof(int16_t));
    if (!raw || !pcm || !chbuf) {
        fprintf(stderr, "error: out of memory\n");
        free(raw); free(pcm); free(chbuf); free(adpc);
        brstm_close(&b);
        return 3;
    }

    WavWriter w;
    if (wav_open(&w, out, info->channels, info->sample_rate) != 0) {
        fprintf(stderr, "error: cannot create '%s'\n", out);
        free(raw); free(pcm); free(chbuf); free(adpc);
        brstm_close(&b);
        return 3;
    }

    DspAdpcmState state[BRSTM_MAX_CHANNELS];
    for (uint32_t c = 0; c < info->channels; c++) {
        state[c].hist1 = info->init_hist1[c];
        state[c].hist2 = info->init_hist2[c];
    }

    int rc = 0;
    uint32_t written = 0;
    for (uint32_t block = 0; block < info->block_count; block++) {
        uint32_t frames = brstm_block_sample_count(&b, block);

        if (use_adpc) {
            for (uint32_t c = 0; c < info->channels; c++) {
                state[c].hist1 = adpc[block * info->channels + c][0];
                state[c].hist2 = adpc[block * info->channels + c][1];
            }
        }

        for (uint32_t c = 0; c < info->channels; c++) {
            size_t len = 0;
            st = brstm_read_channel_block(&b, block, c, raw,
                                          info->block_size, &len);
            if (st != BRSTM_OK) {
                fprintf(stderr, "error: cannot read block %u channel %u "
                                "(%s)\n", block, c, brstm_status_string(st));
                rc = 3;
                goto done;
            }
            dsp_adpcm_decode(raw, len, info->coefs[c], &state[c],
                             chbuf, frames);
            for (uint32_t s = 0; s < frames; s++)
                pcm[(size_t)s * info->channels + c] = chbuf[s];
        }

        if (wav_write(&w, pcm, frames) != 0) {
            fprintf(stderr, "error: cannot write '%s'\n", out);
            rc = 3;
            goto done;
        }
        written += frames;
    }

done:
    if (wav_close(&w) != 0 && rc == 0) {
        fprintf(stderr, "error: cannot finalize '%s'\n", out);
        rc = 3;
    }
    if (rc == 0)
        printf("decoded %u samples, %u blocks, %u channels, %u Hz, state=%s\n",
               written, info->block_count, info->channels,
               info->sample_rate, state_mode);
    free(raw); free(pcm); free(chbuf); free(adpc);
    brstm_close(&b);
    return rc;
}
