/* waveOut backend: PCM16 device, fixed buffer pool, completion event.
 *
 * All functions must be called from a single thread (the engine thread in
 * player.c). The device reports finished buffers by signaling done_event
 * (CALLBACK_EVENT), which the engine waits on together with its command
 * event.
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

/* windows.h must precede mmsystem.h (basic types like DWORD). */
#include <windows.h>
#include <mmsystem.h>

#include <stdint.h>

#define AUDIO_OUT_SLOTS 4

typedef struct {
    HWAVEOUT handle;
    WAVEHDR  headers[AUDIO_OUT_SLOTS];
    BYTE    *data[AUDIO_OUT_SLOTS];
    uint32_t slot_frames;
    uint32_t frame_bytes;   /* dev_channels * 2: bytes per device frame */
    uint16_t channels;      /* source channels declared by the file */
    uint16_t dev_channels;  /* channels opened on the device */
    int      folded;        /* 1 = stereo fallback, folded at write time */
    uint32_t rate;
    HANDLE   done_event;
    int      open;
} AudioOut;

/* Open the default device with `channels` PCM channels. Many devices reject
 * >2 channels (WAVERR_BADFORMAT); in that case the device is opened as
 * stereo and `folded` is set: audio_write folds the source channels into
 * two. Returns MMSYSERR_NOERROR or a waveOut error. */
MMRESULT audio_open(AudioOut *a, uint16_t channels, uint32_t rate,
                    uint32_t slot_frames, HANDLE done_event);

/* Reset (drop queued buffers) and close, releasing all headers. */
MMRESULT audio_close(AudioOut *a);

MMRESULT audio_pause(AudioOut *a, int pause);
MMRESULT audio_reset(AudioOut *a);

/* Queue `frames` (<= slot_frames) from pcm on slot. */
MMRESULT audio_write(AudioOut *a, uint32_t slot, const int16_t *pcm,
                     uint32_t frames);

/* 1 if the slot holds no buffer in flight and can be refilled. */
int audio_slot_free(const AudioOut *a, uint32_t slot);

/* Frames consumed by the device since the device was opened. */
MMRESULT audio_position(const AudioOut *a, uint32_t *frames);

#endif /* AUDIO_OUT_H */
