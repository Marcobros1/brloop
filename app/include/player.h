/* BRSTM playback engine: file, decode, loop, speed and audio output.
 *
 * Layering (T2):
 *   main.c        UI, never touches samples or the BRSTM reader internals
 *   player.c      decode cursor, loop wrap, speed, playback state machine
 *   audio_out.c   waveOut device and buffer headers
 *   core/         BRSTM reader + DSP-ADPCM decoder (unchanged)
 *
 * Playback is always block-based: player_render() produces at most a small
 * block of PCM16 frames per call, so the whole song is never pre-decoded.
 *
 * Speed model (reproduces the measured FLSM effect, INFORME_TAREA01 C15):
 * the audio device plays the original PCM at
 *     device_rate = sample_rate * speed_ppm / 1000000   (truncated)
 * exactly what FLSM does by rewriting the file's sample-rate field, but in
 * real time and without touching the file. PLAYER_FLSM_SPEED_PPM yields
 * 44100 -> 47104 and 48000 -> 51269, the two conversions measured in T0 and
 * in the collection. k is an observed bound, not a proven universal
 * constant; other ppm values are plain speed ratios.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include "brstm.h"
#include "dsp_adpcm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_SPEED_SCALE 1000000u
#define PLAYER_FLSM_SPEED_PPM 1068120u /* observed FLSM factor 1.06812 */

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_FINISHED
} PlayerState;

typedef struct Player Player;

typedef struct {
    PlayerState state;
    uint64_t position;       /* source samples already produced for output */
    uint64_t total_samples;
    uint32_t sample_rate;    /* base rate declared by the file */
    uint32_t channels;
    uint32_t device_rate;    /* floor(sample_rate * speed_ppm / 1e6) */
    uint32_t speed_ppm;      /* PLAYER_SPEED_SCALE = 1.0x */
    int      loop_enabled;
    int      file_loop;      /* file carries valid loop metadata */
    uint32_t loop_start;
    uint32_t wraps;          /* completed loop wraps this session */
    uint32_t last_wrap_position; /* source sample of the most recent wrap */
    int      audio_active;   /* device is open */
    uint32_t audio_error;    /* waveOut error code, 0 = none */
    int      audio_folded;   /* device rejected >2ch: playing stereo fold */
    int      decode_failed;  /* block read/decode failed; playback stopped */
} PlayerSnapshot;

/* device_rate = sample_rate * speed_ppm / 1e6, truncated like FLSM. */
uint32_t player_device_rate(uint32_t sample_rate, uint32_t speed_ppm);

/* enable_audio: 1 runs the playback thread and the waveOut device; 0 creates
 * a headless player for tests, which drive player_render() directly. */
Player *player_create(int enable_audio);
void player_destroy(Player *p);

/* Stop playback and open a BRSTM file. Returns 0 on success, a BrstmStatus
 * code (>0) on an invalid/unreadable file, or -1 on allocation failure; err
 * receives a human-readable message. */
int player_open(Player *p, const char *path, char *err, size_t errlen);
void player_close(Player *p);

/* Transport. play() returns 0, -1 if no file is open; audio failures show up
 * asynchronously in PlayerSnapshot.audio_error. */
int player_play(Player *p);
int player_pause(Player *p);
int player_stop(Player *p);

void player_set_speed(Player *p, uint32_t speed_ppm);
void player_set_loop(Player *p, int enabled);

/* Jump to a source-sample position in [0, total_samples]. Works in any
 * state; while audio is playing the queued buffers are dropped so the new
 * position takes effect immediately and playback continues from there.
 * While stopped, the next player_play() resumes from the seeked position
 * (player_stop() still rewinds to 0). BRSTM blocks carry no embedded
 * decoder history, so the jump re-decodes from the closest exact anchor
 * (the captured loop-block state, or the file start): exact everywhere,
 * worst case a linear decode up to the target. Returns 0, or -1 if no file
 * is open or the re-decode fails (decode_failed is set). */
int player_seek(Player *p, uint64_t frame);

PlayerSnapshot player_get_state(Player *p);

/* Produce up to `frames` interleaved PCM16 frames, advancing the position
 * and applying the loop ([loop_start, total) while the loop is enabled).
 * Returns the frame count; 0 means the end was reached with the loop off.
 * Single consumer: the audio thread in audio mode, the caller when
 * headless. */
uint32_t player_render(Player *p, int16_t *out, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_H */
