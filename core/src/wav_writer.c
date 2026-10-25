#include "wav_writer.h"

#include <string.h>

/* Little-endian scalar writers: WAV is a little-endian format regardless of
 * host byte order, so every multi-byte field goes through these. */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/* Write the canonical 44-byte RIFF header at offset 0. Called twice with
 * the same layout: once at open with data_bytes = 0 (so a crashed run still
 * leaves a parseable file) and again at close with the real size - the
 * streaming path never seeks back per write, it only patches at the end. */
static int write_header(WavWriter *w, uint32_t data_bytes)
{
    uint8_t hdr[44];
    uint32_t byte_rate = w->sample_rate * w->channels * 2;
    uint16_t block_align = (uint16_t)(w->channels * 2);

    memcpy(hdr, "RIFF", 4);
    put_u32(hdr + 4, 36 + data_bytes);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    put_u32(hdr + 16, 16);            /* PCM fmt chunk size */
    put_u16(hdr + 20, 1);             /* PCM */
    put_u16(hdr + 22, w->channels);
    put_u32(hdr + 24, w->sample_rate);
    put_u32(hdr + 28, byte_rate);
    put_u16(hdr + 32, block_align);
    put_u16(hdr + 34, 16);            /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    put_u32(hdr + 40, data_bytes);

    if (fseek(w->file, 0, SEEK_SET) != 0)
        return -1;
    return fwrite(hdr, 1, sizeof hdr, w->file) == sizeof hdr ? 0 : -1;
}

int wav_open(WavWriter *w, const char *path, uint16_t channels,
             uint32_t sample_rate)
{
    memset(w, 0, sizeof *w);
    w->channels = channels;
    w->sample_rate = sample_rate;
    w->file = fopen(path, "wb");
    if (!w->file)
        return -1;
    return write_header(w, 0);
}

int wav_write(WavWriter *w, const int16_t *interleaved, uint32_t frames)
{
    size_t samples = (size_t)frames * w->channels;
    /* int16_t array is host-endian; WAV needs little-endian. Convert
     * explicitly so the tool behaves the same on any host. */
    static uint8_t buf[1 << 16];
    size_t off = 0;
    while (off < samples) {
        size_t chunk = samples - off;
        if (chunk > sizeof buf / 2)
            chunk = sizeof buf / 2;
        for (size_t i = 0; i < chunk; i++)
            put_u16(buf + 2 * i, (uint16_t)interleaved[off + i]);
        if (fwrite(buf, 1, chunk * 2, w->file) != chunk * 2)
            return -1;
        off += chunk;
    }
    w->frames_written += frames;
    return 0;
}

/* Count bytes in data_bytes as uint32_t: files here are a few minutes of
 * PCM16 at most (4 GB RIFF limit is never approached), so no RF64 needed. */
int wav_close(WavWriter *w)
{
    if (!w->file)
        return -1;
    uint32_t data_bytes = w->frames_written * w->channels * 2;
    if (write_header(w, data_bytes) != 0) {
        fclose(w->file);
        w->file = NULL;
        return -1;
    }
    if (fclose(w->file) != 0) {
        w->file = NULL;
        return -1;
    }
    w->file = NULL;
    return 0;
}
