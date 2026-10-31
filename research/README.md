# Research

Investigation notes and one-off scripts used to understand the BRSTM format and
to compare playback speed against Final Lap Stream Maker. They are not part of
the BRLoop application: nothing in `app/` or `core/` runs them.

The decisions they produced are implemented in `core/` and `app/`, and the test
harness in `tests/` cites them.

## Reports

- `INFORME_TAREA01.md`: format investigation and the reference collection.
- `INFORME_TAREA02.md`: decoder and playback findings.
- `INFORME_MULTICANAL.md`: meaning of the stereo pairs in multichannel files.

## Scripts

- `brstm_header_probe.py`, `brstm_inventory.py`: read BRSTM headers and report metadata.
- `dsp_formula_probe.py`, `dsp_residual.py`, `dsp_final_checks.py`: check the DSP-ADPCM formula against vgmstream.
- `adpc_mismatch_probe.py`: why per-block decoding from the ADPC table differs in some files.
- `loop_alignment.py`, `loop_verify.py`: loop point alignment and wrap-point checks.
- `speed_loop_experiment.py`: offline prototype of looped, variable-speed playback.
- `flsm_*.py`, `flsm_t0.ps1`, `flsm_drive_msg.ps1`: analysis and automation of Final Lap Stream Maker.
- `wav_compare.py`: compares two decoded WAV files.

## Running the scripts

Scripts read the reference collection from the `MKW_COLLECTION` environment
variable, and some use vgmstream through `MKW_VGMSTREAM`. They need your own
`.brstm` files, which this repository does not include.

The `flsm_*` scripts drive a Windows GUI and can take focus while they run.
