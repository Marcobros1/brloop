# BRLoop architecture

This document describes the current Windows player before the Android and web
frontends exist. It is a map for contributors, not a replacement for the
format and playback contracts in the headers.

## Component boundaries

```text
app/src/main.c         Win32 window, transport commands, track validation and ListView
app/src/library.c      library rows: ownership of paths, names and capacity; selection repair
app/src/transport.c    transport rules as pure functions: play/pause, previous, next, version
app/src/resampler.c    4-point Catmull-Rom sample-rate converter; applies every speed other than 1.0x
app/src/cli_options.c  command-line parsing (no Win32, no Player)
app/src/ui_selftest.c  scripted window test (--selftest); borrows the window state
app/src/player.c       playback state, decode cursor, loop, seek and speed
app/src/audio_out.c    waveOut device, queued PCM slots and stereo fallback
core/src/brstm.c       BRSTM/RSTM validation, metadata and block reads
core/src/dsp_adpcm.c   DSP-ADPCM block decoding
```

Shared UI vocabulary lives in `app/include/ui_types.h` (control IDs, `TrackRow`,
`LibraryState`, `Controls`). `ui_selftest.h` and `cli_options.h` define the only
interfaces those modules expose.

`main.c` must use the public `player.h` API. It must not inspect BRSTM reader
internals or manipulate decoded samples. The UI library is grouped in one
`LibraryState`: it owns the row array, its capacity and the selected index;
`selected_index == -1` is the empty-selection state. `player.c` owns the decode cursor and
is the only component that combines BRSTM blocks, DSP state, loop jumps and
transport state. `audio_out.c` owns the WinMM device and its `WAVEHDR` buffers.

## Threads and ownership

There are two execution contexts in audio mode:

- The **UI thread** receives Win32 messages, changes the library and sends
  player commands through the public API.
- The **engine thread** renders PCM and calls every `audio_out` function.

`Player` owns the BRSTM file, decoder buffers, command events, the critical
section and the engine thread. `AudioOut` owns the waveOut handle, headers and
slot buffers. The UI receives a value copy through `PlayerSnapshot`; it never
keeps a pointer into the player's mutable state.

The engine thread is the sole caller of `audio_out.c`. A command wakes it
through the command event, and the stop event wakes it during shutdown. The
critical section protects the player's mutable state shared with the UI.

Changing between two selected stereo pairs only changes which decoded channels
are copied into the next PCM buffer. The waveOut handle and queued buffers stay
alive, so a live version switch does not pause playback. Reopening remains the
fallback when a change alters the device format.

## Track opening and closing

1. The UI stops and closes the current track.
2. `player_open` parses the replacement into a temporary `Brstm`.
3. It allocates the replacement buffers before swapping them into `Player`.
4. The device is opened for the file's rate and channel count. If WinMM
   rejects multichannel PCM, `AudioOut` retries stereo and folds source pairs
   while writing.
5. A parse, allocation or device error leaves the player closed and reports
   the reason to the UI.

`player_destroy` signals the engine thread and waits up to five seconds. If
that wait expires, it deliberately retains the player allocation because a
live thread may still access it. Freeing it would create a use-after-free.

## Decode, seek and loop

The decoder advances block by block and produces only small PCM portions. The
whole song is never pre-decoded. BRSTM blocks do not contain complete decoder
history, so a seek either uses the current/previous cached block or rebuilds
the exact state from the file start or the captured state entering the loop
block.

A loop jump restores the decoder state captured when entering the loop block.
The ADPC table is an optional exact anchor: some validated collection files
contain a zeroed table, so the engine can fall back to carried decoder state.
The loop is applied to source sample positions. The device always opens at the
file's sample rate; speed is applied by the resampler, which reads source frames
at a fractional step (`playback rate / sample rate`). Changing speed never
reopens the device.

## Player state ownership

Every field of `Player` is protected by `p->cs`. The table lists who may write
each group, so a change that breaks the rule is visible in review.

| Group | Fields | Written by | Read by |
|---|---|---|---|
| Lock | `cs` | `lock`/`unlock` only | all |
| File and buffers | `brstm`, `file_open`, `adpcm_block`, `block_pcm`, `channel_block`, `slotbuf` | `player_open`, `player_close`, `player_destroy` (lock held) | decode and render (lock held) |
| Decode cursor | `cursor`, `resampler`, `next_decoder_state`, `next_block`, `block_start_state`, `decoded_block`, `previous_start_state`, `previous_block`, `have_prev` | `decode_block_locked`, `reset_decode_locked`, `rewind_to_locked`, `seek_decode_to_locked`, `render_locked` | same, plus `player_get_state` for `cursor` |
| Loop capture | `loop_block`, `loop_entering`, `have_loop_state` | `player_open`, `render_locked`, `seek_decode_to_locked` | `render_locked` |
| Runtime | `state`, `position`, `wraps`, `last_wrap_position`, `loop_enabled`, `speed_ppm`, `channel_pair`, `audio_error`, `decode_failed`, `finish_pending` | the `player_*` transport functions, `engine_thread`, `finish_check_locked` | `player_get_state` |
| Device queue | `audio`, `done_event`, `slot_busy[]`, `slot_src[]`, `slot_frames[]` | `reopen_device_locked`, `close_device_locked`, `harvest_slots_locked`, `fill_slots_locked`, `engine_thread` | `any_slot_busy`, `player_get_state` |
| Engine thread | `headless`, `thread`, `stop_event`, `cmd_event`, `thread_exit` | `player_create`, `player_destroy` | `engine_thread`, `wake_engine`, every transport function (`headless` selects the device path) |

Functions named `*_locked` assume the caller holds `cs` and never take it
themselves. Their comments say so. The only callers outside the public API are
`engine_thread`, which takes the lock before every call, and the public
functions.

`player_get_state` is the only reader that touches several groups at once. It
takes the lock once and copies a snapshot, so the UI never sees a half-updated
state.

## Public state model

```text
STOPPED  no active playback; stop/reset returns the cursor to sample 0
PLAYING  engine renders and queues audio
PAUSED   device is held; cursor and queued state are retained
FINISHED end reached with looping disabled and queued audio drained
```

`PlayerSnapshot.position` is the audible source position harvested from played
slots, not merely the furthest decoded position. The UI uses this distinction
for progress and the `Anterior` rule: more than three seconds into the track
restarts it; near the beginning selects the previous track.

## Continuous integration

`.github/workflows/ci.yml` runs on `windows-latest` and checks only what needs
no local data: whitespace (`git diff --check`), a clean build of `core/` and
`app/` with `/W4 /WX`, and removal of build intermediates.

The full harness `tests/run_t2_verification.ps1` is not portable. It needs the
reference collection under `%APPDATA%\CT-MKWII`, the vgmstream binary and an
audio device. Several `player_tests` groups (`th`, `memory`, and the rest that
open collection files) cannot run without the collection. That harness belongs
on a self-hosted Windows runner with audio, with the collection path supplied
as an environment variable. It is not wired yet.

## Safe refactoring rules

- Preserve the public contracts in `player.h`, `audio_out.h` and `brstm.h`.
- Keep the engine thread as the only `AudioOut` caller.
- Do not expose `Player` internals to the UI.
- Keep ownership explicit: every allocation, handle and `WAVEHDR` has one
  owner and one cleanup path.
- After each structural change run `tests/run_t2_verification.ps1` and remove
  generated object files and temporary build tools.
