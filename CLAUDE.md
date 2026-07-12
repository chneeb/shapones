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

**Interlaced update** (`INTERLACE_LCD` in `picocalc_nes.cpp`, default on): a full-frame DMA is 320×300×2 = 192 KB — at ~75 MHz SPI that's ~20 ms, which was the frame-rate ceiling (~40 fps at the old 62.5 MHz SPI). With interlacing, each frame still converts all 300 rows but transfers only every *other* row (even/odd parity alternating via `lcd_field`); untouched rows keep the previous frame on the persistent panel. Each row is a 1-line async DMA that overlaps the next scanline's `cpu::service()`/conversion, so halving the transferred data roughly doubles the achievable rate — SMB3 now holds ~60 fps (the frame-pacing cap). The cost is faint interlace combing on fast vertical motion. Set `INTERLACE_LCD 0` for the original single full-frame path.

**Keyboard**: I2C peripheral at address `0x1F` on `i2c1` (SDA=GP6, SCL=GP7), polled from a 1 kHz hardware alarm ISR. Key codes are decoded in `kbd_interrupt()` inside `common.cpp` and written into `input_pins[]`.

**SD card**: FatFS (`samples/fatfs/`) over SPI. Picocalc-specific pin mapping is in `samples/fatfs/source/mmc_pico_spi.c` (TX=GP19, RX=GP16, SCK=GP18, CS=GP17).

**ROM location** (`boot_menu.cpp`): `enum_files()` scans the `nes/` subfolder for `*.nes` (see `ROM_DIR`), falling back to the SD root if `nes/` doesn't exist (`FR_NO_PATH`). The winning directory is recorded in the `rom_dir` static so `load_nes()` can build the full path. The menu shows bare filenames; it's `nes/` **or** root, not a merged listing.

**Host interface** (`host_intf.cpp`): implements `shapones::semaphore_*` using `pico/sem.h`, `shapones::spinlock_*` using hardware spin locks, and `get_time_us` via `get_absolute_time()`. The `fsys::*` functions are stubs (save-state not supported in this port).

### ROM size and PSRAM (Pico2)

`load_nes()` (`boot_menu.cpp`) picks the path per ROM: it pre-checks free heap with `sbrk(0)` + `mallinfo().fordblks`, `malloc`s the whole file if it fits (SRAM path), and otherwise falls back to the PSRAM path. Only if *both* fail does it show "ROM too large (NNkB)" and return to the menu (the Pico SDK sbrk panics on OOM instead of returning NULL, hence the pre-check).

**Small ROMs (fits in heap, ≈ ≤ 258 KB)**: the entire `.nes` file is `malloc()`'d into SRAM and `map_ines()` sets `prgrom`/`chrrom` as direct pointers into that buffer. Available heap is ~275 KB total; subtract shapones init allocations (~16 KB) leaving ~258 KB safe headroom. No bank-cache overhead — the fast path.

**Large ROMs (doesn't fit in heap)**: streamed into PSRAM via `psram_loader.cpp` (`psram_load_nes()`). The Picocalc has 8 MB PSRAM on GP20 (CS) / GP21 (SCK) / GP2 (MOSI) / GP3 (MISO), driven by the [polpo/rp2040-psram](https://github.com/polpo/rp2040-psram) library on **PIO1** (PIO0 is taken by the LCD). The library is a git submodule at `samples/v3/rp2040-psram/`, wired into the build via `add_subdirectory` (it's an INTERFACE lib, so `psram_spi.c` compiles into `picocalc_nes` and picks up the `PSRAM_PIN_*` compile defs from CMakeLists). `psram_loader_init()` runs once in `main()` after `picocalc::init()`; it also resets loader state so a warm reboot into a different game starts clean.

**PSRAM SPI timing** (`psram_loader_init`): the system clock is **300 MHz** (`SYS_CLK_FREQ` in `common.hpp`), far above the polpo library's default clkdiv of 1.0. Reliability here is a **sampling-phase** problem, not a raw-speed one — a timing sweep found pass/fail oscillating with clkdiv (fails at both fast *and* slow ends). The known-good operating point is a **~100 MHz PIO state-machine clock** (~50 MHz SCK) with the **non-fudge** PIO program (the extra read-sync cycle lands wrong at this speed). So the divisor is computed as `SYS_CLK_FREQ / 100 MHz` (`psram_spi_init_clkdiv(pio1, -1, SYS_CLK_FREQ/(100*MHZ), false)`) → **3.0 @ 300 MHz**, 2.5 @ 250 MHz; a `static_assert` guards clocks that don't divide cleanly. The passing window (2.0–3.0 @ 250 MHz) was verified with 0 errors over the full 384 KB; keeping the SM clock at ~100 MHz reproduces the same electrical timing from any system clock. Do **not** change the divisor without re-verifying against a full bulk read/verify — a 16-byte round-trip is too weak to catch marginal timing. The build must define `PSRAM_ASYNC` in CMakeLists even though the loader only uses blocking transfers: the library's async inline helpers are unguarded and won't compile as C++ without it (freesci-archive gets away without it because its copy compiles as C).

**PRG cache (PSRAM)**: a 96 KB, 12 × 8 KB set-associative victim cache (`prg_sram`) in SRAM; `prgrom` is set to `prg_sram - PRG_SLOT_OFFSET*8KB` so a slot-encoded remap entry (`≥ PRG_SLOT_OFFSET`) indexes straight into it. There are only 4 CPU PRG windows, so the extra 8 slots retain recently-used banks — a window switching *back* to a resident bank is a free hit with no PSRAM read (SMB3 gameplay churns > 8 banks; at 8 slots it thrashed down to ~35 fps, 12 slots holds ~55). Bank switches are handled **synchronously** by `prgrom_bank_switch_hook` (added to the core in `memory.hpp`/`memory.cpp`): when a mapper calls `prgrom_remap()` mid-`cpu::service()`, the hook runs `load_prg_bank()`, which finds/loads the bank in a slot and rewrites the table entry to the slot-encoded value, so `prgrom_read()` never sees a raw physical index that would underflow the shifted pointer. `psram_sync_prg()` (after each `cpu::service()` and after `reset()`) reconciles any raw entries the hook didn't. **Eviction correctness is critical**: a victim slot is only ever one **not pinned by another window's current mapping** (with 12 slots > 4 windows one always exists). An earlier LRU that could evict a still-referenced window's slot silently mapped that window to the wrong (but valid) bank → CPU executed wrong code → runaway JSR → 6502 stack overflow. This only bites large MMC3 games doing single-window bank switches, so small ROMs never exposed it.

**Performance note**: the frame rate is gated by two things. (1) **LCD transfer** — a full-frame DMA didn't fit the 16.6 ms/60 fps budget, which capped it at ~40 fps; interlaced update (above) halves it and lifts the cap to the ~60 fps frame-pacing limit (`FRAME_DELAY_US = 16666` in `ppu_loop`, which hard-caps game speed at real-time — it cannot run *too fast*). (2) **PSRAM bank switches** — the PPU (Core 1) is throttled to CPU cycle progress (`ppu.cpp` early-returns if it would lead the CPU), so anything stalling Core 0 lowers fps; each PRG cache **miss** is a slow ~8 KB PSRAM read (≈264 SPI transactions, the polpo protocol caps a transfer at 31 bytes) run on Core 0 inside `cpu::service()`. The victim cache keeps MMC3's working set resident so most gameplay bank switches hit. With interlacing + 300 MHz + a 12-slot cache, SMB3 holds ~55–60 fps. Bump `PRG_SLOTS` further (SRAM permitting — ~14 is the practical ceiling; re-check `arm-none-eabi-size` BSS) if a game's working set exceeds 12 banks.

**CHR cache (PSRAM)**: all CHR-ROM is **fully pre-cached** into a `malloc`'d SRAM buffer at load time and `chrrom` points at it directly. This replaced an earlier slot-cache scheme that caused Core 1 BusFaults (Core 1 reads CHR continuously and could see a raw physical index before a sync fixed it). `psram_sync_chr()` is therefore a no-op. Trade-off: CHR size is bounded by free heap (fine for SMB3's 128 KB; a >~256 KB CHR game would not fit). CHR-RAM games (`chr_pages == 0`) use the core's `chrram` as usual.

**`map_ines()` with PSRAM**: only the 16-byte iNES header is kept in SRAM; `map_ines()` is called with a header-only buffer so mappers initialise their remap tables, then `prgrom`/`chrrom` are overridden to point at the caches above.

**Nunchuck disabled with PSRAM**: the build defines `DISABLE_NUNCHUCK` (GP4/GP5 conflict with PSRAM on the Picocalc PCB); `nunchuck_init()`/`nunchuck_poll()` in `common.cpp` are `#ifndef`-guarded. Input is via the I2C keyboard regardless. `PIN_RAM_CS` in `picocalc.hpp` moved GP21→GP20 to clear the PSRAM SCK pin.

### Other samples

| Sample | Target |
|---|---|
| `samples/xiao/rp/` | Seeed XIAO RP2350 — the most complete reference implementation; uses `pico_extras` |
| `samples/pico_ws19804/` | Pico + WS19804 LCD breakout (original Pico/RP2040) |
| `samples/wxapp/` | Desktop wxWidgets app for development/testing |
| `samples/picopad/` | PicoPad handheld (uses PicoLibSDK, not Pico SDK) |
