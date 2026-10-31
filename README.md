# BRLoop

A player for BRSTM audio files, the format used by Nintendo Wii/Gamecube games such as
Mario Kart Wii. It respects the loop points stored in each file. It plays the
original files directly, without converting them or generating new ones.

## Features

- Opens and plays `.brstm` files, repeating the section defined by their loop points.
- Changes playback speed while the file plays, without reopening the audio device.
- Shows file metadata (sample rate, channels, duration, loop points).
- Selects stereo channel pairs, as used by Mario Kart Wii files.
- English and Spanish interface, switchable at runtime.

## Status

Early version, Windows only. Playback and looping are checked by automated tests;
the interface is still under development.

## Roadmap

- Mobile-style layout: library and file information at the top, playback bar fixed at the bottom.
- Final color palette, following the Windows system theme.
- Motion control for the speed dial.
- Application icon.
- Android version.
- Web version, sharing the playback core where the platforms allow it.

## Requirements

- Windows 10 or 11.
- Visual Studio 2022 with the C++ tools (`cl.exe` compiler).
- Your own `.brstm` files. **This repository does not include any music.**

## Build

```powershell
powershell -ExecutionPolicy Bypass -File app\build.ps1
```

This builds `app\build\brloop.exe` and the test executable `player_tests.exe`. The
interface font is copied next to them automatically.

To also build the command-line tools in `core\` (`brstm_info`, `brstm_decode`,
`brstm_channels`), run:

```powershell
powershell -ExecutionPolicy Bypass -File core\build.ps1
```

The app does not need them; they are for development and the test suite.

## Run

```powershell
app\build\brloop.exe path\to\file.brstm
```

You can also open files from the window with the **Open…** button.

For a full description of the window, the controls and the troubleshooting
messages, see the [user guide](docs/USER_GUIDE.md).

## Tests

The full test suite needs a reference collection and the vgmstream tool; see
[`tests/README.md`](tests/README.md). Environment variables:

- `MKW_COLLECTION`: folder with your reference `.brstm` files.
- `MKW_VGMSTREAM`: path to vgmstream's `test.exe`.

The interface self-test runs with `brloop.exe --selftest <files>`.

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md): engine, interface and test design.
- [`DESIGN.md`](DESIGN.md): visual direction of the interface.

## License

MIT. See [`LICENSE`](LICENSE).

The interface font, Pixelify Sans, is distributed under the SIL Open Font License 1.1.
Its license is in [`app/assets/fonts/OFL.txt`](app/assets/fonts/OFL.txt).

Third-party material, its licenses and the sources used for reference are listed in
[`THIRD_PARTY.md`](THIRD_PARTY.md).

Any Wii/GameCube games and the BRSTM files of its tracks belong to their respective owners.
This project does not distribute them.
