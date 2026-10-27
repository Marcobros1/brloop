# Metal Kart Wii Music App architecture

This document describes the current Windows player before the Android and web
frontends exist. It is a map for contributors, not a replacement for the
format and playback contracts in the headers.

## Component boundaries

```text
app/src/main.c       Win32 window, library actions, transport and selftest
app/src/player.c     playback state, decode cursor, loop, seek and speed
app/src/audio_out.c  waveOut device, queued PCM slots and stereo fallback
core/src/brstm.c     BRSTM/RSTM validation, metadata and block reads
core/src/dsp_adpcm.c DSP-ADPCM block decoding
```

`main.c` must use the public `player.h` API. It must not inspect BRSTM reader
internals or manipulate decoded samples. `player.c` owns the decode cursor and
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
The loop is applied to source sample positions, independently of the device
rate used for Final Lap speed.

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

## Safe refactoring rules

- Preserve the public contracts in `player.h`, `audio_out.h` and `brstm.h`.
- Keep the engine thread as the only `AudioOut` caller.
- Do not expose `Player` internals to the UI.
- Keep ownership explicit: every allocation, handle and `WAVEHDR` has one
  owner and one cleanup path.
- After each structural change run `tests/run_t2_verification.ps1` and remove
  generated object files and temporary build tools.
