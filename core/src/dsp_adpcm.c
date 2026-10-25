#include "dsp_adpcm.h"

/* Saturate to int16 instead of wrapping: the predictor sum can exceed the
 * 16-bit range on loud frames, and C's cast to int16_t is implementation-
 * defined for out-of-range values, so the clamp also makes the output
 * reproducible across compilers (byte-identical vs vgmstream is a hard
 * acceptance test). */
static int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Frame-by-frame decode of one channel block; algorithm and frame layout in
 * dsp_adpcm.h. Histories are kept in locals for speed and written back once
 * so *state describes the position exactly after any return, including the
 * truncated-data path (the loop-jump code relies on that: it captures the
 * state entering the loop block, see player.c). */
uint32_t dsp_adpcm_decode(const uint8_t *data, size_t len,
                          const int16_t coefs[DSP_ADPCM_PREDICTORS][2],
                          DspAdpcmState *state,
                          int16_t *out, uint32_t samples)
{
    uint32_t done = 0;  /* samples written to out so far */
    size_t pos = 0;     /* read cursor in data (frame headers + nibbles) */
    int32_t hist1 = state->hist1;  /* previous sample (n-1) */
    int32_t hist2 = state->hist2;  /* sample before that (n-2) */

    while (done < samples) {
        /* No data left: fall through to the zero-fill below. */
        if (pos >= len)
            break;

        /* Frame header byte: [7:4] predictor pair index, [3:0] scale shift.
         * Index 8-15 cannot exist in the coefficient table (only 8 pairs are
         * stored), so it clamps to 7 rather than erroring: a real file never
         * does this, and clamping keeps malformed input decodable instead of
         * crashing or producing garbage pointers. */
        uint8_t ps = data[pos++];
        int predictor = ps >> 4;
        int scale = ps & 0x0F;
        if (predictor >= DSP_ADPCM_PREDICTORS)
            predictor = DSP_ADPCM_PREDICTORS - 1; /* nibble could ask 8-15 */
        int32_t coef1 = coefs[predictor][0];
        int32_t coef2 = coefs[predictor][1];
        int32_t distance_base = (int32_t)1 << (scale + 11); /* (1<<scale)*2048 */

        /* How many samples this frame yields: never more than the caller
         * still needs, never more than the frame's 14, never more than the
         * bytes actually present. */
        uint32_t frame_samples = samples - done;
        if (frame_samples > DSP_ADPCM_SAMPLES_PER_FRAME)
            frame_samples = DSP_ADPCM_SAMPLES_PER_FRAME;
        /* Each remaining byte carries two samples (nibbles). */
        if (frame_samples > (uint32_t)((len - pos) * 2))
            frame_samples = (uint32_t)((len - pos) * 2);
        if (frame_samples == 0)
            break;

        for (uint32_t i = 0; i < frame_samples; i++) {
            /* High nibble first; 4-bit two's complement (0..7 stay, 8..15
             * become -8..-1). */
            int nibble = (i & 1) ? (data[pos + i / 2] & 0x0F)
                                 : (data[pos + i / 2] >> 4);
            if (nibble >= 8)
                nibble -= 16;

            /* Prediction + quantized distance, rounded (+1024) and scaled
             * (>>11): the fixed-point form of the formula documented in
             * dsp_adpcm.h. Histories update with the *clamped* sample, which
             * is what the reference decoders do. */
            int32_t corrected = coef1 * hist1 + coef2 * hist2
                              + nibble * distance_base;
            int16_t sample = clamp16((corrected + 1024) >> 11);

            out[done + i] = sample;
            hist2 = hist1;
            hist1 = sample;
        }

        done += frame_samples;
        /* Full frame: 1 header + 7 nibble bytes = 14 samples. Partial
         * (final) frame: 1 header + ceil(n/2) nibble bytes. The (n+1)/2
         * advance covers both; the (len - pos) clamp above guarantees we
         * never read past the buffer. */
        pos += (frame_samples + 1) / 2;
    }

    /* Zero-fill if the data was truncated (malformed file). Keeping the
     * output length equal to `samples` in every case lets callers compute
     * buffer sizes from the header alone. */
    for (; done < samples; done++)
        out[done] = 0;

    state->hist1 = (int16_t)hist1;
    state->hist2 = (int16_t)hist2;
    return done;
}
