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

#define MAX_CH 16   /* mirrors BRSTM_MAX_CHANNELS; the analysis buffers are
                     * fixed-size arrays, so the tool refuses larger files */
#define ENV_WIN 1024 /* envelope window in samples (~23 ms at 44.1 kHz):
                      * fine enough to follow musical phrasing, coarse
                      * enough to ignore single-sample detail */

/* Report a fatal error and return 1. Functions that hold resources return
 * this to main() instead of exiting, so main() releases everything on every
 * path. */
static int fail(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    return 1;
}

/* i16abs with an int32 intermediate: -(-32768) is not representable in
 * int16_t, so the naive ternary would overflow. */
static int16_t i16abs(int16_t v)
{
    return v < 0 ? (int16_t)-(int32_t)v : v;
}

/* Big-endian read; the tool re-parses the header itself for the track table
 * (brstm.h deliberately does not expose HEAD part 2). */
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
            printf("  track %u: <implausible nch=%u left=%u right=%u>\n", t,
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

/* ------------------------------------------------------------- statistics */

/* Whole-file accumulators for one analysis run. Kept together because every
 * report section reads more than one of them and they must all describe the
 * same decode pass. */
typedef struct {
    double sum[MAX_CH];          /* sum of samples, per channel (for Pearson)*/
    double sumsq[MAX_CH];        /* sum of squares (variance + RMS) */
    double cross[MAX_CH][MAX_CH];/* sum of a[i]*b[i], upper triangle only */
    int16_t peak[MAX_CH];        /* max |sample| */
    /* Bit-identical detection: a pair stays alive while every sample of the
     * two channels matches exactly; identical pairs are common in original
     * MKW multi-channel streams (duplicated stereo). */
    int identical_alive[MAX_CH][MAX_CH];
} ChannelStats;

static void stats_init(ChannelStats *st, uint32_t nc)
{
    memset(st, 0, sizeof *st);
    for (uint32_t i = 0; i < nc; i++)
        for (uint32_t j = i; j < nc; j++)
            st->identical_alive[i][j] = 1;
}

/* --------------------------------------------------------------- envelope */

/* Amplitude-envelope rows, one per completed ENV_WIN-sample window. The
 * window buffer and the row array are shared by all channels (every channel
 * is at the same playback position), which is why this is one struct rather
 * than per-channel state. */
typedef struct {
    int16_t buf[MAX_CH][ENV_WIN]; /* current partial window, per channel */
    uint32_t n;                   /* samples filled in buf so far */
    double *row;                  /* (cap) * MAX_CH second moments */
    uint32_t rows;                /* completed windows so far */
    uint32_t cap;                 /* allocated row capacity */
} Envelope;

/* Returns 0 on success, or -1 when the row buffer cannot be allocated. */
static int envelope_init(Envelope *env, uint32_t total_samples)
{
    env->n = 0;
    env->rows = 0;
    env->row = NULL;
    /* One row per full window, plus one for the trailing partial window and
     * one spare so the final flush always fits. */
    env->cap = total_samples / ENV_WIN + 2;
    env->row = (double *)malloc((size_t)env->cap * MAX_CH * sizeof(double));
    return env->row ? 0 : -1;
}

/* Append one decoded block slice to the envelope. pcm is channel-major with
 * the given stride (samples_per_block), so each channel copies from its own
 * row. Completed windows are reduced to a mean of squares immediately, which
 * keeps the retained data at one double per channel per 1024 samples instead
 * of the full PCM. The caller must guarantee nc <= MAX_CH: env->buf has one
 * slot per channel and main() rejects wider files before decoding starts. */
static void envelope_append(Envelope *env, const int16_t *pcm, uint32_t stride,
                            uint32_t nc, uint32_t frames)
{
    uint32_t s = 0;
    while (s < frames) {
        uint32_t room = ENV_WIN - env->n;
        uint32_t take = frames - s < room ? frames - s : room;
        for (uint32_t c = 0; c < nc; c++)
            memcpy(env->buf[c] + env->n, pcm + (size_t)c * stride + s,
                   (size_t)take * sizeof(int16_t));
        env->n += take;
        s += take;
        if (env->n == ENV_WIN) {
            double *row = env->row + (size_t)env->rows * MAX_CH;
            for (uint32_t c = 0; c < nc; c++) {
                double acc = 0;
                for (uint32_t k = 0; k < ENV_WIN; k++) {
                    double x = env->buf[c][k];
                    acc += x * x;
                }
                row[c] = acc / ENV_WIN;
            }
            env->rows++;
            env->n = 0;
        }
    }
}

/* Reduce a trailing partial window so the final (usually short) stretch of
 * the file still counts; without this the last <1024 samples would be
 * silently dropped from the activity report. */
static void envelope_flush(Envelope *env, uint32_t nc)
{
    if (env->n > 0 && env->rows < env->cap) {
        double *row = env->row + (size_t)env->rows * MAX_CH;
        for (uint32_t c = 0; c < nc; c++) {
            double acc = 0;
            for (uint32_t k = 0; k < env->n; k++) {
                double x = env->buf[c][k];
                acc += x * x;
            }
            row[c] = acc / env->n;
        }
        env->rows++;
        env->n = 0;
    }
}

static void envelope_free(Envelope *env)
{
    free(env->row);
    env->row = NULL;
}

/* ------------------------------------------------------------- WAV dumps */

/* Optional listening material. Opened before the decode loop (so a bad
 * output directory fails fast instead of after decoding the whole file) and
 * closed after it. pair_open[] exists because pair writers are created only
 * for even channel pairs. */
typedef struct {
    int enabled;
    const char *dir;
    const char *stem;
    WavWriter mono[MAX_CH];
    WavWriter pair[MAX_CH];
    WavWriter fold;              /* even/odd fold, what the player plays */
    int pair_open[MAX_CH];
    int16_t *scratch;            /* shared interleave buffer, one block wide */
} WavDumps;

static int dumps_close(WavDumps *d);

/* Open every WAV dump and allocate the shared scratch buffer. On failure the
 * files opened so far are closed before returning -1, so no handle leaks. */
static int dumps_open(WavDumps *d, const char *dir, const char *stem,
                      const BrstmInfo *info)
{
    char pth[1024];

    memset(d, 0, sizeof *d);
    d->dir = dir;
    d->stem = stem;
    if (!dir)
        return 0;

    d->scratch = (int16_t *)malloc((size_t)info->samples_per_block * 2
                                   * sizeof(int16_t));
    if (!d->scratch)
        return -1;

    for (uint32_t c = 0; c < info->channels; c++) {
        snprintf(pth, sizeof pth, "%s\\%s_ch%u.wav", dir, stem, c);
        if (wav_open(&d->mono[c], pth, 1, info->sample_rate) != 0) {
            fprintf(stderr, "error: cannot write %s\n", pth);
            dumps_close(d);
            return -1;
        }
    }
    for (uint32_t c = 0; c + 1 < info->channels; c += 2) {
        snprintf(pth, sizeof pth, "%s\\%s_p%u-%u.wav", dir, stem, c, c + 1);
        if (wav_open(&d->pair[c], pth, 2, info->sample_rate) != 0) {
            fprintf(stderr, "error: cannot write %s\n", pth);
            dumps_close(d);
            return -1;
        }
        d->pair_open[c] = 1;
    }
    snprintf(pth, sizeof pth, "%s\\%s_fold.wav", dir, stem);
    if (wav_open(&d->fold, pth, 2, info->sample_rate) != 0) {
        fprintf(stderr, "error: cannot write fold wav\n");
        dumps_close(d);
        return -1;
    }
    d->enabled = 1;
    return 0;
}

/* Write this block's PCM to every open dump. Returns 0 on success, or -1 on a
 * write error. Uses the scratch buffer allocated by dumps_open, so the decode
 * loop does no allocation here. */
static int dumps_write_block(WavDumps *d, const int16_t *pcm, uint32_t stride,
                             uint32_t nc, uint32_t frames)
{
    if (!d->enabled)
        return 0;

    for (uint32_t c = 0; c < nc; c++)
        if (wav_write(&d->mono[c], pcm + (size_t)c * stride, frames) != 0)
            return -1;

    for (uint32_t c = 0; c + 1 < nc; c += 2) {
        if (!d->pair_open[c])
            continue;
        for (uint32_t s = 0; s < frames; s++) {
            d->scratch[s * 2] = pcm[(size_t)c * stride + s];
            d->scratch[s * 2 + 1] = pcm[(size_t)(c + 1) * stride + s];
        }
        if (wav_write(&d->pair[c], d->scratch, frames) != 0)
            return -1;
    }

    {   /* Same even/odd fold as audio_out.c for reference. */
        uint32_t nl = (nc + 1) / 2, nr = nc / 2;
        if (nl == 0)
            return -1; /* no channels: nothing to fold */
        for (uint32_t s = 0; s < frames; s++) {
            int32_t l = 0, r = 0;
            for (uint32_t c = 0; c < nc; c++) {
                int32_t v = pcm[(size_t)c * stride + s];
                if ((c & 1) == 0)
                    l += v;
                else
                    r += v;
            }
            d->scratch[s * 2] = (int16_t)(l / (int32_t)nl);
            /* Mono has no right-hand channel: copy the mono signal to both
             * sides so the fold is audible in both ears. */
            d->scratch[s * 2 + 1] = nr > 0 ? (int16_t)(r / (int32_t)nr)
                                           : d->scratch[s * 2];
        }
        if (wav_write(&d->fold, d->scratch, frames) != 0)
            return -1;
    }
    return 0;
}

/* Close every writer that is open and free the scratch buffer. Safe to call
 * on a partially opened set: only handles that were actually opened are
 * touched. Returns 0, or -1 if a close reported an error. */
static int dumps_close(WavDumps *d)
{
    int rc = 0;
    for (uint32_t c = 0; c < MAX_CH; c++) {
        if (d->mono[c].file && wav_close(&d->mono[c]) != 0)
            rc = -1;
        if (d->pair_open[c] && d->pair[c].file && wav_close(&d->pair[c]) != 0)
            rc = -1;
    }
    if (d->fold.file && wav_close(&d->fold) != 0)
        rc = -1;
    free(d->scratch);
    d->scratch = NULL;
    d->enabled = 0;
    return rc;
}

/* ---------------------------------------------------------------- decode */

/* Decode the whole file block by block and feed the statistics, envelope and
 * WAV dumps. Carries ADPCM state across blocks (the analyzer measures the
 * file as the player plays it; ADPC per-block states are not needed here).
 * Temp buffers live inside this function: they are meaningless outside one
 * decode pass. */
/* Returns 0 on success or 1 on error. Every buffer allocated here is released
 * on the single exit path below, whichever step fails. */
static int decode_file(Brstm *b, ChannelStats *st, Envelope *env,
                       WavDumps *dumps)
{
    const BrstmInfo *info = &b->info;
    const uint32_t nc = info->channels;
    const uint32_t stride = info->samples_per_block;
    DspAdpcmState state[MAX_CH];
    uint8_t *raw = NULL;
    int16_t *pcm = NULL;
    int16_t *chbuf = NULL;
    BrstmStatus stcode;
    int rc = 0;

    raw = (uint8_t *)malloc(info->block_size);
    pcm = (int16_t *)malloc((size_t)nc * stride * sizeof(int16_t));
    chbuf = (int16_t *)malloc((size_t)stride * sizeof(int16_t));
    if (!raw || !pcm || !chbuf) {
        rc = fail("out of memory");
        goto done;
    }
    memset(state, 0, sizeof state);
    for (uint32_t c = 0; c < nc; c++) { /* initial histories from HEAD p.3 */
        state[c].hist1 = info->init_hist1[c];
        state[c].hist2 = info->init_hist2[c];
    }

    for (uint32_t block = 0; block < info->block_count; block++) {
        uint32_t frames = brstm_block_sample_count(b, block);
        if (frames > stride)
            frames = stride;

        for (uint32_t c = 0; c < nc; c++) {
            size_t len = 0;
            stcode = brstm_read_channel_block(b, block, c, raw,
                                              info->block_size, &len);
            if (stcode != BRSTM_OK) {
                rc = fail("cannot read a channel block");
                goto done;
            }
            dsp_adpcm_decode(raw, len, info->coefs[c], &state[c], chbuf,
                             frames);
            memcpy(pcm + (size_t)c * stride, chbuf,
                   (size_t)frames * sizeof(int16_t));
        }

        for (uint32_t c = 0; c < nc; c++) {
            const int16_t *ch = pcm + (size_t)c * stride;
            for (uint32_t s = 0; s < frames; s++) {
                double x = ch[s];
                st->sum[c] += x;
                st->sumsq[c] += x * x;
                if (i16abs(ch[s]) > st->peak[c])
                    st->peak[c] = i16abs(ch[s]);
            }
        }

        envelope_append(env, pcm, stride, nc, frames);
        if (dumps_write_block(dumps, pcm, stride, nc, frames) != 0) {
            rc = fail("wav write failed");
            goto done;
        }

        /* Sample-level cross products for the Pearson matrix. The whole
         * upper triangle is accumulated here at nc^2 * frames; reducing it
         * later keeps this loop branch-free. */
        for (uint32_t i = 0; i < nc; i++) {
            const int16_t *a = pcm + (size_t)i * stride;
            for (uint32_t j = i; j < nc; j++) {
                const int16_t *c2 = pcm + (size_t)j * stride;
                double acc = 0;
                for (uint32_t s = 0; s < frames; s++)
                    acc += (double)a[s] * c2[s];
                st->cross[i][j] += acc;
                if (i < j && st->identical_alive[i][j]) {
                    for (uint32_t s = 0; s < frames; s++) {
                        if (a[s] != c2[s]) {
                            st->identical_alive[i][j] = 0;
                            break;
                        }
                    }
                }
            }
        }
    }

done:
    free(raw);
    free(pcm);
    free(chbuf);
    return rc;
}

/* ---------------------------------------------------------------- report */

/* Pearson correlation from the accumulated sums, for the pair (i, j). The
 * nx/ny terms are total*sumsq - sum^2 (n times the variance); returning 0
 * for a constant channel avoids dividing by zero (a silent channel has zero
 * variance and simply has no correlation to report). */
static double pearson(const ChannelStats *st, uint32_t total,
                      uint32_t i, uint32_t j)
{
    double xy = (i <= j) ? st->cross[i][j] : st->cross[j][i];
    double sx = st->sum[i], sy = st->sum[j];
    double nx = (double)total * st->sumsq[i] - sx * sx;
    double ny = (double)total * st->sumsq[j] - sy * sy;
    if (nx <= 0 || ny <= 0)
        return 0;
    return ((double)total * xy - sx * sy) / sqrt(nx * ny);
}

/* Same formula over envelope rows instead of samples: channels that move
 * together in energy correlate here even when their waveforms differ (two
 * covers of the same arrangement), which sample correlation alone misses.
 * Sums are accumulated for this (i, j) pair directly - identical result to
 * the full cross matrix the previous version built, without retaining it. */
static double envelope_pearson(const Envelope *env, uint32_t i, uint32_t j)
{
    double si = 0, sj = 0;    /* sum of each channel's window energies */
    double sii = 0, sjj = 0;  /* sum of squares of each */
    double sij = 0;           /* sum of products (the cross term) */
    double nx, ny;

    for (uint32_t w = 0; w < env->rows; w++) {
        double a = env->row[(size_t)w * MAX_CH + i];
        double b = env->row[(size_t)w * MAX_CH + j];
        si += a;
        sj += b;
        sii += a * a;
        sjj += b * b;
        sij += a * b;
    }
    nx = (double)env->rows * sii - si * si;
    ny = (double)env->rows * sjj - sj * sj;
    if (nx <= 0 || ny <= 0)
        return 0;
    return ((double)env->rows * sij - si * sj) / sqrt(nx * ny);
}

/* Print the full analysis: header, track table, levels, activity,
 * bit-identical pairs and both correlation matrices. Pure reporting: it
 * reads state but never mutates the decode results. */
static void print_report(const char *path, const BrstmInfo *info,
                         const ChannelStats *st, const Envelope *env)
{
    const uint32_t nc = info->channels;
    const uint32_t total = info->total_samples;

    printf("file: %s\n", path);
    printf("rate=%u channels=%u loop=%d loop_start=%u total=%u "
           "blocks=%u samples_per_block=%u windows=%u\n",
           info->sample_rate, nc, info->loop ? 1 : 0, info->loop_start,
           total, info->block_count, info->samples_per_block, env->rows);

    print_track_table(path, nc);

    printf("\nchannel levels (full file):\n");
    printf("  ch      rms    peak\n");
    for (uint32_t c = 0; c < nc; c++) {
        double rms = sqrt(st->sumsq[c] / (double)total);
        printf("  %2u  %7.0f  %6d\n", c, rms, st->peak[c]);
    }

    /* Activity: fraction of envelope windows above 8% of the channel peak
     * window (silence detection: intro/outro rests or muted channels). */
    printf("\nchannel activity (windows above 8%% of own peak):\n");
    for (uint32_t c = 0; c < nc; c++) {
        double mx = 0;
        uint32_t active = 0;
        for (uint32_t w = 0; w < env->rows; w++)
            if (env->row[(size_t)w * MAX_CH + c] > mx)
                mx = env->row[(size_t)w * MAX_CH + c];
        for (uint32_t w = 0; w < env->rows; w++)
            if (env->row[(size_t)w * MAX_CH + c] > mx * 0.08)
                active++;
        printf("  %2u  %5.1f%%  (rms of active windows vs all)\n", c,
               env->rows ? 100.0 * active / env->rows : 0.0);
    }

    /* Bit-identical pairs: exact duplicated content inside the file. */
    {
        int found = 0;
        for (uint32_t i = 0; i < nc && !found; i++)
            for (uint32_t j = i + 1; j < nc; j++)
                if (st->identical_alive[i][j]) {
                    if (!found) {
                        printf("\nbit-identical channel pairs:\n");
                        found = 1;
                    }
                    printf("  ch%u == ch%u\n", i, j);
                }
        if (!found)
            printf("\nbit-identical channel pairs: none\n");
    }

    printf("\ncorrelation (samples, Pearson):\n     ");
    for (uint32_t c = 0; c < nc; c++)
        printf(" %6u", c);
    printf("\n");
    for (uint32_t i = 0; i < nc; i++) {
        printf("  %2u ", i);
        for (uint32_t j = 0; j < nc; j++)
            printf(" %6.2f", pearson(st, total, i, j));
        printf("\n");
    }

    if (env->rows > 1) {
        printf("\ncorrelation (envelope, %u-sample windows):\n     ",
               (unsigned)ENV_WIN);
        for (uint32_t c = 0; c < nc; c++)
            printf(" %6u", c);
        printf("\n");
        for (uint32_t i = 0; i < nc; i++) {
            printf("  %2u ", i);
            for (uint32_t j = 0; j < nc; j++)
                printf(" %6.2f", envelope_pearson(env, i, j));
            printf("\n");
        }
    }
}

/* ------------------------------------------------------------------ main */

/* Everything above the decode is wiring: arguments, open, stem for output
 * names, then one pass through decode_file() and the report. */
int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *wavdir = NULL;
    Brstm b;
    BrstmStatus st;
    const BrstmInfo *info;
    char err[512];
    char stem[256];
    ChannelStats stats;
    Envelope env;
    WavDumps dumps;
    int rc = 0;

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
    if (info->channels > MAX_CH) {
        rc = fail("more channels than the analyzer supports");
        brstm_close(&b);
        return rc;
    }

    {   /* Output basename: file name without directory or extension, used
         * as the prefix of every WAV dump. */
        const char *base = strrchr(path, '\\');
        size_t len;
        char *dot;
        base = base ? base + 1 : path;
        len = strlen(base);
        if (len >= sizeof stem)
            len = sizeof stem - 1;
        memcpy(stem, base, len);
        stem[len] = '\0';
        dot = strrchr(stem, '.');
        if (dot)
            *dot = '\0';
    }

    stats_init(&stats, info->channels);
    if (envelope_init(&env, info->total_samples) != 0) {
        rc = fail("out of memory (envelope)");
        goto close_brstm;
    }
    if (dumps_open(&dumps, wavdir, stem, info) != 0) {
        rc = fail("cannot open the WAV dumps");
        goto free_envelope;
    }

    if (decode_file(&b, &stats, &env, &dumps) != 0) {
        rc = 1;
        goto close_dumps;
    }
    envelope_flush(&env, info->channels);
    if (dumps_close(&dumps) != 0)
        rc = fail("wav close failed");
    else
        print_report(path, info, &stats, &env);
    goto free_envelope;

close_dumps:
    dumps_close(&dumps);
free_envelope:
    envelope_free(&env);
close_brstm:
    brstm_close(&b);
    return rc;
}
