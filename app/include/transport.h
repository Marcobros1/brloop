#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <stdint.h>

#include "player.h"

/* What the play/pause button does for a given engine state. */
typedef enum {
    TRANSPORT_PLAY_START,     /* from stopped or unopened: start playback */
    TRANSPORT_PLAY_RESUME,    /* from paused: continue from the same place */
    TRANSPORT_PLAY_RESTART,   /* from finished: seek to the start, then play */
    TRANSPORT_PLAY_PAUSE      /* from playing: pause */
} TransportPlayAction;

/* What the "previous" button does. */
typedef enum {
    TRANSPORT_PREVIOUS_RESTART_SONG, /* far into the song: seek to the start */
    TRANSPORT_PREVIOUS_TRACK         /* near the start: select the row before */
} TransportPreviousAction;

/* Play/pause decision from the engine state alone. */
TransportPlayAction transport_play_action(PlayerState state);

/* Previous-button decision. Within the first three seconds the previous
 * track is selected; beyond that the current song restarts. A sample rate of
 * zero (no file open) also restarts, because there is no position to judge. */
TransportPreviousAction transport_previous_action(uint64_t position,
                                                  uint32_t sample_rate);

/* Row index that the previous button selects. Any row after the first goes
 * one back; from the first row (or with nothing selected) it wraps to the
 * last row, or returns -1 when the library is empty. The caller still checks
 * that the result differs from the selection, so a one-row library never
 * "changes" track to itself. */
int transport_previous_index(int selected, int count);

/* Row index that the next button selects, or -1 when the library is empty
 * or nothing is selected. Wraps from the last row to the first one. */
int transport_next_index(int selected, int count);

/* Logical state of the transport controls, without any text. The UI turns it
 * into captions in the active language; the self-test compares it directly,
 * so a language change never changes what the test checks. */
typedef struct {
    int playing;      /* 1 while the engine is playing */
    int paused;       /* 1 while the engine is paused */
    int finished;     /* 1 when the song reached its end */
    int speed_flsm;   /* 1 when the Final Lap speed is in force */
    int pair_index;   /* 0-based version shown, or -1 when there is none */
} TransportLogicalState;

TransportLogicalState transport_logical_state(PlayerState state, int speed_is_flsm,
                                              int32_t pair_sel, uint32_t pair_count);

/* Version (channel pair) that the version button selects next, or -1 when
 * the file has no pairs. Cycles through the real versions only: the engine's
 * mix-all value is never offered. */
int transport_next_pair(int32_t pair_sel, uint32_t pair_count);

#endif /* TRANSPORT_H */
