# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ShapoNES is a work-in-progress NES emulator. The codebase is split into a platform-independent core library (`core/`) and platform-specific sample applications (`samples/`). The actively developed target is the **Clockworkpi Picocalc** (RP2350/Pico2) in `samples/v3/`.

## Building (Picocalc / Pico2)

```bash
cd samples/v3
mkdir -p build && cd build
PICO_SDK_PATH=~/Source/pico-sdk cmake .. -DPICO_BOARD=pico2
cmake --build . -j$(nproc)
```

Output: `samples/v3/build/picocalc_nes.uf2` — flash by copying to the Pico2 in BOOTSEL mode.

The Pico SDK lives at `~/Source/pico-sdk`. There are no tests.

## Code formatting

Google C++ style, 2-space indent. Apply with:
```bash
clang-format -i <file>
```

## Architecture

### Core (`core/`)

The emulator core is entirely platform-independent C++17. All platform dependencies are expressed through a set of functions the host application must implement, declared in `core/include/shapones/host_intf.hpp`:

- **Memory**: `ram_alloc` / `ram_free`
- **Synchronisation**: `spinlock_*` (1 lock), `semaphore_*` (2 semaphores: `SEMAPHORE_PPU`, `SEMAPHORE_APU`)
- **Time**: `get_time_us`
- **Filesystem**: `fsys::*` (needed only for save-states and config; can be stubbed out)

All core symbols live in the `shapones::` namespace. Subsystems have their own nested namespaces: `shapones::cpu`, `shapones::ppu`, `shapones::apu`, `shapones::input`, `shapones::memory`, `shapones::mapper`.

**Mandatory init sequence** (must follow this order):
```cpp
auto cfg = shapones::get_default_config();
cfg.apu_sampling_rate = <hz>;
shapones::init(cfg);           // inits spinlocks, semaphores, all subsystems
shapones::memory::map_ines(ines_bytes);  // load ROM
shapones::reset();             // reset CPU/PPU/APU state
```
Skipping `shapones::init()` leaves semaphores uninitialised and causes hangs.

**Error handling**: most core functions return `shapones::result_t`. Use the `SHAPONES_RET_ERR(expr)` macro to propagate errors.

**PPU rendering** produces one scanline at a time into a caller-supplied `uint8_t` line buffer (256 bytes of palette indices). Check `ppu::status_t` after each `ppu::service()` call:
- `is_end_of_visible_line()` — line is ready to display
- `is_end_of_frame()` — vsync point

**Mappers**: only 0–4 (NROM, MMC1, UxROM, CNROM, MMC3) are implemented.

**CPU address bus accuracy** (`core/src/cpu.cpp`): several NES accuracy requirements that are non-obvious on ARM:

- `addr_t = uint_fast16_t` is **uint32_t on ARM Cortex-M** — PC never wraps at $FFFF without explicit masking. All PC increments use `& 0xFFFF`: `fetch()`, `fetch_w()`, `opRTS()`; `bus_read_w()` masks `addr+1` too. `fetch_rel()` masks its result.
- **WRAM mirrors**: $0000–$07FF is primary WRAM; $0800–$1FFF are mirrors. The bus decoder covers the full mirror range with `addr < 0x2000` and `wram[addr & 0x7FF]`.
- **PPU register mirrors**: $2000–$2007 are the real registers; $2008–$3FFF mirror them every 8 bytes. The bus decoder covers the full range with `addr < 0x4000` and `0x2000 + (addr & 7)`.
- **Open-bus for write-only PPU registers**: reading $2000, $2001, $2003, $2005, $2006 (write-only) returns the last byte that was on the CPU data bus (`static uint8_t open_bus`, updated at the end of every `bus_read`). Returning 0 instead breaks games (e.g. Bubble Bobble) that RTI into PPU address space and execute the byte there as an opcode.
- **$2002 vblank flag** must clear immediately when read, not deferred. Done inline in `ppu::reg_read()` with `reg.status.raw &= 0x7F`.
- **Stack pointer wraps**: `pop()` with SP=0xFF wraps to 0x00 (reads $0100) — this is valid 6502 behaviour. Only log a warning, do not stop.

### Picocalc sample (`samples/v3/`)

Runs on two cores:

- **Core 0** (`cpu_loop`): runs `shapones::cpu::service()`, reads keyboard via I2C interrupt (`device_init` sets up a hardware alarm), converts palette-indexed scanlines to RGB565 and writes to the frame buffer, triggers LCD DMA transfers.
- **Core 1** (`ppu_loop`): runs `shapones::ppu::service()`, pushes completed scanlines into a small FIFO (`line_buff`, 8 entries × 257 bytes).

Communication between cores is a lock-free FIFO (`line_fifo_wptr`/`line_fifo_rptr`) — no SDK queues.

**Audio**: `PwmAudio` (in `pwm_audio.hpp`) drives a PWM pin via DMA double-buffering. The DMA completion IRQ calls back into `shapones::apu::service()` to refill.

**LCD**: `picocalc::` namespace (SPI + PIO0). Frame buffer is 320×300 RGB565 (`FRAME_BUFF_STRIDE = 640` bytes/row) — a 5/4 scale of the NES 256×240 output. Horizontal scale is Bresenham (no division in the inner loop); vertical scale doubles every 4th input line (`y % 4 == 3`). The 320×300 image is centred in the 320×320 display (10px border top and bottom). Transfers are started with `start_write_data()` and completed with `finish_write_data()`.

**Keyboard**: I2C peripheral at address `0x1F` on `i2c1` (SDA=GP6, SCL=GP7), polled from a 1 kHz hardware alarm ISR. Key codes are decoded in `kbd_interrupt()` inside `common.cpp` and written into `input_pins[]`.

**SD card**: FatFS (`samples/fatfs/`) over SPI. Picocalc-specific pin mapping is in `samples/fatfs/source/mmc_pico_spi.c` (TX=GP19, RX=GP16, SCK=GP18, CS=GP17).

**ROM location** (`boot_menu.cpp`): `enum_files()` scans the `nes/` subfolder for `*.nes` (see `ROM_DIR`), falling back to the SD root if `nes/` doesn't exist (`FR_NO_PATH`). The winning directory is recorded in the `rom_dir` static so `load_nes()` can build the full path. The menu shows bare filenames; it's `nes/` **or** root, not a merged listing.

**Host interface** (`host_intf.cpp`): implements `shapones::semaphore_*` using `pico/sem.h`, `shapones::spinlock_*` using hardware spin locks, and `get_time_us` via `get_absolute_time()`. The `fsys::*` functions are stubs (save-state not supported in this port).

### ROM size and PSRAM (Pico2)

**Small ROMs (≤ ~268 KB)**: the entire `.nes` file is `malloc()`'d into SRAM and `map_ines()` sets `prgrom`/`chrrom` as direct pointers into that buffer. Available heap is ~275 KB total; subtract shapones init allocations (~16 KB) and that leaves ~258 KB safe headroom. `load_nes()` pre-checks with `sbrk(0)` + `mallinfo().fordblks` before calling `malloc` — if the ROM is too large it shows "ROM too large (NNkB)" and returns to the menu rather than panicking (the Pico SDK sbrk panics on OOM instead of returning NULL). SMB3 (384 KB PRG) is 115 KB over this limit and requires PSRAM.

**Large ROMs (> 256 KB)**: use the PSRAM path via `psram_loader.cpp`. The Picocalc has 8 MB PSRAM on GP20 (CS) / GP21 (SCK) / GP2 (MOSI) / GP3 (MISO), driven by the [polpo/rp2040-psram](https://github.com/polpo/rp2040-psram) library on **PIO1** (PIO0 is taken by the LCD). The library lives at `samples/v3/rp2040-psram/` as a submodule.

**PSRAM cache design**: the core reads `prgrom[phys_block * 8KB + offset]` where `phys_block` comes from `prgrom_remap_table[]`. For PSRAM we keep a 32 KB PRG cache (4 × 8 KB slots) and an 8 KB CHR cache (8 × 1 KB slots) in SRAM, and point `prgrom`/`chrrom` at these caches. The steady-state invariant is `prgrom_remap_table[i] == i` (identity). When a mapper calls `prgrom_remap()`, it writes a non-identity value; `psram_sync_prg()` (called after every `cpu::service()`) detects the change, fetches the new 8 KB block from PSRAM into slot `i`, and resets the table entry to `i`. CHR cache updates (`psram_sync_chr()`) are deferred to VBlank to avoid a race with Core1's continuous CHR reads. Mid-frame CHR bank switches (rare, some MMC3 games) will be delayed one frame.

**`map_ines()` with PSRAM**: only the 16-byte iNES header is kept in SRAM; `map_ines()` is called with a header-only buffer so mappers initialise their remap tables, then `prgrom`/`chrrom` are immediately overridden to point to the SRAM caches.

### Other samples

| Sample | Target |
|---|---|
| `samples/xiao/rp/` | Seeed XIAO RP2350 — the most complete reference implementation; uses `pico_extras` |
| `samples/pico_ws19804/` | Pico + WS19804 LCD breakout (original Pico/RP2040) |
| `samples/wxapp/` | Desktop wxWidgets app for development/testing |
| `samples/picopad/` | PicoPad handheld (uses PicoLibSDK, not Pico SDK) |
