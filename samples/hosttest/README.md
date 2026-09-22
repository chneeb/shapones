# Offline shapones harness

Runs a ROM headless on a PC and dumps PPU state. `core/` is
platform-independent, so the whole emulator builds here with a debugger
attached and no hardware in the loop.

```bash
make
./shapones-host rom.nes [frames] [out.ppm] [start]
```

`start` presses START on a fixed schedule, enough to walk Tetris from its
copyright screen into a game. `out.ppm` is the rendered frame — convert with
PIL or ImageMagick and look at it.

## Why

Diagnosing a rendering bug on the device costs a flash cycle per hypothesis.
Five went on Bubble Bobble before this existed. Here an iteration is seconds.

It is also **single-threaded**: `cpu::service()` and `ppu::service()` alternate
in one loop, so there is no core-0/core-1 race. A bug that reproduces here is
an emulation bug, not a concurrency one — which is worth knowing before
hunting for either.

## What it prints

- `PPUCTRL` / `PPUMASK`, decoded (pattern table selection, background and
  sprite enables)
- the palette, background banks first
- nametable occupancy, and the first rows as tile indices — sensible structure
  means the CPU wrote a real screen
- the CHR remap table, with whether each physical block actually holds data
- pattern bytes for specific tiles, to tell "blank tile" from "wrong tile"
- a colour histogram of the rendered frame

## Validating it

Run a ROM known to work first. `smb.nes` should render its title screen
recognisably; if it does not, the harness is wrong, not the emulator.
