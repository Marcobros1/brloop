/* Streaming PCM16 WAV writer (RIFF, little-endian). */
#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FILE    *file;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t frames_written;
} WavWriter;

/* Open path for writing and emit the 44-byte canonical WAV header.
 * Returns 0 on success, -1 on failure. */
int wav_open(WavWriter *w, const char *path, uint16_t channels,
             uint32_t sample_rate);

/* Append interleaved PCM16 frames. Returns 0 on success, -1 on failure. */
int wav_write(WavWriter *w, const int16_t *interleaved, uint32_t frames);

/* Patch sizes into the header and close. Returns 0 on success, -1 on
 * failure. */
int wav_close(WavWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* WAV_WRITER_H */
