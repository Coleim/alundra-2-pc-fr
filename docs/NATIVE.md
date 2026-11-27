# The standalone engine (`src/native/`, target `alundra2`)

Phase 4 replaced the simulated console (PSXRecomp + OpenBIOS) by a small engine that runs
the game's C code directly. The game code is the one of phase 3 (2 678 functions translated by
`tools/a2lift.py`, 29 rewritten by hand); the PsyQ library is translated the same way
(`src/game/lifted/lib_*.c`), except the parts replaced by native code listed below.

```
src/game/lifted/*.c   game code + PsyQ library, translated (generated from the player's disc)
src/game/*.c          game functions rewritten by hand
src/native/           the engine
    engine.c          guest memory, function table, calls, interrupts, boot, longjmp
    a2_native.h       memory access and load-delay helpers used by the translated code
    libetc.c          VSync, vertical blank, callbacks, work clock, deterministic mode
    bios.c libc.c     BIOS calls (A0/B0/C0), critical sections, longjmp, _patch_gte
    gpu.c libgpu.c    software GPU (VRAM, primitives) and libgpu's drawing entry points
    gte.c             geometry coprocessor (bit-exact)
    cd.c disc.c xa.c  CD-ROM drive at its registers, the imported disc, XA-ADPCM audio
    io.c              DMA controller, interrupt controller, root counters
    spu.c             sound chip at its registers (the game's own sound driver programs it)
    mdec.c            movie decoder at its registers (libpress unchanged)
    pad.c card.c      libpad (DualShock in analog mode), libmcrd (.mcd card images)
    host.c            SDL3: window, sound, pads, files, first-launch import, test options
```

## Running the game code

- **Memory.** 2 MB of RAM and the 1 KB scratchpad are two arrays; the game keeps its 32-bit
  addresses (`a2n_r32`/`a2n_w32`, mirrors handled). Other addresses go to the I/O regions
  registered by the modules (CD, DMA, SPU, MDEC...).
- **Registers.** `CPUState` holds the MIPS registers, HI/LO, the GTE registers and the load
  delay slot, exactly as the translated code expects (`a2_lift.h`, `A2_STANDALONE` branch).
- **Functions.** `a2n_dispatch(target)` looks the address up in a table where each address may
  have several candidates, tried in this order: native replacement, hand-written function,
  translated function. Overlay code is guarded by its first words (the function only runs if
  that overlay is loaded). A native function may return 0 to let the original run after it
  (ResetGraph, PutDispEnv and SetDispMask do: the engine records what it needs, the library
  keeps its own variables right).
- **Interrupts.** No CPU is simulated, so there are no cycles: interrupts (vertical blank, CD,
  DMA, GPU) are posted as events and delivered at the translated code's control transfers
  (`A2_EDGE` → `a2n_edge`) when interrupts are enabled, on the libetc interrupt stack, with the
  interrupted registers saved. The order matters: a finished DMA's callback runs before the
  next CD sector, as on the console.
- **Time.** Real-time mode: a vertical blank every 20 ms of the host clock (PAL), waits sleep
  until the deadline. `A2_DETERMINISTIC=1`: no clock, a vertical blank when the game waits for
  one, or after 200 000 control transfers (busy waits still see time pass).
- **Soft reset.** The game's `longjmp` (error recovery) unwinds the host stack to the engine's
  top and re-enters the translated code right after the matching `setjmp` (the translator adds
  an entry point there).

## Devices

- **CD drive** (`cd.c`): commands, responses and interrupts of the real drive; libcd, libds and
  the streaming library run unchanged. Data reads start at the vertical blank after `ReadN`
  (the game checks the drive's status once) and then deliver a sector every 4 000 control
  transfers once the previous one was taken: loads are much faster than on the console while
  the game's main code still runs between sectors. Streamed reads (`ReadS`: XA music, movies)
  keep the drive's speed (150 sectors/s). XA-ADPCM sectors go to the sound chip.
- **Disc** (`disc.c`): the import copies the ISO 9660 files of the player's image (data files as
  bytes, XA/STR files as raw 2352-byte sectors) and the 19 distinct sectors outside any file;
  `disc.txt` maps sectors to files. Every sector of the image reads back byte for byte.
- **DMA** (`io.c`): channels 0/1 (MDEC), 3 (CD), 4 (SPU) transfer at once; the DMA interrupt
  register is honoured (the streaming library enables channel 3's interrupt for the last sector
  of a movie frame only).
- **GPU** (`gpu.c`): software rasterizer of the GPU command set (polygons with Gouraud shading
  and 4/8/15-bit textures, lines, rectangles, fills, VRAM copies, texture window, four
  semi-transparency modes, mask bit, dithering, drawing area and offset). libgpu's drawing calls
  are native (`libgpu.c`): DrawOTag walks the ordering table and draws at once.
- **Sound** (`spu.c`): 24 voices, ADPCM, ADSR envelopes, pitch modulation, noise, reverb, CD
  input; 44.1 kHz, 882 samples per vertical blank.
- **MDEC** (`mdec.c`): decode, quantization and IDCT table commands, 15/24-bit output, DMA in
  and out.

## Building

```
python3 tools/a2lift.py                       # the game code, from the decomp of your disc
cmake -S . -B build-native -G Ninja -DA2_FRAMEWORK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-native --target alundra2
build-native/alundra2 --import ../alundra.cue  # once: copies the disc's files
```

Release packages (macOS universal with static SDL3, Windows cross-built with MinGW):

```
cmake -S . -B build-release-native -G Ninja -DA2_FRAMEWORK=OFF -DA2_FETCH_SDL3=ON \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake -S . -B build-win64-native -G Ninja -DA2_FRAMEWORK=OFF -DA2_FETCH_SDL3=ON \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64-macos.cmake
cmake --build build-release-native --target alundra2 && cmake --build build-win64-native --target alundra2
scripts/package_native.sh                      # dist/alundra2-fr-<version>-{macos,windows-x64}.zip
```

`-DA2_ENGINE_ONLY=ON` builds the engine without the game code (compilation check, CI).

## Test and diagnostic options

| Option / variable | Use |
|---|---|
| `--headless --frames N` | no window or sound, stop after N vertical blanks |
| `--input FILE` | scripted buttons: `START END PERIOD HOLD BUTTON+BUTTON` per line |
| `--shot-every N --shot-prefix P` | BMP of the screen every N vertical blanks |
| `--dump-vram F`, `--dump-ram F`, `--dump-audio F.wav` | state and sound at the end |
| `--verify-disc IMAGE.bin` | compare every sector the drive reads with the image |
| `--self-test` | memory card functions on a scratch card |
| `A2_DETERMINISTIC=1` | no host clock: identical runs |
| `A2_WATCHDOG=s` (`A2_WATCHDOG_EXIT=1`) | print the guest call stack when no vertical blank came for s seconds |
| `A2_TRACE_FN=f1,f2` | print calls and results of these functions |
| `A2_TRACE_NATIVE=1` | count the calls of native functions |
| `A2_LOG_CD=1`, `A2_LOG_DMA=1`, `A2_LOG_DRAW=1`, `A2_DUMP_DRAW_AT=n` | device logs |
| `A2_POKE=vblank:addr=value` | write a guest word at a given vertical blank |
| `A2_DISABLE=f1,f2` | leave these hand-written functions out (translated ones run) |
