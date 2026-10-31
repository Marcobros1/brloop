# Third-party material and references

This file lists every external work that BRLoop uses, cites, or was checked
against, with its license and where the information was obtained. Each entry
states how the work is used, so it is clear what ships in the release and what
is only used for development.

## Shipped with the release

### Pixelify Sans (interface font)

- Files: `app/assets/fonts/PixelifySans.ttf`, `app/assets/fonts/OFL.txt`
- Copyright 2021 The Pixelify Sans Project Authors (https://github.com/eifetx/Pixelify-Sans)
- License: SIL Open Font License 1.1 (text in `app/assets/fonts/OFL.txt`)
- Use: loaded privately at startup by `app/src/main.c`. The font is not modified
  and is redistributed with its license.

## Not shipped (development and verification only)

### vgmstream

- Project: https://github.com/vgmstream/vgmstream
- License: ISC license, with portions under other permissive notices listed in
  `COPYING`. Checked on 2026-10-31 from `COPYING` in the repository (copyright
  2008-2025 Adam Gashlin, Fastelbja, et al.).
- Use: reference decoder. The test suite compares BRLoop's PCM output byte for
  byte with `test.exe` from vgmstream (`tests/run_verification.ps1`,
  `tests/run_t2_verification.ps1`, `app/tests/player_tests.c`). No vgmstream
  code is copied into this repository.
- The binary is not distributed. It is located through `MKW_VGMSTREAM`.

### VGAudio

- Project: https://github.com/Thealexbarney/VGAudio
- License: MIT. Full text checked on 2026-10-31 from `LICENSE` in the repository.
- Use: cross-check of the DSP-ADPCM sample equation, in
  `research/dsp_final_checks.py` (`decode_vgaudio`), which is a Python
  re-implementation written for the comparison. No VGAudio code is copied into
  `core/`.

### WiiBrew: BRSTM file format

- Source: https://wiibrew.org/wiki/BRSTM_file (retrieved 2026-10-31)
- License: WiiBrew content. Used as a format reference; the header layout and
  field offsets used by `core/src/brstm.c` are described there.
- Use: primary specification of the BRSTM container and the DSP-ADPCM sample
  equation (`core/include/dsp_adpcm.h`). Facts and offsets are cited, not copied.

### Final Lap Stream Maker (FLSM)

- Author: Atlas. Windows tool written in C#, version 2.0 (2016-10-06).
- Source: https://mkwiiki.org/wiki/Final_Lap_Stream_Maker (checked 2026-10-31).
  The page gives author, platform, version and history. It does not state a
  license or terms of use for the program. The project owner searched the program
  and its sources and found no EULA or license (reported 2026-10-31; not verified
  independently).
- Author's statement on file use (https://mkwiiki.org/wiki/User:Atlas, last edited
  2026-07-03, checked 2026-10-31): "You may use any of my files for your own mods,
  but please give proper credit and ask me for confirmation in case you want to
  make any modification based on them." This applies to the author's files for
  mods. It does not clearly state whether it covers the FLSM executable, so it is
  recorded as stated, not extended. BRLoop does not copy, modify or redistribute
  any of the author's files.
- Use: observed behaviour only. `research/flsm_*` analyse the output file and the
  visible UI text, and `research/INFORME_TAREA01.md` documents the measured
  effect (sample-rate field change, factor 1.06812). No FLSM code or binary is
  distributed.

### LoopingAudioConverter (LAC)

- Project: https://github.com/libertyernie/LoopingAudioConverter
- License: GNU LGPL v3 or later, per `LoopingAudioConverter/About.html` in the
  project (checked 2026-10-31, version 2.5 text). The GitHub API reports
  `license: null` for the `version-3.0` branch, so the license must be re-checked
  before any LAC code is reused. Its source code is not used by BRLoop.
- Use: read for its README and About information. The `tools\vgmstream\test.exe`
  that LAC bundles was used as a reference decoder during development.
- LAC bundles components with their own licenses: vgmstream (above), VGAudio (MIT,
  above), and RSTMLib/BrawlLib for BRSTM writing. Those components are not used by
  BRLoop.

### BrawlLib / BrawlBox (libertyernie/brawltools)

- Project: https://github.com/libertyernie/brawltools (README checked 2026-10-31).
- License: mixed. The README states that code added by libertyernie is available
  under the MIT No Attribution license or LGPL v3 or later. The original BrawlLib
  code comes from Kryal and BlackJax96, and its license terms are not stated in
  that README. The GitHub API reports `license: null`.
- Use: **not used** by BRLoop. Only noted because LAC uses it for BRSTM writing.

## Consulted but not used

- **openrevolution** (https://github.com/ic-scm/openrevolution): licensed under
  GPL-3.0 (`LICENSE` checked 2026-10-31). Its README was reviewed to compare
  scope. No code was read or copied for BRLoop.
- **nikku** (https://github.com/kenrick95/nikku): MIT (`LICENSE.md` checked
  2026-10-31, copyright 2019 Kenrick). Its README was reviewed to compare scope.
  No code was copied for BRLoop.

## Project license

BRLoop itself is released under the MIT license. See [`LICENSE`](LICENSE).
