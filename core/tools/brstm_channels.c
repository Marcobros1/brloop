/* Per-channel analysis of a multichannel BRSTM file.
 *
 * Decodes every channel with the verified core decoder and reports, so
 * channel groups (candidate musical versions or layers) can be identified
 * by evidence instead of assumption:
 *   - level (RMS / peak) and activity per channel,
 *   - Pearson correlation between all channel pairs at sample level
 *     (channels of the same stereo mix correlate strongly),
 *   - correlation of the amplitude envelopes (channels that share musical
 *     timing, even with different timbres, e.g. two covers of the same song).
 *
 * With --wav <dir> it also dumps, for listening:
 *   - one mono WAV per channel           <stem>_chN.wav
 *   - one stereo WAV per adjacent pair   <stem>_pN-N+1.wav
 *   - the even/odd fold the player does  <stem>_fold.wav
 *
 * Usage: brstm_channels <file.brstm> [--wav <dir>]
 */
#include "brstm.h"
#include "dsp_adpcm.h"
#include "wav_writer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CH 16
#define ENV_WIN 1024

static void fail(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static int16_t i16abs(int16_t v)
{
    return v < 0 ? (int16_t)-(int32_t)v : v;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Print the track table (HEAD chunk part 2): the file's own definition of
 * which channels form each track (one track = one stereo/mono version of
 * the music). Format per wiibrew.org/wiki/BRSTM_file. HEAD offsets are
 * relative to HEAD+0x08. Returns 0 on success, -1 if the table is absent
 * or fails validation (reported by the caller). */
static int print_track_table(const char *path, uint32_t nc)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[0x40];
    uint8_t head_hdr[0x20];
    uint8_t part2_hdr[4];
    uint32_t head_off, part2_field, part2_abs;
    uint8_t tracks, type;
    int ok = -1;
    uint32_t t;

    if (!f) {
        printf("\ntrack table: <cannot open file>\n");
        return -1;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr
        || memcmp(hdr, "RSTM", 4) != 0) {
        printf("\ntrack table: <bad RSTM header>\n");
        fclose(f);
        return -1;
    }
    head_off = be32(hdr + 0x10);
    if (fseek(f, (long)head_off, SEEK_SET) != 0
        || fread(head_hdr, 1, sizeof head_hdr, f) != sizeof head_hdr
        || memcmp(head_hdr, "HEAD", 4) != 0) {
        printf("\ntrack table: <bad HEAD chunk>\n");
        fclose(f);
        return -1;
    }
    part2_field = be32(head_hdr + 0x14);
    part2_abs = head_off + 8 + part2_field;
    if (fseek(f, (long)part2_abs, SEEK_SET) != 0
        || fread(part2_hdr, 1, sizeof part2_hdr, f) != sizeof part2_hdr) {
        printf("\ntrack table: <cannot read HEAD part 2>\n");
        fclose(f);
        return -1;
    }
    tracks = part2_hdr[0];
    type = part2_hdr[1];
    printf("\ntrack table (HEAD part 2): %u tracks, description type %u\n",
           tracks, type);

    for (t = 0; t < tracks; t++) {
        uint8_t entry[8];
        uint8_t desc[12];
        uint32_t desc_field, desc_abs;
        uint32_t nch, left, right;
        long entry_pos = (long)(part2_abs + 4 + (size_t)t * 8);

        if (fseek(f, entry_pos, SEEK_SET) != 0
            || fread(entry, 1, sizeof entry, f) != sizeof entry) {
            printf("  track %u: <cannot read entry>\n", t);
            break;
        }
        if (entry[0] != 0x01) {
            printf("  track %u: <bad marker 0x%02X>\n", t, entry[0]);
            break;
        }
        desc_field = be32(entry + 4);
        desc_abs = head_off + 8 + desc_field;
        if (fseek(f, (long)desc_abs, SEEK_SET) != 0
            || fread(desc, 1, sizeof desc, f) != sizeof desc) {
            printf("  track %u: <cannot read description>\n", t);
            break;
        }
        if (entry[1] == 0) {
            /* Type 0: nch, left, right */
            nch = desc[0];
            left = desc[1];
            right = desc[2];
        } else {
            /* Type 1: vol, pan, ... nch, left, right at 0x08..0x0A */
            nch = desc[8];
            left = desc[9];
            right = desc[10];
        }
        if (nch > 2 || left >= nc || (nch == 2 && right >= nc)) {
            printf("  track %u: <implausible nch=%u left=%u right=%u>\\n", t,
                   nch, left, right);
            continue;
        }
        if (entry[1] != 0)
            printf("  track %u: volume=%u pan=%u\n", t, desc[0], desc[1]);
        if (nch == 1)
            printf("  track %u: mono  = ch%u\n", t, left);
        else
            printf("  track %u: stereo = ch%u + ch%u\n", t, left, right);
        ok = 0;
    }
    fclose(f);
    return ok;
}

/* Envelope window buffers (same fill level for every channel). */
static int16_t env_buf[MAX_CH][ENV_WIN];
static uint32_t env_n;

/* Envelope second-moment rows, one per completed window. */
static double *env_row;    /* (max_windows) * MAX_CH */
static uint32_t env_rows;  /* completed windows so far */
static uint32_t env_cap;

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *wavdir = NULL;
    Brstm b;
    BrstmStatus st;
    const BrstmInfo *info;
    char err[512];
    char stem[256];
    uint32_t nc, total, block;
    uint8_t *raw = NULL;
    int16_t *pcm = NULL;
    int16_t *chbuf = NULL;
    DspAdpcmState state[MAX_CH];
    double sum[MAX_CH], sumsq[MAX_CH];
    double cross[MAX_CH][MAX_CH];
    int16_t peak[MAX_CH];
    int have_wav = 0;
    /* Bit-identical detection: a pair stays alive while every sample of the
     * two channels matches exactly; identical pairs are common in original
     * MKW multi-channel streams (duplicated stereo). */
    int identical_alive[MAX_CH][MAX_CH];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc)
            wavdir = argv[++i];
        else if (!path)
            path = argv[i];
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "usage: brstm_channels <file.brstm> [--wav <dir>]\n");
        return 2;
    }

    st = brstm_open(path, &b, err, sizeof err);
    if (st != BRSTM_OK) {
        fprintf(stderr, "error: open %s: %s\n", path, err);
        return 1;
    }
    info = &b.info;
    nc = info->channels;
    total = info->total_samples;
    if (nc > MAX_CH)
        fail("more channels than the analyzer supports");

    {
        const char *base = strrchr(path, '\\');
        size_t len;
        base = base ? base + 1 : path;
        len = strlen(base);
        if (len >= sizeof stem)
            len = sizeof stem - 1;
        memcpy(stem, base, len);
        stem[len] = '\0';
        {
            char *dot = strrchr(stem, '.');
            if (dot)
                *dot = '\0';
        }
    }

    memset(sum, 0, sizeof sum);
    memset(sumsq, 0, sizeof sumsq);
    memset(cross, 0, sizeof cross);
    memset(peak, 0, sizeof peak);
    memset(state, 0, sizeof state);
    memset(identical_alive, 0, sizeof identical_alive);
    for (uint32_t i = 0; i < nc; i++)
        for (uint32_t j = i; j < nc; j++)
            identical_alive[i][j] = 1;
    for (uint32_t c = 0; c < nc; c++) {
        state[c].hist1 = info->init_hist1[c];
        state[c].hist2 = info->init_hist2[c];
    }

    raw = (uint8_t *)malloc(info->block_size);
    pcm = (int16_t *)malloc((size_t)nc * info->samples_per_block
                            * sizeof(int16_t));
    chbuf = (int16_t *)malloc((size_t)info->samples_per_block
                              * sizeof(int16_t));
    if (!raw || !pcm || !chbuf)
        fail("out of memory");

    env_cap = total / ENV_WIN + 2;
    env_row = (double *)malloc((size_t)env_cap * MAX_CH * sizeof(double));
    if (!env_row)
        fail("out of memory (envelope)");

    /* Open the WAV dumps before the decode loop. */
    WavWriter mono[MAX_CH];
    WavWriter pair[MAX_CH];
    WavWriter foldw;
    int pair_open[MAX_CH];
    memset(mono, 0, sizeof mono);
    memset(pair, 0, sizeof pair);
    memset(pair_open, 0, sizeof pair_open);
    memset(&foldw, 0, sizeof foldw);
    if (wavdir) {
        char pth[1024];
        for (uint32_t c = 0; c < nc; c++) {
            snprintf(pth, sizeof pth, "%s\\%s_ch%u.wav", wavdir, stem, c);
            if (wav_open(&mono[c], pth, 1, info->sample_rate) != 0) {
                fprintf(stderr, "error: cannot write %s\n", pth);
                return 1;
            }
        }
        for (uint32_t c = 0; c + 1 < nc; c += 2) {
            snprintf(pth, sizeof pth, "%s\\%s_p%u-%u.wav", wavdir, stem, c,
                     c + 1);
            if (wav_open(&pair[c], pth, 2, info->sample_rate) != 0) {
                fprintf(stderr, "error: cannot write %s\n", pth);
                return 1;
            }
            pair_open[c] = 1;
        }
        snprintf(pth, sizeof pth, "%s\\%s_fold.wav", wavdir, stem);
        if (wav_open(&foldw, pth, 2, info->sample_rate) != 0) {
            fprintf(stderr, "error: cannot write fold wav\n");
            return 1;
        }
        have_wav = 1;
    }

    /* Decode everything, block by block, channel by channel. */
    for (block = 0; block < info->block_count; block++) {
        uint32_t frames = brstm_block_sample_count(&b, block);
        if (frames > info->samples_per_block)
            frames = info->samples_per_block;

        for (uint32_t c = 0; c < nc; c++) {
            size_t len = 0;
            st = brstm_read_channel_block(&b, block, c, raw,
                                          info->block_size, &len);
            if (st != BRSTM_OK)
                fail("cannot read a channel block");
            dsp_adpcm_decode(raw, len, info->coefs[c], &state[c], chbuf,
                             frames);
            memcpy(pcm + (size_t)c * info->samples_per_block, chbuf,
                   (size_t)frames * sizeof(int16_t));
        }

        for (uint32_t c = 0; c < nc; c++) {
            const int16_t *ch = pcm + (size_t)c * info->samples_per_block;
            for (uint32_t s = 0; s < frames; s++) {
                double x = ch[s];
                sum[c] += x;
                sumsq[c] += x * x;
                if (i16abs(ch[s]) > peak[c])
                    peak[c] = i16abs(ch[s]);
            }
        }
        /* Envelope append: all channels share the same window offsets. */
        {
            uint32_t s = 0;
            while (s < frames) {
                uint32_t room = ENV_WIN - env_n;
                uint32_t take = frames - s < room ? frames - s : room;
                for (uint32_t c = 0; c < nc; c++) {
                    const int16_t *ch = pcm
                        + (size_t)c * info->samples_per_block;
                    memcpy(env_buf[c] + env_n, ch + s,
                           (size_t)take * sizeof(int16_t));
                }
                env_n += take;
                s += take;
                if (env_n == ENV_WIN) {
                    double *row = env_row + (size_t)env_rows * MAX_CH;
                    for (uint32_t c = 0; c < nc; c++) {
                        double acc = 0;
                        for (uint32_t k = 0; k < ENV_WIN; k++) {
                            double x = env_buf[c][k];
                            acc += x * x;
                        }
                        row[c] = acc / ENV_WIN;
                    }
                    env_rows++;
                    env_n = 0;
                }
            }
        }

        if (have_wav) {
            for (uint32_t c = 0; c < nc; c++) {
                const int16_t *ch = pcm + (size_t)c * info->samples_per_block;
                if (wav_write(&mono[c], ch, frames) != 0)
                    fail("wav write failed (mono)");
            }
            for (uint32_t c = 0; c + 1 < nc; c += 2) {
                if (!pair_open[c])
                    continue;
                {
                    const int16_t *a = pcm
                        + (size_t)c * info->samples_per_block;
                    const int16_t *bb = pcm
                        + (size_t)(c + 1) * info->samples_per_block;
                    int16_t *dst = (int16_t *)malloc((size_t)frames * 2
                                                      * sizeof(int16_t));
                    if (!dst)
                        fail("out of memory (pair)");
                    for (uint32_t s = 0; s < frames; s++) {
                        dst[s * 2] = a[s];
                        dst[s * 2 + 1] = bb[s];
                    }
                    if (wav_write(&pair[c], dst, frames) != 0)
                        fail("wav write failed (pair)");
                    free(dst);
                }
            }
            {
                /* Same even/odd fold as audio_out.c for reference. */
                uint32_t nl = (nc + 1) / 2, nr = nc / 2;
                int16_t *dst = (int16_t *)malloc((size_t)frames * 2
                                                  * sizeof(int16_t));
                if (!dst)
                    fail("out of memory (fold)");
                for (uint32_t s = 0; s < frames; s++) {
                    int32_t l = 0, r = 0;
                    for (uint32_t c = 0; c < nc; c++) {
                        int32_t v = pcm[(size_t)c * info->samples_per_block
                                        + s];
                        if ((c & 1) == 0)
                            l += v;
                        else
                            r += v;
                    }
                    dst[s * 2] = (int16_t)(l / (int32_t)nl);
                    dst[s * 2 + 1] = (int16_t)(r / (int32_t)nr);
                }
                if (wav_write(&foldw, dst, frames) != 0)
                    fail("wav write failed (fold)");
                free(dst);
            }
        }

        /* Sample-level cross products (upper triangle computed later from
         * full sums: accumulate all pairs here, it is nc^2 * frames). */
        for (uint32_t i = 0; i < nc; i++) {
            const int16_t *a = pcm + (size_t)i * info->samples_per_block;
            for (uint32_t j = i; j < nc; j++) {
                const int16_t *c2 = pcm
                    + (size_t)j * info->samples_per_block;
                double acc = 0;
                for (uint32_t s = 0; s < frames; s++)
                    acc += (double)a[s] * c2[s];
                cross[i][j] += acc;
                if (i < j && identical_alive[i][j]) {
                    for (uint32_t s = 0; s < frames; s++) {
                        if (a[s] != c2[s]) {
                            identical_alive[i][j] = 0;
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Flush a trailing partial envelope window. */
    if (env_n > 0 && env_rows < env_cap) {
        double *row = env_row + (size_t)env_rows * MAX_CH;
        for (uint32_t c = 0; c < nc; c++) {
            double acc = 0;
            for (uint32_t k = 0; k < env_n; k++) {
                double x = env_buf[c][k];
                acc += x * x;
            }
            row[c] = acc / env_n;
        }
        env_rows++;
        env_n = 0;
    }

    if (have_wav) {
        for (uint32_t c = 0; c < nc; c++)
            if (wav_close(&mono[c]) != 0)
                fail("wav close failed");
        for (uint32_t c = 0; c + 1 < nc; c += 2)
            if (pair_open[c] && wav_close(&pair[c]) != 0)
                fail("wav close failed (pair)");
        if (wav_close(&foldw) != 0)
            fail("wav close failed (fold)");
    }

    /* ---------------------------------------------------------- report -- */
    printf("file: %s\n", path);
    printf("rate=%u channels=%u loop=%d loop_start=%u total=%u "
           "blocks=%u samples_per_block=%u windows=%u\n",
           info->sample_rate, nc, info->loop ? 1 : 0, info->loop_start,
           total, info->block_count, info->samples_per_block, env_rows);

    print_track_table(path, nc);

    printf("\nchannel levels (full file):\n");
    printf("  ch      rms    peak\n");
    for (uint32_t c = 0; c < nc; c++) {
        double rms = sqrt(sumsq[c] / (double)total);
        printf("  %2u  %7.0f  %6d\n", c, rms, peak[c]);
    }

    /* Activity: fraction of envelope windows above 8% of the channel peak
     * window (silence detection: intro/outro rests or muted channels). */
    printf("\nchannel activity (windows above 8%% of own peak):\n");
    for (uint32_t c = 0; c < nc; c++) {
        double mx = 0;
        uint32_t active = 0;
        for (uint32_t w = 0; w < env_rows; w++)
            if (env_row[(size_t)w * MAX_CH + c] > mx)
                mx = env_row[(size_t)w * MAX_CH + c];
        for (uint32_t w = 0; w < env_rows; w++)
            if (env_row[(size_t)w * MAX_CH + c] > mx * 0.08)
                active++;
        printf("  %2u  %5.1f%%  (rms of active windows vs all)\n", c,
               env_rows ? 100.0 * active / env_rows : 0.0);
    }

    /* Bit-identical pairs: exact duplicated content inside the file. */
    {
        int found = 0;
        for (uint32_t i = 0; i < nc && !found; i++)
            for (uint32_t j = i + 1; j < nc; j++)
                if (identical_alive[i][j]) {
                    if (!found) {
                        printf("\nbit-identical channel pairs:\n");
                        found = 1;
                    }
                    printf("  ch%u == ch%u\\n", i, j);
                }
        if (!found)
            printf("\nbit-identical channel pairs: none\\n");
    }

    /* Sample-level Pearson correlation. */
    printf("\ncorrelation (samples, Pearson):\n     ");
    for (uint32_t c = 0; c < nc; c++)
        printf(" %6u", c);
    printf("\n");
    for (uint32_t i = 0; i < nc; i++) {
        printf("  %2u ", i);
        for (uint32_t j = 0; j < nc; j++) {
            double xy = (i <= j) ? cross[i][j] : cross[j][i];
            double sx = sum[i], sy = sum[j];
            double nx = (double)total * sumsq[i] - sx * sx;
            double ny = (double)total * sumsq[j] - sy * sy;
            double r = 0;
            if (nx > 0 && ny > 0)
                r = ((double)total * xy - sx * sy) / sqrt(nx * ny);
            printf(" %6.2f", r);
        }
        printf("\n");
    }

    /* Envelope (energy) correlation: shared musical timing. */
    if (env_rows > 1) {
        double esum[MAX_CH] = {0}, esumsq[MAX_CH] = {0};
        double ecross[MAX_CH][MAX_CH];
        memset(ecross, 0, sizeof ecross);
        for (uint32_t w = 0; w < env_rows; w++) {
            for (uint32_t i = 0; i < nc; i++) {
                double v = env_row[(size_t)w * MAX_CH + i];
                esum[i] += v;
                esumsq[i] += v * v;
                for (uint32_t j = i; j < nc; j++)
                    ecross[i][j] += v * env_row[(size_t)w * MAX_CH + j];
            }
        }
        printf("\ncorrelation (envelope, %u-sample windows):\n     ",
               (unsigned)ENV_WIN);
        for (uint32_t c = 0; c < nc; c++)
            printf(" %6u", c);
        printf("\n");
        for (uint32_t i = 0; i < nc; i++) {
            printf("  %2u ", i);
            for (uint32_t j = 0; j < nc; j++) {
                double xy = (i <= j) ? ecross[i][j] : ecross[j][i];
                double nx = (double)env_rows * esumsq[i] - esum[i] * esum[i];
                double ny = (double)env_rows * esumsq[j] - esum[j] * esum[j];
                double r = 0;
                if (nx > 0 && ny > 0)
                    r = ((double)env_rows * xy - esum[i] * esum[j])
                        / sqrt(nx * ny);
                printf(" %6.2f", r);
            }
            printf("\n");
        }
    }

    if (have_wav)
        printf("\nwav dumps written to: %s\n", wavdir);

    brstm_close(&b);
    free(raw);
    free(pcm);
    free(chbuf);
    free(env_row);
    return 0;
}
