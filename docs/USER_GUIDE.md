# BRLoop user guide

BRLoop plays BRSTM files, the audio format of Nintendo Wii and GameCube games.
A BRSTM can store a loop point, so a song repeats one section without a gap.
BRLoop uses that loop point as stored in the file. It never converts or saves
a new file.

## What you need

- Windows 10 or 11.
- Your own `.brstm` files. BRLoop does not include any music.

## Starting the program

Run `brloop.exe`. The window opens empty.

You can also pass files on the command line:

```text
brloop.exe path\to\song.brstm
```

## Adding files

- Click **Open…** to pick one or more `.brstm` files.
- Files appear in the **Library** list. Each row shows the name, sample rate,
  channel count, duration and whether the file has a loop.
- Click **Remove** to take the selected file out of the list. The file itself
  stays on your disk.
- Click **Clear** to empty the list.

A file that is already in the list is not added twice.

If a file cannot be read, BRLoop shows a message with the reason. Files that
are not valid BRSTM data are rejected and the rest of the list keeps working.

## Playing a song

Select a row in the Library. The **File information** panel shows its details:
name, sample rate, channels, duration, samples, loop start and loop end, path,
and the stereo version.

- **Play** starts the selected song. While it plays, the same button becomes
  **Pause**. After the song ends it becomes **Play again**.
- **Resume** continues a paused song from where it stopped.
- **Previous** and **Next** move through the list.
  - **Previous** restarts the song if you are more than 3 seconds into it.
    Press it again to go to the previous song.
  - If a song is playing when you change songs, the new one starts playing too.
- The progress bar shows the position in the song. Click it to jump to that
  point, or drag to a new point and release.
- The status line shows the playback state (Stopped, Playing, Paused or
  Finished) and the current time.

## Loop

The **Repeat loop** checkbox controls what happens at the end of the song:

- **Checked:** the section between the loop start and loop end repeats
  indefinitely. This is how the game plays the song.
- **Unchecked:** the song plays to its end and stops.

Files without a loop point show **No** in the Loop field, **Not available** for
the loop start and end, and the **Repeat loop** checkbox is disabled. They play
to the end.

## Speed

The **Speed** button switches between two modes:

- **1.0×:** the original speed.
- **Final Lap ×1,06812:** plays about 6.8% faster.

The speed changes during playback. The file is not modified.

Speed changes the pitch as well as the tempo, so a faster song sounds higher.

## Stereo versions

Some files, such as those used in Mario Kart Wii, contain several stereo
versions of the same song. The **Version** field shows the current one and its
channels.

Use the version button to switch to the next stereo pair. If the file has only one
pair, the button has nothing to switch to.

## Language

The language button (globe icon) opens the language menu. Choose
**English** or **Español**. The change applies immediately and is saved.

The language is stored in:

```text
%APPDATA%\BRLoop\settings.json
```

If that file is damaged, BRLoop shows a notice and uses English. It does not
overwrite the file unless you change the language again.

## Troubleshooting

| Message | What it means | What to do |
|---|---|---|
| Invalid or unreadable file | The file is not a valid BRSTM, or it is damaged or cut short. | Check that the file is complete and was not renamed from another format. |
| Could not open «…»: … | The file could not be read or the audio device rejected it. | Check the file, then try again. |
| Decoding error; playback stopped | The file contains data BRLoop cannot decode. | The file may be damaged. |
| Audio output error | Windows could not use the audio device. | Check the audio output in Windows, then try again. |
| The interface font was not found | `assets\fonts\PixelifySans.ttf` is missing next to the program. | Put the `assets` folder back next to `brloop.exe`. The program still works, with the system font. |
| The settings file is invalid | `settings.json` could not be read. | Delete the file or change the language again to rewrite it. |

If none of these explains the problem, the window's status line shows the
latest message. Include it when you report an issue.

## Where files are kept

- BRLoop stores only the language setting, in `%APPDATA%\BRLoop\settings.json`.
- BRLoop never writes to your `.brstm` files.
- BRLoop does not send any data over the network.
