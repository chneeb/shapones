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

**None on `main`.** This directory is byte-for-byte upstream `419375d`, so the
vendoring can be verified by diffing against upstream.

The copy exists anyway because the QPI work needs it, and that work lives on
the **`psram-qpi`** branch: <https://github.com/chneeb/shapones/tree/psram-qpi>

That branch carries, on top of this:

1. **PR #15, "Initial QSPI(QPI) support"** (`shtirlic`, upstream `bdf754f`) -
   a `quad` parameter on `psram_spi_init_clkdiv()`, the `qspi_psram` PIO
   program, quad command variants, and `psram_spi_uninit()`.
2. **A PicoCalc quad read-phase fix.** The PR's read loop carries one
   turnaround clock too many at 50 MHz SCK, so reads come back shifted left by
   exactly one nibble. Removing the `jmp readloop_mid` makes the loop enter one
   clock earlier; since that samples `y+1` times, every quad read length passes
   `y-1`. Device-verified: 4 KB read back clean.
3. Loader integration, a QPI marker check at init, and an SPI fallback.

Keeping the vendored directory on `main` rather than restoring the submodule
means that branch merges cleanly when QPI is finished, instead of colliding a
directory against a gitlink.

**Status: QPI is not working end-to-end.** Standalone firmware reaches
~21.9 MB/s with all four data lines sound, but the same sequence inside
`psram_loader` does not come up. `ROADMAP.md` section 3 has the evidence and
what is left to try.

## Known upstream bugs

These bite anyone using this library, on `main` or the branch:

- `psram_spi_uninit()` **never disables the state machine** - there is no
  `pio_sm_set_enabled` call anywhere in `psram_spi.c`. It removes the program
  from instruction memory while the SM is still executing it, so the SM runs
  whatever lands at those addresses and toggles CS/SCK/SIO at the part.
  Observed effect: the chip drops out of QPI and every read returns zeros.
- `psram_spi_uninit()` sends its `0xF5` exit-QPI *after* unclaiming both DMA
  channels, so the command may not go out at all.
- That exit command is mis-sized: `{8, 0, 0xF5}` asks for 8 nibbles in QPI
  framing while supplying 2. Correct is `{2, 0, 0xF5}`.
- The part **keeps its mode across a warm reset**. A reflash resets the RP2350
  but not the PSRAM, so a run that ends in QPI leaves the next boot sending
  SPI-framed commands to a chip that is not listening.
