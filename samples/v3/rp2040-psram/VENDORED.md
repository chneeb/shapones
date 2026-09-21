# Vendored copy of polpo/rp2040-psram

Upstream: <https://github.com/polpo/rp2040-psram>
Vendored at commit `419375d5af2af2c72af63cdab6c6df2c558a3a9c`
("Add PSRAM_DEBUG define to enable debug printing"), which was `origin/main`
at the time — this is upstream's tip, not an older pin.

This was a git submodule until 2026-09-21. It became a checked-in copy because
the PicoCalc needs changes that do not exist upstream and cannot be carried as
a submodule pin:

- **QPI (quad) support** is an unmerged pull request,
  [#15 "Initial QSPI(QPI) support"](https://github.com/polpo/rp2040-psram/pull/15),
  open since 2025-08-18.
- **The quad read section needs a PicoCalc-specific fix** on top of that PR: its
  read loop carries one turnaround clock too many at 50 MHz SCK, so reads come
  back shifted by exactly one nibble. See `ROADMAP.md` section 3.

Keeping the directory path unchanged means `samples/v3/CMakeLists.txt` and every
reference in `CLAUDE.md` and `ROADMAP.md` still resolve.

## Local changes

1. **PR #15, "Initial QSPI(QPI) support"** (`shtirlic`, upstream commit
   `bdf754f`) applied as-is to `psram_spi.c`, `psram_spi.h`, `psram_spi.pio`.
   Adds a `quad` parameter to `psram_spi_init_clkdiv()`, a `qspi_psram` PIO
   program, quad command variants, and `psram_spi_uninit()`.

2. **PicoCalc quad read-phase fix** (`psram_spi.pio`, `psram_spi.h`). The PR's
   quad read loop carries one turnaround clock too many at 50 MHz SCK, so reads
   come back shifted left by exactly one nibble. Removing the
   `jmp readloop_mid` after `set pindirs 0` makes the loop enter at `readloop`
   instead, one clock earlier. Because that entry samples `y+1` times rather
   than `y`, every quad read length now passes `y-1`:

   | | was | now |
   |---|---|---|
   | `read8_quad_command[1]` | 2 | 1 |
   | `read16_quad_command[1]` | 4 | 3 |
   | `read32_quad_command[1]` | 8 | 7 |
   | `psram_read()` quad | `count * 2` | `count * 2 - 1` |

   Verified on hardware: 4 KB read back with zero errors at 50 MHz, against a
   pattern written over SPI immediately beforehand. See `ROADMAP.md` section 3
   for how it was diagnosed and for the variant matrix this came from.

**Known upstream issues left alone**, because nothing here depends on them:

- `psram_spi_uninit()` **never disables the state machine** — there is no
  `pio_sm_set_enabled` call anywhere in `psram_spi.c`. It removes the program
  from instruction memory while the SM is still executing it, so the SM runs
  whatever lands at those addresses and toggles CS/SCK/SIO at the part. This is
  not theoretical: it put the chip back out of QPI mode and made every read
  return zeros. `psram_loader.cpp` works around it by disabling the SM itself
  before calling uninit. The library's own `psram_qpi_init()` has the same
  pattern and the same exposure.
- `psram_spi_uninit()` sends its `0xF5` exit-QPI *after* unclaiming both DMA
  channels, so the command may not go out at all.
- That exit command is mis-sized: `{8, 0, 0xF5}` asks for 8 nibbles in QPI
  framing while supplying 2. Correct would be `{2, 0, 0xF5}`.

Both matter to anyone who tears a QPI instance down and expects the part to
return to SPI mode. `psram_loader.cpp` initialises once and never exits QPI.
