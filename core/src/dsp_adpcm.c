#include "dsp_adpcm.h"

static int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

uint32_t dsp_adpcm_decode(const uint8_t *data, size_t len,
                          const int16_t coefs[DSP_ADPCM_PREDICTORS][2],
                          DspAdpcmState *state,
                          int16_t *out, uint32_t samples)
{
    uint32_t done = 0;
    size_t pos = 0;
    int32_t hist1 = state->hist1;
    int32_t hist2 = state->hist2;

    while (done < samples) {
        if (pos >= len)
            break;

        uint8_t ps = data[pos++];
        int predictor = ps >> 4;
        int scale = ps & 0x0F;
        if (predictor >= DSP_ADPCM_PREDICTORS)
            predictor = DSP_ADPCM_PREDICTORS - 1; /* nibble could ask 8-15 */
        int32_t coef1 = coefs[predictor][0];
        int32_t coef2 = coefs[predictor][1];
        int32_t distance_base = (int32_t)1 << (scale + 11); /* (1<<scale)*2048 */

        uint32_t frame_samples = samples - done;
        if (frame_samples > DSP_ADPCM_SAMPLES_PER_FRAME)
            frame_samples = DSP_ADPCM_SAMPLES_PER_FRAME;
        /* Each remaining byte carries two samples (nibbles). */
        if (frame_samples > (uint32_t)((len - pos) * 2))
            frame_samples = (uint32_t)((len - pos) * 2);
        if (frame_samples == 0)
            break;

        for (uint32_t i = 0; i < frame_samples; i++) {
            int nibble = (i & 1) ? (data[pos + i / 2] & 0x0F)
                                 : (data[pos + i / 2] >> 4);
            if (nibble >= 8)
                nibble -= 16;

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

    /* Zero-fill if the data was truncated (malformed file). */
    for (; done < samples; done++)
        out[done] = 0;

    state->hist1 = (int16_t)hist1;
    state->hist2 = (int16_t)hist2;
    return done;
}
