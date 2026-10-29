#ifndef RESAMPLER_H
#define RESAMPLER_H

#include "brstm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sample-rate converter for variable playback speed. Reads source frames at
 * a fractional position and interpolates them with a 4-point cubic
 * (Catmull-Rom). Positions are 16.16 fixed point, so the step is exact and
 * accumulates no float drift. A step of exactly 1.0 (0x10000) copies the
 * source frames unchanged. */
#define RESAMPLER_ONE 0x10000u

typedef struct {
    uint32_t frac;                              /* fraction of the next source frame */
    int16_t history[4][BRSTM_MAX_CHANNELS];     /* the four most recent source frames */
    uint32_t channels;
} Resampler;

/* Source frames consumed per output frame, 16.16. For a source at
 * `sample_rate` played at `playback_rate` this is playback_rate / sample_rate,
 * the same ratio Final Lap Stream Maker produces by rewriting the rate field
 * (44100 -> 47104 gives 1.06812). Returns 0 when either rate is zero. */
uint32_t resampler_step(uint32_t sample_rate, uint32_t playback_rate);

void resampler_reset(Resampler *r, uint32_t channels);

/* Pulls one source frame (`channels` samples) into `frame`. Returns 1 when a
 * frame was produced and 0 when the source is exhausted. */
typedef int (*ResamplerPull)(void *ctx, int16_t *frame);

/* Writes up to `out_frames` interleaved output frames and returns how many
 * were written. A short count means the source was exhausted. */
size_t resampler_render(Resampler *r, uint32_t step, ResamplerPull pull, void *ctx,
                        int16_t *out, size_t out_frames);

#ifdef __cplusplus
}
#endif

#endif /* RESAMPLER_H */
