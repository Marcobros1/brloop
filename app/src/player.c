/* Playback engine: BRSTM decode cursor, loop wrap, speed and waveOut glue.
 *
 * Threading model: one CRITICAL_SECTION protects every piece of player
 * state, including the waveOut handle. The engine thread and the UI thread
 * only wait on events while unlocked, so there is no deadlock path; every
 * UI mutation signals cmd_event, so no wakeup is ever lost (auto-reset
 * events stay signaled until a wait consumes them).
 *
 * Loop model: the loop section is [loop_start, total_samples). On the first
 * pass the engine captures the next_decoder_state DSP state at the start of the block
 * containing loop_start (HEAD part 3 loop_hist does NOT match that state,
 * see MEMORY.md), so wraps restore the exact decoder history and the seam
 * is sample-exact without trusting the ADPC table.
 *
 * Speed model: the device plays the original PCM at
 * device_rate = sample_rate * speed_ppm / 1000000 (truncated), the same
 * effect FLSM produces by rewriting the file's rate field, done in real time
 * and without touching the file. No resampler and no pitch preservation:
 * pitch rises with speed, like the reference. On a speed change the device
 * is reopened and playback resumes from the first buffer not yet heard, so
 * the position and the loop bounds are preserved.
 */
#include "player.h"

#include "audio_out.h"
#include "resampler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PLAYER_BLOCK_NONE 0xFFFFFFFFu
#define PLAYER_SLOT_FRAMES 512
#define PLAYER_MIN_PPM 250000u   /* 0.25x */
#define PLAYER_MAX_PPM 4000000u  /* 4.0x */

struct Player {
    CRITICAL_SECTION cs;
    int headless;

    /* File + decode buffers. */
    Brstm brstm;
    int file_open;
    uint8_t *adpcm_block;    /* one ADPCM block, one channel */
    int16_t *block_pcm;    /* one decoded block, interleaved (render source) */
    int16_t *channel_block;  /* one channel block */
    int16_t *slotbuf; /* PLAYER_SLOT_FRAMES * BRSTM_MAX_CHANNELS */

    /* Decode cursor. */
    uint32_t cursor;                       /* next source sample to produce */
    Resampler resampler;                   /* fractional read, used when speed != 1.0x */
    DspAdpcmState next_decoder_state[BRSTM_MAX_CHANNELS];
    uint32_t next_block;                /* block `next_decoder_state` enters */
    DspAdpcmState block_start_state[BRSTM_MAX_CHANNELS]; /* enters decoded_block */
    uint32_t decoded_block;                    /* block decoded in block_pcm */
    DspAdpcmState previous_start_state[BRSTM_MAX_CHANNELS];
    uint32_t previous_block;
    int have_prev;

    /* Loop capture. */
    uint32_t loop_block;
    DspAdpcmState loop_entering[BRSTM_MAX_CHANNELS];
    int have_loop_state;

    /* Runtime. */
    PlayerState state;
    int loop_enabled;
    uint32_t speed_ppm;
    int channel_pair;    /* -1 = mix all channels; else source stereo pair */
    uint64_t position;   /* source samples already heard */
    uint32_t wraps;
    uint32_t last_wrap_position;
    uint32_t audio_error;
    int decode_failed;
    int finish_pending;  /* end reached, waiting for the queue to drain */

    /* Audio device + queue bookkeeping (idle when headless). */
    AudioOut audio;
    HANDLE done_event;
    int slot_busy[AUDIO_OUT_SLOTS];
    uint32_t slot_src[AUDIO_OUT_SLOTS];
    uint32_t slot_frames[AUDIO_OUT_SLOTS];

    /* Engine thread. */
    HANDLE thread;
    HANDLE stop_event;
    HANDLE cmd_event;
    volatile LONG thread_exit;
};

/* Acquire/release the single player lock. Kept as functions (not macros)
 * so lock ordering is grep-able and future code cannot silently inline a
 * different lock. */
static void lock(Player *p) { EnterCriticalSection(&p->cs); }
static void unlock(Player *p) { LeaveCriticalSection(&p->cs); }

/* Nudge the engine to re-examine state after a UI mutation. The event is
 * auto-reset, so signaling without a waiter just leaves it signaled - the
 * wake is never lost (see header comment on the threading model). */
static void wake_engine(Player *p)
{
    if (p->cmd_event)
        SetEvent(p->cmd_event);
}

/* ppm -> device sample rate (truncated, same as FLSM's field rewrite).
 * Exposed so tests and UI can predict the device rate without duplicating
 * the formula; truncation (not rounding) matches the observed FLSM output
 * for 44100/48000 (see MEMORY.md speed decision). */
uint32_t player_device_rate(uint32_t sample_rate, uint32_t speed_ppm)
{
    return (uint32_t)(((uint64_t)sample_rate * speed_ppm)
                      / PLAYER_SPEED_SCALE);
}

/* device_rate with the ppm clamp applied: the speed actually in force,
 * derived from the stored value (UI can only set 1.0x/Final Lap today, but
 * set_speed accepts any value in range, so the clamp lives here too).
 * Caller holds the lock: reads speed_ppm and the file header. */
static uint32_t current_device_rate(const Player *p)
{
    uint32_t ppm = p->speed_ppm;
    if (ppm < PLAYER_MIN_PPM) ppm = PLAYER_MIN_PPM;
    if (ppm > PLAYER_MAX_PPM) ppm = PLAYER_MAX_PPM;
    return player_device_rate(p->brstm.info.sample_rate, ppm);
}

/* Render produced nothing: the loop section was reached with the loop off
 * (or decoding failed). Caller holds the lock. */
static int at_stream_end(const Player *p)
{
    return p->cursor >= p->brstm.info.total_samples
        && !(p->loop_enabled && p->brstm.info.loop);
}

/* Decode one whole block into block_pcm. `next_decoder_state` must enter `block` (caller
 * checks next_block == block); on success `next_decoder_state` exits the block. */
static int decode_block_locked(Player *p, uint32_t block)
{
    const BrstmInfo *info = &p->brstm.info;
    uint32_t frames = brstm_block_sample_count(&p->brstm, block);
    uint32_t channels = info->channels;
    DspAdpcmState entering[BRSTM_MAX_CHANNELS];

    memcpy(entering, p->next_decoder_state, sizeof entering);

    for (uint32_t c = 0; c < channels; c++) {
        size_t len = 0;
        BrstmStatus st = brstm_read_channel_block(&p->brstm, block, c,
                                                  p->adpcm_block, info->block_size,
                                                  &len);
        if (st != BRSTM_OK) {
            p->decode_failed = 1;
            p->state = PLAYER_STOPPED;
            return 0;
        }
        dsp_adpcm_decode(p->adpcm_block, len, info->coefs[c], &p->next_decoder_state[c],
                         p->channel_block, frames);
        for (uint32_t s = 0; s < frames; s++)
            p->block_pcm[(size_t)s * channels + c] = p->channel_block[s];
    }

    if (p->decoded_block != PLAYER_BLOCK_NONE) {
        p->previous_block = p->decoded_block;
        memcpy(p->previous_start_state, p->block_start_state, sizeof p->previous_start_state);
        p->have_prev = 1;
    } else {
        p->have_prev = 0;
    }
    memcpy(p->block_start_state, entering, sizeof p->block_start_state);
    p->decoded_block = block;
    p->next_block = block + 1;
    return 1;
}

/* Caller holds the lock. Drops the resampler history and fraction so the next
 * output is interpolated only from samples at the new position. */
static void reset_resampler_locked(Player *p)
{
    p->resampler.frac = 0;
    memset(p->resampler.history, 0, sizeof p->resampler.history);
}

/* Caller holds the lock. Rewinds the decode cursor to the file start. */
static void reset_decode_locked(Player *p)
{
    const BrstmInfo *info = &p->brstm.info;
    for (uint32_t c = 0; c < info->channels; c++) {
        p->next_decoder_state[c].hist1 = info->init_hist1[c];
        p->next_decoder_state[c].hist2 = info->init_hist2[c];
    }
    p->next_block = 0;
    p->decoded_block = PLAYER_BLOCK_NONE;
    p->previous_block = PLAYER_BLOCK_NONE;
    p->have_prev = 0;
    p->cursor = 0;
    reset_resampler_locked(p);
}

/* Move the cursor back to `sample` when its decoder state is still cached.
 * Only the current block and the previous block are retained; a farther seek
 * must use the exact-state reconstruction path below. */
static int rewind_to_locked(Player *p, uint32_t sample)
{
    uint32_t block = brstm_block_for_sample(&p->brstm, sample);
    if (block == p->decoded_block) {
        p->cursor = sample;
        return 1;
    }
    if (p->have_prev && block == p->previous_block) {
        memcpy(p->next_decoder_state, p->previous_start_state, sizeof p->next_decoder_state);
        p->next_block = block;
        p->decoded_block = PLAYER_BLOCK_NONE; /* force re-decode */
        p->have_prev = 0;
        p->cursor = sample;
        return 1;
    }
    return 0;
}

/* Recreate the exact decoder state at `sample` and leave the block that
 * contains it decoded in block_pcm. BRSTM blocks carry no embedded history, so
 * the walk restarts from the closest exact anchor: the captured loop-block
 * state (valid for samples inside or after the loop block) or the file's
 * initial history at block 0. On success next_decoder_state enters target_block+1.
 * Returns 0 if a block read/decode fails along the way. */
static int seek_decode_to_locked(Player *p, uint32_t sample)
{
    const BrstmInfo *info = &p->brstm.info;
    uint32_t target_block;
    DspAdpcmState anchor[BRSTM_MAX_CHANNELS];
    uint32_t anchor_block = 0;

    if (sample >= info->total_samples) {
        p->cursor = info->total_samples;
        p->decoded_block = PLAYER_BLOCK_NONE;
        p->have_prev = 0;
        return 1;
    }

    /* Fast path: the target sits in the current or the previous block. */
    if (rewind_to_locked(p, sample))
        return 1;

    target_block = brstm_block_for_sample(&p->brstm, sample);
    if (p->have_loop_state && target_block >= p->loop_block) {
        anchor_block = p->loop_block;
        memcpy(anchor, p->loop_entering, sizeof anchor);
    } else {
        for (uint32_t c = 0; c < info->channels; c++) {
            anchor[c].hist1 = info->init_hist1[c];
            anchor[c].hist2 = info->init_hist2[c];
        }
    }

    p->decoded_block = PLAYER_BLOCK_NONE;
    p->have_prev = 0;
    memcpy(p->next_decoder_state, anchor, sizeof p->next_decoder_state);
    p->next_block = anchor_block;

    for (uint32_t b = anchor_block; b <= target_block; b++) {
        if (b == p->loop_block && !p->have_loop_state) {
            /* Sequential traversal of the loop block: exact capture. */
            memcpy(p->loop_entering, p->next_decoder_state, sizeof p->loop_entering);
            p->have_loop_state = 1;
        }
        if (b != p->next_block || !decode_block_locked(p, b))
            return 0;
    }
    return 1;
}

/* Pull one source frame for the resampler: the same cursor, loop and block
 * rules as render_locked, one frame at a time. Returns 0 at a stop, which
 * ends the render exactly where render_locked would. Caller holds the lock. */
static int pull_source_frame_locked(void *ctx, int16_t *frame)
{
    Player *p = (Player *)ctx;
    const BrstmInfo *info = &p->brstm.info;
    uint32_t channels = info->channels;

    if (p->cursor >= info->total_samples) {
        if (p->loop_enabled && info->loop && p->have_loop_state) {
            p->wraps++;
            p->last_wrap_position = info->loop_start;
            p->cursor = info->loop_start;
            memcpy(p->next_decoder_state, p->loop_entering, sizeof p->next_decoder_state);
            p->next_block = p->loop_block;
            p->decoded_block = PLAYER_BLOCK_NONE;
            p->have_prev = 0;
        } else {
            return 0;
        }
    }

    uint32_t block = brstm_block_for_sample(&p->brstm, p->cursor);
    if (block != p->decoded_block) {
        if (block != p->next_block)
            return 0;
        if (block == p->loop_block && !p->have_loop_state) {
            memcpy(p->loop_entering, p->next_decoder_state, sizeof p->loop_entering);
            p->have_loop_state = 1;
        }
        if (!decode_block_locked(p, block))
            return 0;
    }

    uint32_t bstart = brstm_block_start_sample(&p->brstm, block);
    uint32_t off = p->cursor - bstart;
    const int16_t *src = p->block_pcm + (size_t)off * channels;
    int pair = p->channel_pair;
    if (pair >= 0) {
        uint32_t lc = (uint32_t)pair * 2;
        frame[0] = src[lc];
        frame[1] = src[lc + 1];
    } else {
        memcpy(frame, src, (size_t)channels * sizeof(int16_t));
    }
    p->cursor++;
    return 1;
}

/* Produce interleaved PCM16 into out. Caller holds the lock and the state
 * is PLAYING. */
static uint32_t render_locked(Player *p, int16_t *out, uint32_t frames)
{
    const BrstmInfo *info = &p->brstm.info;
    uint32_t channels = info->channels;
    uint32_t step = resampler_step(info->sample_rate, current_device_rate(p));

    if (step != RESAMPLER_ONE) {
        uint32_t out_channels = p->channel_pair >= 0 ? 2u : channels;
        if (p->resampler.channels != out_channels)
            resampler_reset(&p->resampler, out_channels);
        return (uint32_t)resampler_render(&p->resampler, step, pull_source_frame_locked,
                                          p, out, frames);
    }
    /* A selected pair renders source channels 2p/2p+1 as plain stereo; the
     * default emits every source channel (the fold happens at write time).
     * The pair is validated in player_set_channel_pair and reset on open. */
    int pair = p->channel_pair;
    uint32_t produced = 0;

    while (produced < frames) {
        if (p->cursor >= info->total_samples) {
            if (p->loop_enabled && info->loop && p->have_loop_state) {
                p->wraps++;
                p->last_wrap_position = info->loop_start;
                p->cursor = info->loop_start;
                memcpy(p->next_decoder_state, p->loop_entering, sizeof p->next_decoder_state);
                p->next_block = p->loop_block;
                p->decoded_block = PLAYER_BLOCK_NONE;
                p->have_prev = 0;
            } else {
                break;
            }
        }

        uint32_t block = brstm_block_for_sample(&p->brstm, p->cursor);
        if (block != p->decoded_block) {
            if (block != p->next_block)
                break; /* non-sequential jump we cannot serve: stop */
            /* Capture the state entering the loop block on the first pass. */
            if (block == p->loop_block && !p->have_loop_state) {
                memcpy(p->loop_entering, p->next_decoder_state, sizeof p->loop_entering);
                p->have_loop_state = 1;
            }
            if (!decode_block_locked(p, block))
                break;
        }

        uint32_t bstart = brstm_block_start_sample(&p->brstm, block);
        uint32_t bcount = brstm_block_sample_count(&p->brstm, block);
        uint32_t off = p->cursor - bstart;
        uint32_t take = frames - produced;
        if (take > bcount - off)
            take = bcount - off;

        if (pair >= 0) {
            const int16_t *src = p->block_pcm + (size_t)off * channels;
            int16_t *dst = out + (size_t)produced * 2;
            uint32_t lc = (uint32_t)pair * 2;
            for (uint32_t s = 0; s < take; s++) {
                dst[s * 2] = src[s * channels + lc];
                dst[s * 2 + 1] = src[s * channels + lc + 1];
            }
        } else {
            memcpy(out + (size_t)produced * channels,
                   p->block_pcm + (size_t)off * channels,
                   (size_t)take * channels * sizeof(int16_t));
        }
        p->cursor += take;
        produced += take;
    }
    return produced;
}

/* Publish the slot as done and advance the audible position past it.
 * Positions come from queued slots (what has been heard), never from the
 * cursor (what has been produced) - that difference is the whole point of
 * having a separate position. Caller holds the lock. */
static void harvest_slots_locked(Player *p)
{
    for (uint32_t i = 0; i < AUDIO_OUT_SLOTS; i++) {
        if (!p->slot_busy[i])
            continue;
        if (p->audio.headers[i].dwFlags & WHDR_DONE) {
            p->position = (uint64_t)p->slot_src[i] + p->slot_frames[i];
            p->slot_busy[i] = 0;
        }
    }
}

/* True while any queued slot is still audible (used by finish_check and
 * by tests to know the queue drained). */
/* Caller holds the lock: reads slot_busy[]. */
static int any_slot_busy(const Player *p)
{
    for (uint32_t i = 0; i < AUDIO_OUT_SLOTS; i++)
        if (p->slot_busy[i])
            return 1;
    return 0;
}

/* Queue one 512-frame slot per free slot so the device never starves.
 * `src` is captured *before* rendering because render_locked advances the
 * cursor; slot_src then maps a finished slot back to its start position.
 * An empty render either means end-of-stream (finish_pending) or a
 * non-sequential jump render could not serve (silent stop, no error). */
static void fill_slots_locked(Player *p)
{
    if (p->state != PLAYER_PLAYING || !p->audio.open)
        return;

    for (uint32_t i = 0; i < AUDIO_OUT_SLOTS; i++) {
        if (p->slot_busy[i])
            continue;
        uint32_t src = p->cursor;
        uint32_t got = render_locked(p, p->slotbuf, PLAYER_SLOT_FRAMES);
        if (got == 0) {
            if (at_stream_end(p))
                p->finish_pending = 1;
            return;
        }
        p->finish_pending = 0; /* produced audio (possibly after a wrap) */
        MMRESULT r = audio_write(&p->audio, i, p->slotbuf, got);
        if (r != MMSYSERR_NOERROR) {
            p->audio_error = r;
            p->state = PLAYER_STOPPED;
            audio_reset(&p->audio);
            memset(p->slot_busy, 0, sizeof p->slot_busy);
            return;
        }
        p->slot_busy[i] = 1;
        p->slot_src[i] = src;
        p->slot_frames[i] = got;
    }
}

/* End reached and everything queued has been heard. */
/* Caller holds the lock. Moves to FINISHED once the end is reached and the
 * queue has drained. */
static void finish_check_locked(Player *p)
{
    if (p->state == PLAYER_PLAYING && p->finish_pending
        && !any_slot_busy(p) && at_stream_end(p)) {
        p->position = p->brstm.info.total_samples;
        p->cursor = p->brstm.info.total_samples;
        p->state = PLAYER_FINISHED;
    }
}

/* Close and reopen the device at the current speed, preserving the position
 * of the oldest buffer the device has not played yet. */
/* Caller holds the lock. Closes and reopens waveOut for the current channel
 * count and rate, resuming from the oldest buffer the device has not played.
 * Used only when the device format changes; a stereo-to-stereo pair switch
 * does not come through here. */
static void reopen_device_locked(Player *p)
{
    uint32_t resume = p->cursor;
    int have_resume = 0;
    int was_paused = (p->state == PLAYER_PAUSED);

    if (p->audio.open) {
        harvest_slots_locked(p);
        for (uint32_t i = 0; i < AUDIO_OUT_SLOTS; i++) {
            if (p->slot_busy[i]
                && !(p->audio.headers[i].dwFlags & WHDR_DONE)) {
                resume = p->slot_src[i];
                have_resume = 1;
                break; /* slots are queued in order */
            }
        }
        audio_close(&p->audio);
    }
    memset(p->slot_busy, 0, sizeof p->slot_busy);
    p->finish_pending = 0;

    /* A selected pair outputs plain stereo: open the device as stereo so
     * every backend plays that pair only; the native/fold multichannel path
     * is kept for the mix-all mode. */
    uint16_t dev_ch = p->channel_pair >= 0
                    ? (uint16_t)2
                    : (uint16_t)p->brstm.info.channels;
    MMRESULT r = audio_open(&p->audio, dev_ch,
                            p->brstm.info.sample_rate, PLAYER_SLOT_FRAMES,
                            p->done_event);
    p->audio_error = r;
    if (r != MMSYSERR_NOERROR) {
        p->state = PLAYER_STOPPED;
        return;
    }

    if (have_resume && p->state != PLAYER_STOPPED) {
        (void)rewind_to_locked(p, resume); /* <=1-block queue: always fits */
        p->position = resume;
    }
    if (was_paused)
        audio_pause(&p->audio, 1);
}

/* Device teardown without reopening: drops queued audio (position bookkeeping
 * in slot_busy/finish_pending resets with it). Caller holds the lock. */
static void close_device_locked(Player *p)
{
    if (p->audio.open)
        audio_close(&p->audio);
    memset(p->slot_busy, 0, sizeof p->slot_busy);
    p->finish_pending = 0;
}

/* Engine thread: sleep on stop/command/buffer-done, then pump the queue.
 * The wait set is rebuilt under the lock every iteration so a device that
 * opened or closed between iterations is never waited on stale; after the
 * wait, the lock is retaken before touching state (the window where a UI
 * command signals cmd_event is covered by the auto-reset event staying
 * signaled, per the header comment). Exit only via stop_event or
 * thread_exit - both set by player_destroy. */
static DWORD WINAPI engine_thread(void *arg)
{
    Player *p = (Player *)arg;

    for (;;) {
        HANDLE waits[3];
        DWORD n = 2;

        /* Build the wait set under the lock; a mutation after this point
         * signals cmd_event, whose auto-reset state is not lost. */
        lock(p);
        waits[0] = p->stop_event;
        waits[1] = p->cmd_event;
        if (p->audio.open && p->audio.handle) {
            waits[2] = p->done_event;
            n = 3;
        }
        unlock(p);

        DWORD w = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED)
            break;

        lock(p);
        if (InterlockedCompareExchange(&p->thread_exit, 0, 0) != 0) {
            unlock(p);
            break;
        }
        if (p->state == PLAYER_PLAYING) {
            harvest_slots_locked(p);
            fill_slots_locked(p);
            finish_check_locked(p);
        }
        unlock(p);
    }
    return 0;
}

/* Allocate the player, its lock and (unless headless) the engine thread.
 * Headless mode is the test path: same decode/loop/seek logic with no audio
 * device, driven directly through player_render. Every failure path runs
 * player_destroy so a half-built object never leaks. */
Player *player_create(int enable_audio)
{
    Player *p = (Player *)calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->headless = !enable_audio;
    p->state = PLAYER_STOPPED;
    p->speed_ppm = PLAYER_SPEED_SCALE;
    p->channel_pair = -1;
    p->decoded_block = PLAYER_BLOCK_NONE;
    p->previous_block = PLAYER_BLOCK_NONE;
    p->next_block = PLAYER_BLOCK_NONE;
    p->loop_block = PLAYER_BLOCK_NONE;
    InitializeCriticalSection(&p->cs);

    p->slotbuf = (int16_t *)malloc((size_t)PLAYER_SLOT_FRAMES
                                   * BRSTM_MAX_CHANNELS * sizeof(int16_t));
    if (!p->slotbuf) {
        player_destroy(p);
        return NULL;
    }
    if (p->headless)
        return p;

    p->done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    p->cmd_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    p->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!p->done_event || !p->cmd_event || !p->stop_event) {
        player_destroy(p);
        return NULL;
    }
    p->thread = CreateThread(NULL, 0, engine_thread, p, 0, NULL);
    if (!p->thread) {
        player_destroy(p);
        return NULL;
    }
    return p;
}

/* Stop the engine thread before freeing buffers or closing waveOut. The 5 s
 * wait is a last resort for a wedged winmm call; if it expires, deliberately
 * leak the player because the live thread may still access its buffers or
 * handle. The process caller can then exit without risking a use-after-free. */
void player_destroy(Player *p)
{
    if (!p)
        return;
    if (p->thread) {
        InterlockedExchange(&p->thread_exit, 1);
        SetEvent(p->stop_event);
        if (WaitForSingleObject(p->thread, 5000) != WAIT_OBJECT_0)
            return; /* Keep the live thread's owner allocated. */
        CloseHandle(p->thread);
        p->thread = NULL;
    }
    if (p->file_open) {
        lock(p);
        close_device_locked(p);
        brstm_close(&p->brstm);
        p->file_open = 0;
        unlock(p);
    } else if (p->audio.open) {
        lock(p);
        close_device_locked(p);
        unlock(p);
    }

    free(p->adpcm_block);
    free(p->block_pcm);
    free(p->channel_block);
    free(p->slotbuf);
    if (p->done_event) CloseHandle(p->done_event);
    if (p->cmd_event) CloseHandle(p->cmd_event);
    if (p->stop_event) CloseHandle(p->stop_event);
    DeleteCriticalSection(&p->cs);
    free(p);
}

/* Open a new file after closing the current file and device. Parse into a
 * temporary Brstm and allocate its buffers before swapping it into the
 * player, so a parse or allocation failure leaves no dangling Brstm. A failed
 * open therefore leaves the player closed, not on the previous track. */
int player_open(Player *p, const char *path, char *err, size_t errlen)
{
    lock(p);

    /* Stop whatever is playing before swapping the file. */
    if (p->state == PLAYER_PLAYING || p->state == PLAYER_PAUSED)
        audio_reset(&p->audio);
    close_device_locked(p);
    if (p->file_open) {
        brstm_close(&p->brstm);
        p->file_open = 0;
    }

    Brstm tmp;
    BrstmStatus st = brstm_open(path, &tmp, err, errlen);
    if (st != BRSTM_OK) {
        unlock(p);
        return (int)st;
    }

    const BrstmInfo *info = &tmp.info;
    size_t need_pcm = (size_t)info->samples_per_block * info->channels
                    * sizeof(int16_t);
    size_t need_ch = (size_t)info->samples_per_block * sizeof(int16_t);
    uint8_t *adpcm_block = (uint8_t *)malloc(info->block_size);
    int16_t *block_pcm = (int16_t *)malloc(need_pcm);
    int16_t *channel_block = (int16_t *)malloc(need_ch);
    if (!adpcm_block || !block_pcm || !channel_block) {
        free(adpcm_block); free(block_pcm); free(channel_block);
        brstm_close(&tmp);
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        unlock(p);
        return -1;
    }

    free(p->adpcm_block); free(p->block_pcm); free(p->channel_block);
    p->adpcm_block = adpcm_block; p->block_pcm = block_pcm; p->channel_block = channel_block;
    p->brstm = tmp;
    p->file_open = 1;

    reset_decode_locked(p);
    p->state = PLAYER_STOPPED;
    p->position = 0;
    p->wraps = 0;
    p->last_wrap_position = 0;
    p->audio_error = 0;
    p->decode_failed = 0;
    p->finish_pending = 0;
    p->channel_pair = -1; /* every file starts mixing all channels */
    p->loop_enabled = info->loop != 0;
    p->loop_block = brstm_block_for_sample(&p->brstm, info->loop_start);
    p->have_loop_state = 0;

    if (!p->headless)
        reopen_device_locked(p); /* device follows channels and rate */

    unlock(p);
    wake_engine(p);
    return 0;
}

/* Close file and device but keep the Player reusable (UI calls this when
 * the library changes selection); pair resets to -1 because it is a
 * per-file property. */
void player_close(Player *p)
{
    lock(p);
    close_device_locked(p);
    if (p->file_open) {
        brstm_close(&p->brstm);
        p->file_open = 0;
    }
    p->state = PLAYER_STOPPED;
    p->position = 0;
    p->channel_pair = -1;
    unlock(p);
    wake_engine(p);
}

/* Start or resume. From PAUSED it just unpauses the device (position kept);
 * from STOPPED/FINISHED it reopens the device and starts at the current
 * position, which is 0 after a plain stop but a seek target if the user
 * seeked while stopped - that behavior is covered by the test battery. */
int player_play(Player *p)
{
    int rc = -1;
    lock(p);
    if (!p->file_open) {
        unlock(p);
        return -1;
    }
    if (p->state == PLAYER_PAUSED) {
        if (!p->headless && p->audio.open)
            audio_pause(&p->audio, 0);
        p->state = PLAYER_PLAYING;
        rc = 0;
    } else if (p->state == PLAYER_STOPPED || p->state == PLAYER_FINISHED) {
        /* At the end (or never opened playback): start from the beginning.
         * A player_seek() while stopped left a position < total, and the
         * playback resumes from there. */
        if (p->position >= p->brstm.info.total_samples) {
            reset_decode_locked(p);
            p->position = 0;
        }
        p->wraps = 0;
        p->finish_pending = 0;
        if (!p->headless) {
            reopen_device_locked(p);
            if (!p->audio.open) {
                unlock(p);
                return -1;
            }
        }
        p->state = PLAYER_PLAYING;
        rc = 0;
    }
    unlock(p);
    wake_engine(p);
    return rc;
}

/* Pause: hold the device and the state; decode cursor and queue stay as
 * they are, so resume continues sample-exact (unlike stop, which resets). */
int player_pause(Player *p)
{
    lock(p);
    if (p->state == PLAYER_PLAYING) {
        if (!p->headless && p->audio.open)
            audio_pause(&p->audio, 1);
        p->state = PLAYER_PAUSED;
    }
    unlock(p);
    wake_engine(p);
    return 0;
}

/* Stop: drop queued audio, rewind the decoder to sample 0 and zero the
 * position (distinction from pause is the contract the UI relies on).
 * decode_failed clears too so a retry after stop is allowed to play. */
int player_stop(Player *p)
{
    lock(p);
    if (!p->headless)
        close_device_locked(p);
    if (p->file_open)
        reset_decode_locked(p);
    p->state = PLAYER_STOPPED;
    p->position = 0;
    p->decode_failed = 0;
    unlock(p);
    wake_engine(p);
    return 0;
}

/* Seek: rebuild the decoder state at the target (seek_decode_to_locked,
 * which can walk from the loop anchor or restart from block 0), then reopen
 * the device so queued audio from the old position is discarded. When not
 * playing, the cursor alone is kept as the future start position - reopening
 * is deferred to player_play. FINISHED degrades to STOPPED on success so a
 * subsequent play starts from the seek target. */
int player_seek(Player *p, uint64_t frame)
{
    int rc = -1;
    lock(p);
    if (!p->file_open) {
        unlock(p);
        return -1;
    }

    if (frame > p->brstm.info.total_samples)
        frame = p->brstm.info.total_samples;

    int keep_playing = (p->state == PLAYER_PLAYING
                        || p->state == PLAYER_PAUSED);

    /* Drop whatever is queued so the new position takes effect now. */
    if (!p->headless)
        close_device_locked(p);

    if (seek_decode_to_locked(p, (uint32_t)frame)) {
        p->cursor = (uint32_t)frame;
        p->position = frame;
        reset_resampler_locked(p);
        p->decode_failed = 0;
        p->finish_pending = 0;
        if (p->state == PLAYER_FINISHED)
            p->state = PLAYER_STOPPED;
        rc = 0;
    }

    if (rc == 0 && !p->headless && keep_playing) {
        reopen_device_locked(p); /* resumes paused if it was paused */
        if (!p->audio.open)
            p->state = PLAYER_STOPPED;
    }
    unlock(p);
    wake_engine(p);
    return rc;
}

/* Change speed (ppm of 1e6). Clamped to [0.25x, 4x]. The device keeps the
 * file's sample rate, so no reopen is needed: the resampler step follows the
 * new speed from the next block and playback continues without a gap. */
void player_set_speed(Player *p, uint32_t speed_ppm)
{
    lock(p);
    if (speed_ppm < PLAYER_MIN_PPM) speed_ppm = PLAYER_MIN_PPM;
    if (speed_ppm > PLAYER_MAX_PPM) speed_ppm = PLAYER_MAX_PPM;
    if (speed_ppm != p->speed_ppm) {
        /* The resampler history is kept: it holds the frames just before the
         * cursor, which do not depend on the step, so the next output is
         * interpolated from the same audio at the new speed. */
        p->speed_ppm = speed_ppm;
    }
    unlock(p);
    wake_engine(p);
}

/* Loop on/off at runtime. No device reopen: the loop check happens in
 * render_locked per sample batch, so toggling takes effect on the next
 * fill - including while paused (applied when playback resumes). */
void player_set_loop(Player *p, int enabled)
{
    lock(p);
    p->loop_enabled = enabled != 0;
    unlock(p);
    wake_engine(p);
}

/* Select the source stereo pair to play: -1 = mix every channel (the
 * default and the only legal value for <4-channel files), otherwise pair p
 * plays source channels 2p/2p+1 as plain stereo. Out-of-range selections
 * are rejected instead of clamped. Switching between two stereo pairs keeps
 * the device and queued audio alive; the next rendered buffer uses the new
 * pair. A change that alters the device format still uses the reopen path. */
int player_set_channel_pair(Player *p, int pair)
{
    lock(p);
    if (!p->file_open) {
        unlock(p);
        return -1;
    }
    if (pair != -1) {
        uint32_t channels = p->brstm.info.channels;
        if (pair < 0 || channels < 4
            || (uint32_t)(pair * 2 + 1) >= channels) {
            unlock(p);
            return -1;
        }
    }
    if (pair != p->channel_pair) {
        int old_pair = p->channel_pair;
        int stereo_to_stereo = old_pair >= 0 && pair >= 0;
        p->channel_pair = pair;
        /* A pair change only alters PCM selection, not the device format.
         * Keep queued audio for stereo-to-stereo switches so playback does
         * not pause. Reopen only when the output channel count changes. */
        if (!stereo_to_stereo && !p->headless
            && (p->state == PLAYER_PLAYING || p->state == PLAYER_PAUSED))
            reopen_device_locked(p);
    }
    unlock(p);
    wake_engine(p);
    return 0;
}

/* Atomic snapshot for the UI/tests: one lock acquisition, then plain copy.
 * Position rule: while audio is active, use the harvested slot position
 * (what has been *heard*); otherwise the decode cursor (headless or idle) -
 * the two differ by up to the queued audio, and the UI progress bar must
 * show what the listener heard, not what was decoded ahead. Slots are
 * harvested first so the answer is current. pair_count reports 0 for
 * <4-channel files, which the UI uses to disable the pair button. */
PlayerSnapshot player_get_state(Player *p)
{
    PlayerSnapshot s;
    memset(&s, 0, sizeof s);
    lock(p);
    s.state = p->state;
    s.speed_ppm = p->speed_ppm;
    s.wraps = p->wraps;
    s.last_wrap_position = p->last_wrap_position;
    s.audio_error = p->audio_error;
    s.audio_active = p->audio.open;
    s.audio_folded = p->audio.folded;
    s.loop_enabled = p->loop_enabled;
    s.pair_sel = p->channel_pair;
    s.decode_failed = p->decode_failed;
    if (p->file_open) {
        const BrstmInfo *info = &p->brstm.info;
        s.total_samples = info->total_samples;
        s.sample_rate = info->sample_rate;
        s.channels = info->channels;
        s.pair_count = info->channels >= 4 ? info->channels / 2 : 0;
        if (s.pair_sel >= (int32_t)s.pair_count)
            s.pair_sel = -1; /* defensive: never report a missing pair */
        s.file_loop = info->loop != 0;
        s.loop_start = info->loop_start;
        s.device_rate = current_device_rate(p);
        if (!p->headless)
            harvest_slots_locked(p);
        if (p->headless || (p->state != PLAYER_PLAYING
                            && p->state != PLAYER_PAUSED))
            s.position = p->cursor;
        else
            s.position = p->position;
        if (s.position > info->total_samples)
            s.position = info->total_samples;
    }
    unlock(p);
    return s;
}

/* Direct render path used by tests (headless): same render_locked as the
 * engine thread uses with audio, plus the same end-of-stream detection - a
 * zero-length render at total_samples with loop off transitions to FINISHED
 * exactly like the queued path does via finish_check_locked. */
uint32_t player_render(Player *p, int16_t *out, uint32_t frames)
{
    uint32_t got = 0;
    lock(p);
    if (p->file_open && p->state == PLAYER_PLAYING) {
        got = render_locked(p, out, frames);
        if (got > 0)
            p->finish_pending = 0;
        if (got == 0 && p->state == PLAYER_PLAYING && at_stream_end(p)) {
            p->position = p->brstm.info.total_samples;
            p->state = PLAYER_FINISHED;
        }
    }
    unlock(p);
    return got;
}
