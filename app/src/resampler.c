#include "resampler.h"

#include <string.h>

/* Catmull-Rom through history frames p0..p3; the output lies between p1 and
 * p2 at fraction t = frac / RESAMPLER_ONE. Integer arithmetic: t is kept in
 * Q15 and the polynomial is evaluated in 64 bits, so the result is the same
 * on every run. */
static int16_t cubic(int32_t p0, int32_t p1, int32_t p2, int32_t p3, uint32_t frac)
{
    int64_t t = ((int64_t)frac << 15) >> 16; /* Q15 fraction in [0, 1) */
    int64_t t2 = (t * t) >> 15;
    int64_t t3 = (t2 * t) >> 15;

    /* 2 * v(t) = 2*p1 + (p2 - p0)*t + (2*p0 - 5*p1 + 4*p2 - p3)*t^2
     *                + (-p0 + 3*p1 - 3*p2 + p3)*t^3, all in Q15. */
    int64_t acc = 2 * p1 * (1 << 15)
                + (p2 - p0) * t
                + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2
                + (-p0 + 3 * p1 - 3 * p2 + p3) * t3;
    int64_t v = (acc + (1 << 15)) >> 16; /* divide by 2 (weights) and by Q15 */

    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

uint32_t resampler_step(uint32_t sample_rate, uint32_t playback_rate)
{
    if (sample_rate == 0 || playback_rate == 0)
        return 0;
    return (uint32_t)(((uint64_t)playback_rate << 16) / sample_rate);
}

void resampler_reset(Resampler *r, uint32_t channels)
{
    memset(r, 0, sizeof *r);
    r->channels = channels;
}

size_t resampler_render(Resampler *r, uint32_t step, ResamplerPull pull, void *ctx,
                        int16_t *out, size_t out_frames)
{
    const uint32_t ch = r->channels;
    size_t produced = 0;

    if (step == RESAMPLER_ONE) {
        /* Exact path: copy frames without touching them. */
        while (produced < out_frames) {
            if (!pull(ctx, out + produced * ch))
                break;
            produced++;
        }
        return produced;
    }

    while (produced < out_frames) {
        /* Advance the source position until the output lies between the
         * two newest frames: history[2] <= position < history[3]. */
        while (r->frac >= RESAMPLER_ONE) {
            int16_t frame[BRSTM_MAX_CHANNELS];
            if (!pull(ctx, frame))
                return produced;
            memmove(r->history[0], r->history[1], 3 * sizeof r->history[0]);
            memcpy(r->history[3], frame, ch * sizeof(int16_t));
            r->frac -= RESAMPLER_ONE;
        }

        for (uint32_t c = 0; c < ch; c++) {
            out[produced * ch + c] = cubic(r->history[0][c], r->history[1][c],
                                           r->history[2][c], r->history[3][c], r->frac);
        }
        produced++;
        r->frac += step;
    }
    return produced;
}
