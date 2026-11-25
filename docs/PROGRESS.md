# Progress log - real port of Alundra 2 (FR, SLES-02601)

Goal: rewrite the game as readable C, then replace the simulated console with native PC code.
First visible target: 360-degree analog movement and right-stick camera. Then widescreen, 60 fps.

## Done
- Disc extraction (`tools/extract_disc.py`), archives split, 856 MB in `extracted/` (git-ignored).
- Code map (see `docs/DECOMP_NOTES.md`): main exe `0x80017000`, GAME00/EVENT00 overlays `0x80084ED0`.
- splat configs: `config/main.yaml`, `game00.yaml`, `event00.yaml` (2571 functions). Regenerate with
  `tools/build_symbols.py` then `python -m splat split config/<x>.yaml` (`.venv-decomp`, Python 3.12).
- Names: 38 BIOS thunks, 17 PsyQ library functions (from debug strings), 3 libpad functions.
- Test harness: `tools/run_scenario.py` + `runtime_ctl.py` + `play_to_gameplay.py` (stall detection).
- Controller path understood (details in `docs/DECOMP_NOTES.md`):
  - raw pad buffers `0x80082030`; libpad user buffer `0x8006AED0`; per-pad record `0x8006AF20` (stride 0x10C,
    sticks at +0x104 LX / +0x108 LY as signed ints);
  - game input record `0x80060C40` built by `func_8001BF70`: +2 held buttons, +8 direction index (-1 none),
    +0x10 angle = index*64 on a 512 circle (0 right, 128 up, 256 left, 384 down).
  - `func_800A09C0` (GAME00, `0x800A09C0`) = world heading from that angle + camera yaw (`0x8011BBD8`).
- First rewrite: `src/input_override.c` replaces `func_800A09C0` through `func_override_add_guarded`,
  faithful port + analog extension (A2_ANALOG=0 disables) + shadow check (A2_VERIFY=1, counters
  `a2.heading.calls` / `a2.heading.mismatch`). Built into `build-dev`.

## Not verified yet
- The shadow check has NOT produced a result: the scenario kept failing before gameplay.
  `play_to_gameplay.py` was just rewritten to read the screen (title cursor blinks: red arrow / white dash;
  see `tools/screen.py`), run untested. Run:
  `A2_VERIFY=1 A2_ANALOG=0 .venv-decomp/bin/python tools/run_scenario.py verify_heading:main`
  (about 4 minutes; do not poll it, wait for it).
- Analog behaviour in game (does Flint turn smoothly?) is untested: use `tools/analog_probe.py`
  with `--game decomp-build/game_analog.toml` once navigation works.

## Next steps
1. Get `verify_heading` green, then test the analog heading in game, then commit.
2. Speed from stick magnitude (movement code after `func_800A0A54` in GAME00).
3. Right-stick camera: find the camera yaw writers (`0x8011BBD8` users).
4. Raise `FO_MAX_OVERRIDES` (128) by forking psxrecomp when more functions are rewritten.
5. Name more functions; grow `symbols/manual.txt`; track with `tools/progress.py` (0 of 2571 rewritten).
6. Known issue: the recompiled runtime occasionally stalls in turbo mode (memory-card SIO, frame ~25k);
   `run_scenario.py` samples the threads and retries.

## Not part of the real port
`build-release`, `dist/` (v1 recompilation packages, with disc, never publish), `scripts/package_*.sh`.
