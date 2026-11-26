# Decompilation notes (French PAL release, SLES-02601)

Facts established from the disc and from a play session of the recompiled build.

## Memory map of the executables (RAM, KSEG0)

| File | Loaded at | Size | Notes |
|---|---|---|---|
| `SLES_026.01` | `0x80017000` | `0x46800` | main program, entry `0x8003CFCC`, gp `0x8005D168` |
| `GAME00.BIN` | `0x80084ED0` | `0xC4A18` | engine overlay |
| `EVENT00.BIN` | `0x80084ED0` | `0x77000` | cutscene overlay, replaces GAME00 at the same address |
| `GAME01/02/03.BIN` | `0x80084ED0` | `0x70888` / `0x76248` / `0x5FCF8` | same slot; code only in the first 35 / 29 / 25 KiB |
| `PACK1.PAK` entries 1, 2, 11, 13, 17, 18, 21, 23-26, 30-35 | `0x80149AE8` | 4-45 KiB | 17 interchangeable code modules |

The `GAME01-03` base was confirmed by the internal calls (all 481 / 398 / 329 `jal` resolve into the
module or the main program). The `PACK1` base comes from code snapshots taken in a play session
(entry 35 = file offset + `0x80119A4C`, entry 34 = file offset + `0x8011B988`, both `0x80149AE8`).
The other `PAK` and `GP` archives are data, loaded to varying addresses (`symbols/cd_loads.json`).

Function counts (splat, `tools/progress.py`): main 1103, GAME00 1264, EVENT00 204, GAME01 68,
GAME02 36, GAME03 30, PACK1 modules 365; total 3070.

## Archives

`PACK0/PACK1/PAKVCAST/PAKOMAKE.PAK` and `GRP_FR.GP` start with a table of u32 byte offsets
(768 or 512 slots). `tools/extract_disc.py` splits them. `STEREO.XA`, `MONO.XA` and
`A2STR.STR` are raw 2352-byte sector streams.

## Loader behaviour

The CD log shows each data sector DMA preceded by a 12-byte header read, and the logged
`lba` is the seek position of the command, not of each sector
(`tools/summarize_cd_loads.py`).

## Tooling

    python3 tools/extract_disc.py alundra.cue --out extracted
    python3 tools/name_bios_stubs.py extracted/files/SLES_026.01 \
        --names psxrecomp/recompiler/src/bios_call_names.cpp --out symbols/bios_thunks.txt
    python3 tools/build_symbols.py
    python -m splat split config/main.yaml

Everything generated from the disc (`extracted/`, `decomp/`) is git-ignored.

## Test harness caveats

- `build-dev` (`-DPSX_DEBUG_TOOLS=ON`) exposes the TCP debug server on port 4370; `tools/runtime_ctl.py`
  and `tools/play_to_gameplay.py` drive it. `--headless` gives no window and no sound.
- A long turbo run once stalled inside the memory-card SIO handling (`in_exc=1`, repeated
  CTRL writes from `0x80046318`, frame ~25763, opening cutscene). The starvation watchdog killed it
  after ~320 s. Earlier runs passed the same point, so this is intermittent: the harness must
  detect a dead runtime and restart instead of waiting.
- `0x80046318` is in `func_800462F8`, the libpad transfer start run from the VBlank interrupt
  (`SIO_CTRL = 0x40` reset, mode `0xD`, baud `0x88`): the controller is polled every frame inside the
  exception handler, so this write is a symptom (the game loop no longer advances), not the cause.
- `tools/stall_hunt.py` runs several recorded sessions at once (`run_scenario.py --port`: own debug
  port and memory-card folder per instance). A debug server that stops answering now counts as a stall
  (threads sampled, `runtime_ctl.Stalled`); before, it was taken for a normal exit.
- Launching several instances in the same second can fail at startup ("cannot publish state", mods):
  the hunt staggers the starts.
- 1:1 checks no longer use the TCP harness: `replay_check.py` and `replay_diff.py` replay a recorded
  session without turbo commands or injected input; no stall in any replay so far.
- The code around `0x80046300-0x80046910` in the main executable is the PsyQ low-level SIO/memory-card
  layer (`D_8005C52C` holds the SIO register base), not the controller reader.
- `PadInit`-style wrappers around the BIOS `InitPad`/`StartPad` thunks live at `0x8003D524` and
  `0x8003D5B4`, but have no direct callers: they are reached through a pointer table.

## Controller path (PsyQ libpad)

- Raw receive buffers, filled from the SIO interrupt handler: `0x80082030` (pad 1) and
  `0x80082053` (pad 2), 35 bytes each (`0x23` stride); matching transmit buffers at `0x80082078`.
- `PadInitDirect(pad1, pad2)` (`0x80047E98`) is called from `func_8002A510` with the user buffer
  `0x8006AED0`. That buffer is what the game reads: byte 0 status (0 = ok), byte 1 terminal id
  (`0x41` digital, `0x73` analog), bytes 2-3 buttons (active low), then the analog sticks.
- Found by diffing RAM with a button held (`tools/find_pad_buffer.py`): the button bit clears in
  both `0x80082032` and `0x8006AED2`, for LEFT, RIGHT, UP and CROSS.
- `PadGetState` (`0x80045A84`) and `PadInfoMode` (`0x80045B50`) match libpad's return values and
  `term` switch (1, 2, 3, 4).

## Replacing a function

`psxrecomp/runtime/include/func_override.h`: `func_override_add(id, addr, fn, credit)` swaps a guest
function (static EXE, overlay or dirty RAM) for hand-written C, with `func_override_call_original` and
`func_override_guest_call`; an overlay guard exists (`func_override_add_guarded`). Limit:
`FO_MAX_OVERRIDES = 128`. Once more functions are rewritten the limit must be raised
(fork the framework).

The runtime counts guest cycles per instruction (I-cache, load interlocks). A replacement must charge
the cycles of the path the original would take (`FO_CREDIT_SELF` + `psx_advance_cycles`), and leave
the same registers as the original; the rules and tools are in `docs/VERIFICATION.md`.

## libetc timing

`VSync(mode)` is `0x8003B1C8`: it reads the hblank root counter; `mode < 0` returns the vblank count,
`mode == 1` the hblanks since the last vblank, otherwise it waits through `v_wait` (`0x8003B340`, the
owner of the "VSync: timeout" string). GAME00 stores `VSync(1)` once per frame at `0x800FA7F4`, only
shown by a debug performance overlay (`func_80087E34`, gated by the byte `0x8005D170`).
