/* waveOut backend: PCM16 device, fixed buffer pool, completion event.
 *
 * All functions must be called from a single thread (the engine thread in
 * player.c). The device reports finished buffers by signaling done_event
 * (CALLBACK_EVENT), which the engine waits on together with its command
 * event.
 *
 * Slot lifecycle: audio_write prepares+queues a header, the device plays it
 * and sets WHDR_DONE (signaling done_event), player.c then reuses the slot.
 * audio_slot_free and the player's slot_busy[] bookkeeping express the same
 * fact from the two sides; the player keeps its own copy because it also
 * needs to know the slot's source position.
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

/* windows.h must precede mmsystem.h (basic types like DWORD). */
#include <windows.h>
#include <mmsystem.h>

#include <stdint.h>

/* 4 slots of 512 frames each: enough to cover winmm's queueing latency at
 * 44.1-51.3 kHz (~46 ms) while keeping each buffer small, so a speed or
 * pair change only discards at most one not-yet-heard slot. */
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

/* Drop every queued buffer (device keeps playing whatever it already has
 * until it processes the reset): used before reopening the device so no
 * audio from the old rate/position leaks into the new one. */
MMRESULT audio_reset(AudioOut *a);

/* Queue `frames` (<= slot_frames) from pcm on slot. */
MMRESULT audio_write(AudioOut *a, uint32_t slot, const int16_t *pcm,
                     uint32_t frames);

/* 1 if the slot holds no buffer in flight and can be refilled. */
int audio_slot_free(const AudioOut *a, uint32_t slot);

/* Frames consumed by the device since the device was opened. */
MMRESULT audio_position(const AudioOut *a, uint32_t *frames);

#endif /* AUDIO_OUT_H */
