# Progress log - real port of Alundra 2 (FR, SLES-02601)

Goal: rewrite the game as readable C, then replace the simulated console with native PC code.
Priority: a 1:1 port first (release 1.0), improvements after (see `PLAN.md`).

## Phase 1 (mapping and tooling) - done
- Disc extraction (`tools/extract_disc.py`), archives split, 856 MB in `extracted/` (git-ignored).
- Code map (`docs/DECOMP_NOTES.md`): main exe `0x80017000`; GAME00/EVENT00/GAME01-03 overlays at
  `0x80084ED0`; 17 code modules in `PACK1.PAK` at `0x80149AE8`. 3070 functions split by splat.
- Data formats inventory: `docs/DATA_FORMATS.md` (`tools/inventory_data.py`).
- Names: 38 BIOS thunks, 17 PsyQ functions from debug strings, libpad, libgpu entry points
  (`DrawOTag`, `LoadImage`, ...), `VSync`/`v_wait` (`symbols/manual.txt`).
- Harness: `tools/run_scenario.py` (stall detection, `--port` for parallel instances),
  `runtime_ctl.py`, `play_to_gameplay.py`, `record_session.py`.
- 1:1 verification (`docs/VERIFICATION.md`): reference session replay, `tools/replay_check.py`,
  `tools/replay_diff.py` with the RAM / GPU journal / VRAM / cycle tracer `src/a2_diag.c`.
- First rewrite: `src/input_override.c` replaces `func_800A09C0` (player heading), faithful port with
  the original's register state and cycle cost; analog extension kept for phase 7 (`A2_ANALOG=0`).

## Phase 2 (PsyQ libraries) - done
- `tools/psyq_sigs.py` (public lab313ru signatures, cloned next to the repo): 149 objects,
  547 functions, 414 names; SDK ~4.5. Details and map: `docs/LIBRARIES.md`.
- `tools/progress.py` splits game code (2378 functions then, 2707 once all code was found) from
  library code (694, phase 4).
- The end of the main executable (105 functions) is a game-specific sound driver, not libspu/libsnd.

## Phase 3 (the game in C) - done
- All 2707 game functions are C: 2678 translated from the MIPS by `tools/a2lift.py`
  (`src/game/lifted/`, exact semantics and the framework's timing model), 29 rewritten by hand
  (`src/game/`: input, system, frame pacing and display lists, camera).
- The count grew while translating: 77 functions reached only through pointers
  (`tools/find_hidden_functions.py`), and four hand-written assembly libraries loaded at
  `0x80010000` (`AL22`, PACK0 entries 106-109, 256 functions).
- Verified on two recorded sessions (14 644 and 19 062 game frames): GPU journal and VRAM
  identical every frame, no game function executed outside C (`A2_LIFT_COVERAGE`), RAM identical
  outside the documented timing zones (`docs/VERIFICATION.md`).
- Finding: libetc's `VSync` wait ends a few loop turns short of its timeout in this timing model;
  code running in the VBlank interrupt must charge the original's exact cycles.

## Next (phase 4, readable rewrites continue in parallel)
- Native files, input and memory cards first, then VSync/timing, GTE, GPU, sound.
- Readable rewrites: loader, main loop, projection, actors.

## Not part of the real port
`build-release`, `dist/` (v1 recompilation packages, with disc, never publish), `scripts/package_*.sh`.
