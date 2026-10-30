#include "transport.h"

/* Playback is paused, stopped, finished or playing: the button decides
 * from the state it sees, so a stale snapshot never picks the wrong action. */
TransportPlayAction transport_play_action(PlayerState state)
{
    switch (state) {
    case PLAYER_PAUSED:
        return TRANSPORT_PLAY_RESUME;
    case PLAYER_FINISHED:
        return TRANSPORT_PLAY_RESTART;
    case PLAYER_PLAYING:
        return TRANSPORT_PLAY_PAUSE;
    default:
        return TRANSPORT_PLAY_START;
    }
}

/* Three seconds is the usual media-player threshold: earlier than that a
 * "previous" press means "last track", later it means "start this one again". */
TransportPreviousAction transport_previous_action(uint64_t position,
                                                  uint32_t sample_rate)
{
    if (sample_rate == 0)
        return TRANSPORT_PREVIOUS_RESTART_SONG;
    if (position <= 3u * (uint64_t)sample_rate)
        return TRANSPORT_PREVIOUS_TRACK;
    return TRANSPORT_PREVIOUS_RESTART_SONG;
}

int transport_previous_index(int selected, int count)
{
    if (selected > 0)
        return selected - 1;
    return count > 0 ? count - 1 : -1;
}

/* Engine snapshot in, logical state out. The version index is 0-based and
 * stays -1 for files without pairs or in the internal mix-all mode, because
 * the UI never offers that mode. */
TransportLogicalState transport_logical_state(PlayerState state, int speed_is_flsm,
                                              int32_t pair_sel, uint32_t pair_count)
{
    TransportLogicalState s;
    s.playing = state == PLAYER_PLAYING;
    s.paused = state == PLAYER_PAUSED;
    s.finished = state == PLAYER_FINISHED;
    s.speed_flsm = speed_is_flsm != 0;
    s.pair_index = (pair_count > 0 && pair_sel >= 0) ? (int)pair_sel : -1;
    return s;
}

int transport_next_index(int selected, int count)
{
    if (selected < 0 || count <= 0)
        return -1;
    return (selected + 1) % count;
}

int transport_next_pair(int32_t pair_sel, uint32_t pair_count)
{
    if (pair_count == 0)
        return -1;
    if (pair_sel < 0 || pair_sel + 1 >= (int32_t)pair_count)
        return 0;
    return pair_sel + 1;
}
