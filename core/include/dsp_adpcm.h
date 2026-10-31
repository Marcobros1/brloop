/* Nintendo DSP 4-bit ADPCM decoder.
 *
 * Frame layout (verified empirically against vgmstream output, see
 * research/dsp_residual.py and the formula derivation in
 * research/INFORME_TAREA02.md):
 *
 *   byte 0: predictor/scale: high nibble = predictor index (0-15),
 *           low nibble = scale shift.
 *   bytes 1..7: 14 samples as 4-bit two's complement nibbles, high nibble
 *           first (14 samples per 8-byte frame).
 *
 * Sample formula: the Nintendo DSP-ADPCM decoding equation as described in the
 * public BRSTM documentation (WiiBrew, https://wiibrew.org/wiki/BRSTM_file).
 * The same equation is used by VGAudio's GcAdpcmDecoder (MIT); this file is an
 * independent implementation, and VGAudio was used only as a cross-check.
 *
 *   distance  = nibble * ((1 << scale) * 2048)
 *   corrected = coef1 * hist1 + coef2 * hist2 + distance
 *   sample    = clamp16((corrected + 1024) >> 11)
 *
 * State (hist1 = n-1, hist2 = n-2) carries across frames and blocks; block
 * boundary states for random access come from the ADPC table.
 */
#ifndef DSP_ADPCM_H
#define DSP_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSP_ADPCM_SAMPLES_PER_FRAME 14
#define DSP_ADPCM_FRAME_SIZE 8

/* BRSTM channel info stores 8 predictor coefficient pairs; real files never
 * exceed predictor index 7 (verified on the collection), and the 4-bit
 * header nibble could ask for 0-15, so the decoder clamps to this range. */
#define DSP_ADPCM_PREDICTORS 8

typedef struct {
    int16_t hist1;
    int16_t hist2;
} DspAdpcmState;

/* Decode `samples` samples from `data` (one channel block, `len` bytes)
 * starting from *state, writing PCM16 to out (caller provides capacity for
 * `samples`). Updates *state. If the data ends early the remaining samples
 * are zero-filled (keeps output length predictable; only reachable with
 * malformed files, since validation enforces enough bytes for the block).
 * Returns the number of samples actually decoded from data. */
uint32_t dsp_adpcm_decode(const uint8_t *data, size_t len,
                          const int16_t coefs[DSP_ADPCM_PREDICTORS][2],
                          DspAdpcmState *state,
                          int16_t *out, uint32_t samples);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ADPCM_H */
