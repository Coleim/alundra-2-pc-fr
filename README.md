# Alundra 2 – Une Légende Est Née (France) — Recompiled

Static recompilation of the French PAL release of **Alundra 2** (`SLES-02601`),
adapted from [alexbeavs-ps1-ports/alundra-2-recomp](https://github.com/alexbeavs-ps1-ports/alundra-2-recomp)
(USA, `SLUS-01017`) and built on
[psxrecomp](https://github.com/Alexbeav/psxrecomp) +
[recomp-ui](https://github.com/Alexbeav/recomp-ui).

**BIOS:** none required — the bundled, MIT-licensed OpenBIOS is used. A
user-supplied SCPH-1001 dump remains an optional choice.

**Disc:** your own dump of `Alundra 2 - Une Legende Est Nee (France)`
(single-track MODE2/2352 CUE/BIN, 633193680 bytes). Disc images live under
`disc/` and are never committed.

## The standalone game (`alundra2`, phases 4 and 5)

The game now runs without the simulated console: its code, translated to C from your disc,
runs on a small native engine (`src/native/`, see `docs/NATIVE.md`): software GPU and GTE,
sound chip, CD drive reading the files imported from your disc, memory cards in the standard
`.mcd` format, SDL3 for window, sound and pads. No PSXRecomp, no BIOS.

```bash
# after the "game code in C" steps below (src/game/lifted/ must exist)
cmake -S . -B build-native -G Ninja -DA2_FRAMEWORK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --target alundra2
build-native/alundra2 --import "disc/Alundra 2 - Une Legende Est Nee (France).cue"
```

At first launch without `--import`, the program asks for the disc image itself. Players'
instructions (French): `docs/INSTALL_FR.md`. Release packages, without any file of the game:
`scripts/package_native.sh` (`docs/NATIVE.md`, "Building"). They are derived from the game's
code: personal use only.

## The reference runtime (PSXRecomp, used to verify the standalone game)

### Build (macOS / Linux)

```bash
brew install cmake ninja pkg-config freetype harfbuzz   # macOS
git submodule update --init --recursive
./psxrecomp/tools/ci/build_emitters.sh
python3 psxrecomp/psxrecomp_cli.py generate \
  --config game.toml --project-root . \
  --disc "disc/Alundra 2 - Une Legende Est Nee (France).cue"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target psx-runtime
```

### The game code in C (phase 3, optional)

The game's own code can run as C translated from your disc instead of the recompiler's
code (`docs/REWRITING.md`). It is generated locally, like `generated/`, and never committed:

```bash
python3 -m venv .venv-decomp && .venv-decomp/bin/pip install splat64[mips] rabbitizer
python3 tools/extract_disc.py "disc/Alundra 2 - Une Legende Est Nee (France).cue" --out extracted
for c in config/*.yaml; do .venv-decomp/bin/python -m splat split "$c"; done
.venv-decomp/bin/python tools/a2lift.py          # writes src/game/lifted/
cmake --build build-release --target psx-runtime
```

Without `src/game/lifted/`, the build still works: the original code runs wherever no function
was rewritten by hand. `A2_LIFTED=0` or `A2_OVERRIDES=0` switch the C code off at run time.

## Status

Version 0.3.0: the standalone game boots, plays its movies, and runs the game and its menus;
see `PLAN.md` (phase 6: full playthrough and 1:1 validation still to do).

<!-- framework-pin -->
**Framework:** `psxrecomp` and `recomp-ui` are submodules pinned to psxrecomp pin I 0.1.23 (psxrecomp `578bd3b`, recomp-ui `5302f1f`).
<!-- /framework-pin -->
