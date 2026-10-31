# T1 verification — BRLoop

Verifies the BRSTM reader and DSP-ADPCM decoder (`core/`) against vgmstream
and against controlled invalid-file cases.

## Prerequisites

- Visual Studio 2022 with C++ x64 tools (located via vswhere).
- Python 3.
- vgmstream CLI: por defecto `%USERPROFILE%\Desktop\LoopingAudioConverter-3.2\tools\vgmstream\test.exe`.
  Para usar otra copia, define `MKW_VGMSTREAM` con la ruta completa.
- Collection: `%APPDATA%\CT-MKWII\Mods\Metal Kart Wii Retro Rewind` (read-only).

## Run everything

    powershell -ExecutionPolicy Bypass -File tests\run_verification.ps1

Builds `core/`, then runs 180 checks. Add `-SkipBuild` to reuse the compiled
tools. Exit code 0 = all checks passed. Temporary WAVs go to
`%LOCALAPPDATA%\Temp\opencode\t1_verify` and are deleted per case.

## What is checked

1. Metadata of 7 files (44100/48000/51269 stereo, 4ch, 8ch, one no-loop file)
   from `brstm_info` vs `vgmstream -m` and vs expected values, including loop
   start/end/total.
2. Full decode vs `vgmstream -i`: PCM data byte-for-byte identical.
3. `--state adpc` (each block starts from the ADPC table) vs the carried
   decode: bit-identical where the table is exact (`uTTC_N`, `tKD_N`,
   `104_N`), small bounded transient where the encoder wrote slightly off
   states (`n_rainbow32_f`, `o_Wi-Fi_waiting32`), clean exit 4 where the
   table is zeroed (`o_FanfareGP1_32`, `STRM_N_KOOPA_N`).
4. Peak decode memory below half the full PCM size: the song is never
   pre-decoded in full.
5. 38 mutated invalid files (magic, chunk offsets, codecs, loop metadata,
   block layout, truncation...): `brstm_info` and `brstm_decode` must exit
   with the exact expected code and error keyword. A crash surfaces as an
   unexpected exit code.

## Individual tools

    core\build\brstm_info.exe  <file.brstm>
    core\build\brstm_decode.exe <in.brstm> <out.wav> [--state carry|adpc]

Exit codes: 0 ok, 1 usage, 2 invalid BRSTM, 3 I/O, 4 ADPC unusable.

## Supporting scripts

- `make_invalid_cases.py <base.brstm> <outdir>` — writes the mutants plus
  `manifest.json` (expected exit code and error keyword per case).
- `compare_pcm.py <a.wav> <b.wav> [--max-bad-fraction F]` — byte-for-byte WAV
  data comparison; optional tolerance for the ADPC-approximate files.
