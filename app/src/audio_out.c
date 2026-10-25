#include "audio_out.h"

#include <stdlib.h>
#include <string.h>

/* Formats the mapper has already rejected. A FAILED waveOutOpen leaves about
 * three handles behind inside winmm on this machine (measured in isolation:
 * 10 failed 4-channel opens cost +30 handles), so the failing probe is paid
 * only the first time a format is seen; later opens go straight to the fold.
 * Accessed only under the player lock. */
#define AUDIO_REJECTED_MAX 8
static struct {
    uint32_t rate;
    uint16_t channels;
} g_rejected[AUDIO_REJECTED_MAX];
static int g_rejected_count;

/* Probe-or-skip: ask winmm only about (rate, channels) formats not already
 * known to fail. Returns 1 when the native open should be skipped. */
static int native_rejected(uint32_t rate, uint16_t channels)
{
    int i;
    for (i = 0; i < g_rejected_count; i++)
        if (g_rejected[i].rate == rate && g_rejected[i].channels == channels)
            return 1;
    return 0;
}

/* Record a format winmm rejected (bounds the cache silently past
 * AUDIO_REJECTED_MAX: worst case a few extra failed probes, never growth). */
static void remember_rejected(uint32_t rate, uint16_t channels)
{
    if (native_rejected(rate, channels))
        return;
    if (g_rejected_count < AUDIO_REJECTED_MAX) {
        g_rejected[g_rejected_count].rate = rate;
        g_rejected[g_rejected_count].channels = channels;
        g_rejected_count++;
    }
}

/* Open the device in three steps: native format if known-good or untested;
 * on a >2ch rejection, remember it and fall back to stereo + folded=1 (the
 * source channels are then mixed at write time); if even stereo fails,
 * return the error with open=0. Buffers are allocated last so any failure
 * before that leaves no allocations behind (and audio_close is idempotent
 * for the partial-open path). */
MMRESULT audio_open(AudioOut *a, uint16_t channels, uint32_t rate,
                    uint32_t slot_frames, HANDLE done_event)
{
    WAVEFORMATEX fmt;
    MMRESULT r;
    uint32_t bytes;
    uint32_t i;

    memset(a, 0, sizeof *a);
    a->channels = channels;
    a->rate = rate;
    a->slot_frames = slot_frames;
    a->done_event = done_event;

    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = channels;
    fmt.nSamplesPerSec = rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)((uint32_t)channels * sizeof(int16_t));
    fmt.nAvgBytesPerSec = rate * fmt.nBlockAlign;

    if (channels > 2 && native_rejected(rate, channels)) {
        r = WAVERR_BADFORMAT; /* known rejection: skip the failing probe */
    } else {
        r = waveOutOpen(&a->handle, WAVE_MAPPER, &fmt, (DWORD_PTR)done_event,
                        (DWORD_PTR)NULL, CALLBACK_EVENT);
        /* Measured on the test machine: the mapper rejects 4- and 8-channel
         * PCM (WAVERR_BADFORMAT = 32). Remember it and retry as stereo,
         * folding the source channels at write time so multichannel files
         * still play. */
        if (channels > 2 && r == (MMRESULT)WAVERR_BADFORMAT)
            remember_rejected(rate, channels);
    }
    if (r != MMSYSERR_NOERROR && channels > 2) {
        a->handle = NULL;
        a->folded = 1;
        a->dev_channels = 2;
        fmt.nChannels = 2;
        fmt.nBlockAlign = 2 * sizeof(int16_t);
        fmt.nAvgBytesPerSec = rate * fmt.nBlockAlign;
        r = waveOutOpen(&a->handle, WAVE_MAPPER, &fmt,
                        (DWORD_PTR)done_event, (DWORD_PTR)NULL,
                        CALLBACK_EVENT);
    }
    if (r != MMSYSERR_NOERROR) {
        a->handle = NULL;
        a->folded = 0;
        a->dev_channels = 0;
        return r;
    }
    if (!a->folded)
        a->dev_channels = channels;

    a->frame_bytes = (uint32_t)a->dev_channels * sizeof(int16_t);
    bytes = slot_frames * a->frame_bytes;
    for (i = 0; i < AUDIO_OUT_SLOTS; i++) {
        a->data[i] = (BYTE *)malloc(bytes);
        if (!a->data[i]) {
            audio_close(a);
            return MMSYSERR_NOMEM;
        }
        memset(&a->headers[i], 0, sizeof a->headers[i]);
        a->headers[i].lpData = (LPSTR)a->data[i];
        a->headers[i].dwBufferLength = bytes;
    }
    a->open = 1;
    return MMSYSERR_NOERROR;
}

/* Reset (drop queued buffers), unprepare every header still flagged
 * PREPARED (winmm requires unprepare before close), close the handle and
 * free the data. Safe on a never-opened or half-open AudioOut - player.c
 * calls it on every teardown path. */
MMRESULT audio_close(AudioOut *a)
{
    MMRESULT r = MMSYSERR_NOERROR;
    if (a->handle) {
        waveOutReset(a->handle);
        for (uint32_t i = 0; i < AUDIO_OUT_SLOTS; i++) {
            if (a->headers[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(a->handle, &a->headers[i],
                                       sizeof a->headers[i]);
        }
        r = waveOutClose(a->handle);
        a->handle = NULL;
    }
    for (uint32_t i = 0; i < AUDIO_OUT_SLOTS; i++) {
        free(a->data[i]);
        a->data[i] = NULL;
    }
    a->open = 0;
    return r;
}

/* Pause/resume the device without touching the queue: queued buffers stay
 * queued and resume from the same sample (this is why pause is cheap and
 * position-preserving in player.c). */
MMRESULT audio_pause(AudioOut *a, int pause)
{
    if (!a->handle)
        return MMSYSERR_NODRIVER;
    return pause ? waveOutPause(a->handle) : waveOutRestart(a->handle);
}

MMRESULT audio_reset(AudioOut *a)
{
    if (!a->handle)
        return MMSYSERR_NODRIVER;
    return waveOutReset(a->handle);
}

/* Fill one slot and hand it to the device: fold (when the device is
 * stereo-folded) or straight copy, then prepare-once + write. Returns the
 * winmm error directly; player.c treats any error as fatal for playback. */
MMRESULT audio_write(AudioOut *a, uint32_t slot, const int16_t *pcm,
                     uint32_t frames)
{
    if (!a->handle || slot >= AUDIO_OUT_SLOTS || frames > a->slot_frames)
        return MMSYSERR_INVALPARAM;

    WAVEHDR *h = &a->headers[slot];
    if (a->folded) {
        /* Fold N interleaved source channels to stereo: even-indexed
         * channels (FL, BL/SL, ...) to the left, odd-indexed (FR, BR/SR,
         * ...) to the right, each averaged to avoid clipping. Layout
         * assumption: standard L R L2 R2 ordering; documented in T2. */
        const uint32_t sc = a->channels;
        const uint32_t nl = (sc + 1) / 2;
        const uint32_t nr = sc / 2;
        int16_t *dst = (int16_t *)a->data[slot];
        uint32_t f, c;
        for (f = 0; f < frames; f++) {
            int32_t l = 0, r = 0;
            for (c = 0; c < sc; c++) {
                int32_t v = pcm[(size_t)f * sc + c];
                if ((c & 1) == 0)
                    l += v;
                else
                    r += v;
            }
            dst[(size_t)f * 2] = (int16_t)(l / (int32_t)nl);
            dst[(size_t)f * 2 + 1] = (int16_t)(r / (int32_t)nr);
        }
    } else {
        memcpy(a->data[slot], pcm, (size_t)frames * a->frame_bytes);
    }
    h->dwBufferLength = frames * a->frame_bytes;

    if (!(h->dwFlags & WHDR_PREPARED)) {
        MMRESULT r = waveOutPrepareHeader(a->handle, h, sizeof *h);
        if (r != MMSYSERR_NOERROR)
            return r;
    }
    return waveOutWrite(a->handle, h, sizeof *h);
}

int audio_slot_free(const AudioOut *a, uint32_t slot)
{
    if (!a->open || slot >= AUDIO_OUT_SLOTS)
        return 1;
    /* Never written since the last reset/reopen, or fully played. */
    return !(a->headers[slot].dwFlags & WHDR_INQUEUE)
        || (a->headers[slot].dwFlags & WHDR_DONE) != 0;
}

/* Device clock in samples since open. TIME_SAMPLES can be replaced by the
 * driver with TIME_BYTES if unsupported, so only that wType is trusted;
 * player.c currently uses slot bookkeeping instead and this remains the
 * diagnostic/fallback path. */
MMRESULT audio_position(const AudioOut *a, uint32_t *frames)
{
    MMTIME t;
    memset(&t, 0, sizeof t);
    t.wType = TIME_SAMPLES;
    if (!a->handle)
        return MMSYSERR_NODRIVER;
    MMRESULT r = waveOutGetPosition(a->handle, &t, sizeof t);
    if (r == MMSYSERR_NOERROR)
        *frames = t.u.sample;
    return r;
}
