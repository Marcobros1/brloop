/* brstm_info: print validated BRSTM metadata as key=value lines.
 *
 * Usage: brstm_info <file.brstm>
 * Exit codes: 0 valid, 1 usage, 2 invalid BRSTM, 3 I/O error.
 */
#include "brstm.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.brstm>\n", argc > 0 ? argv[0] : "brstm_info");
        return 1;
    }

    Brstm b;
    char err[256];
    BrstmStatus st = brstm_open(argv[1], &b, err, sizeof err);
    if (st != BRSTM_OK) {
        fprintf(stderr, "error: %s (%s)\n", err, brstm_status_string(st));
        return st == BRSTM_ERR_IO ? 3 : 2;
    }

    const BrstmInfo *i = &b.info;
    printf("codec=%u\n", i->codec);
    printf("channels=%u\n", i->channels);
    printf("sample_rate=%u\n", i->sample_rate);
    printf("loop=%s\n", i->loop ? "true" : "false");
    printf("loop_start=%u\n", i->loop_start);
    printf("loop_end=%u\n", i->total_samples);
    printf("total_samples=%u\n", i->total_samples);
    printf("duration_seconds=%.3f\n", (double)i->total_samples / i->sample_rate);
    printf("block_count=%u\n", i->block_count);
    printf("block_size=%u\n", i->block_size);
    printf("samples_per_block=%u\n", i->samples_per_block);
    printf("final_block_size_nopad=%u\n", i->final_block_size_nopad);
    printf("final_block_samples=%u\n", i->final_block_samples);
    printf("final_block_size_pad=%u\n", i->final_block_size_pad);
    printf("adpcm_start=%u\n", i->adpcm_start);
    printf("has_adpc=%s\n", i->has_adpc ? "true" : "false");
    printf("adpc_entry_count=%u\n", i->adpc_entry_count);

    for (uint32_t c = 0; c < i->channels; c++) {
        printf("ch%u_init_hist1=%d\n", c, i->init_hist1[c]);
        printf("ch%u_init_hist2=%d\n", c, i->init_hist2[c]);
        printf("ch%u_init_predictor_scale=0x%04X\n", c,
               i->init_predictor_scale[c]);
        printf("ch%u_loop_hist1=%d\n", c, i->loop_hist1[c]);
        printf("ch%u_loop_hist2=%d\n", c, i->loop_hist2[c]);
        printf("ch%u_loop_predictor_scale=0x%04X\n", c,
               i->loop_predictor_scale[c]);
    }

    brstm_close(&b);
    return 0;
}
