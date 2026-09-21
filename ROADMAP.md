# ShapoNES / Picocalc roadmap

Ordered by value-per-effort. Each item records what is actually in the tree
today, what to change, and the evidence behind it. Two sibling projects on
comparable hardware supplied most of that evidence:

- `~/Source/rp2040-ili9341-infones` — InfoNES on RP2350 + ST7789 (pico-sdk).
  Findings transcribed in `NOTES-from-infones.md`.
- `~/Source/pico-286` — 286 emulator, **same Clockworkpi Picocalc PCB**, and
  crucially the *same* `rp2040-psram` library (its `psram_spi.pio` is
  byte-identical to our submodule copy apart from the licence header).
- `~/Source/freesci-archive` — SCI interpreter on the same PicoCalc. Adopted
  the pico-286 PSRAM findings and got 396 MHz running (commits `3f570ca5`,
  `210f2abc`), which surfaced the *bring-up* work those findings do not
  mention. An older fork of the same PSRAM driver, so its driver-level
  measurements need care before being applied here.

---

## 0. Standing constraint: 1.30 V, and the clock stays high

**Core voltage stays at `VREG_VOLTAGE_1_30`** (`picocalc_nes.cpp:114`). This is
a decision, not a default. Two reasons:

- The PicoCalc is battery-powered, and within one voltage core dynamic power is
  linear in clock, while a voltage step is quadratic: 1.60 V costs
  (1.60/1.30)^2 = **1.51x** before a single extra megahertz (pico-286 `6a564e3`).
- More decisive: `VREG_VOLTAGE_MAX = VREG_VOLTAGE_1_30` in the SDK
  (`hardware/vreg.h`). 1.60 V is **above the SDK's sanctioned maximum** and
  requires defeating that limit. That is a different class of risk from an
  overclock.

So **396 MHz is off the table** — pico-286 device-tested that it needs 1.60 V
(`1a8dfbd`: 1.30 V hangs, 1.20 V does not start, with or without slower flash).

**The clock does not come down, though.** Lowering it is the obvious way to save
power and the wrong trade here: we are Core-0-bound (PSRAM misses and scanline
conversion), power within a voltage is only linear in clock, and 240 MHz would
buy ~20% core power for a direct fps loss. If battery life later becomes a goal
rather than a constraint, the lever to reach for first is the **backlight** —
pico-286 calls it the largest consumer on this board that costs nothing in
emulation speed and cannot destabilise anything (`652ecc9`).

**Field reports, for calibration only.** The forum's *Overclocking Pico 2*
thread has users running 300 MHz stable long-term at 37-38 C, and 370/387 MHz
without trouble. Encouraging for the 360 MHz target below — but **nobody states
a core voltage**, so it does not bear on the 1.30 V question at all, and Pico 2
boards there are not necessarily driving a PicoCalc's PSRAM, SD and LCD at the
same time. Anecdote, not evidence.

**The ceiling at 1.30 V is 360 MHz**, not 300: pico-286's "High" profile is
360 MHz at `VREG=15` — which is 1.30 V, since `VREG_VOLTAGE_1_30 = 0b01111`.
Marked worked-but-not-soaked. See step 2 below.

---

## 1. PSRAM SPI clock: we are leaving ~1.25-1.9x on the table

**Today:** `samples/v3/psram_loader.cpp:160` runs `psram_spi_init_clkdiv(pio1,
-1, SYS_CLK_FREQ / (100*MHZ), false)` — clkdiv 3.0 at 300 MHz, so a **100 MHz
state-machine clock = 50 MHz SPI** with the **plain** PIO program.

**Evidence** (pico-286, sweep + soak on Picocalc hardware at 396 MHz sys clock,
commits `c9c0458`, `7bd1691`, `d5aa195`):

| SPI | plain | fudge |
|---|---|---|
| 49 MHz | works, 2674 KB/s | dead |
| 66 MHz | works | dead |
| 79 MHz | works | dead |
| 99 MHz | works, 5012 KB/s | works, 4997 KB/s |

99 MHz + fudge soak-tested clean: 0 errors sustained, ~5.0 MB/s.

**Temper the headline figure.** pico-286's own numbers scale nearly linearly
(2.02x the SPI rate for 1.87x the throughput). freesci-archive, going
66.5 -> 99 MHz on an older fork of the driver, got only **1.25x for 1.49x** and
concluded it is transaction-overhead-bound. We are on the polpo library that
pico-286 measured, and our own note already records ~264 SPI transactions per
8 KB read (the protocol caps a transfer at 31 bytes), so expect nearer 1.9x than
1.25x — but the honest range is the spread, not the top of it.

Worth borrowing freesci's framing too, inverted: there, PSRAM bandwidth was not
the prize and the CPU clock was. Here it is the other way round, because PSRAM
misses sit on Core 0's critical path. What travels unchanged is the discipline
note that came with it — **MHz adds no bytes.** No overclock moves our ROM-size
or CHR-size ceilings (see the heap bounds in `CLAUDE.md`).

Since the PCB and the PIO programs are identical, this table is inherited
directly rather than re-derived. Porting their sweep/soak harness was
considered and **deliberately declined** — it would only reproduce numbers we
already have for this exact board. Revisit only if a future change makes our
electrical situation differ from theirs.

**Why it matters more here than there:** a PRG cache miss is an 8 KB read
performed on Core 0 *inside* `cpu::service()`. At 2674 KB/s that is ~3.0 ms
against a 16.6 ms frame; at 5012 KB/s it is ~1.6 ms. This is the mechanism
behind the ~35 fps SMB3 thrashing recorded in `CLAUDE.md` for an 8-slot cache.
Halving miss cost raises the floor exactly where the victim cache does not
help, and may let `PRG_SLOTS` come back down to free SRAM, or make CHR
streaming viable again (CHR is currently fully pre-cached into heap, so CHR
size is bounded by free heap).

**Correction to `CLAUDE.md`.** Our note claims reliability is a sampling-phase
problem with pass/fail "oscillating with clkdiv", failing at both fast *and*
slow ends. The sweep shows otherwise: the plain program passes monotonically at
49/66/79/99 MHz, and every slow-end failure was the **fudge** program used
below its valid range. `psram_spi.pio` documents the fudge variant's extra
read-sync cycle as required above **83 MHz** SPI and wrong below it. So this is
not an oscillating window to be feared — it is one coupled two-state choice.
That explanation has been corrected in `CLAUDE.md`. The "do not change the
divisor without a full bulk read/verify" rule **stands** — not because the
window is narrow, but because everything below is inherited from another
board and unmeasured on this firmware.

**The coupling is the actual trap:** SM below ~166 MHz needs `fudge=false`,
above needs `fudge=true`. Our call passes `false`. Raising the clock past
83 MHz SPI without also flipping that argument gives a **dead bus**.

### Step 1 — 1.5x, no overclock, and device-verified (do first)

Stay at 300 MHz; change clkdiv 3.0 -> **2.0** => 150 MHz SM = **75 MHz SPI**,
keep `fudge=false`. Exact integer divisor, comfortably under the 83 MHz fudge
threshold. One-line change plus the `static_assert` at `psram_loader.cpp:159`.

**This is no longer an inference.** pico-286 `1a8dfbd` records as a
device-verified operating point: *"300 MHz runs at 1.30 V, paired with
`PSRAM_SM_CLOCK_VAL=150000000` and `PSRAM_FUDGE_VAL=0` for an exact divider."*
Same PCB, same library, same system clock, same voltage, same PIO program —
their "Medium" profile, and what their board is currently running. It also
moots the `clk_sys` question below for this step, since nothing about our clock
differs from the measurement.

### ~~Withdrawn: 100 MHz SPI at 300 MHz~~ (dead end, do not attempt)

100 MHz SPI needs `fudge=true`, and at 300 MHz that is clkdiv **1.5**. Earlier
drafts called this "worth soak-testing" with duty jitter as the caveat.
pico-286 `652ecc9` is blunter and correct: the PIO divider is 16.8 fixed-point
and **dithers the cycle length** on a fractional value rather than dividing
evenly, which is *"fatal for a bus whose reliability is a sampling-phase
problem."* They now refuse a non-exact rate at boot outright.

At 300 MHz the exact dividers give 150 / 100 / 75 / 50 MHz SPI. **There is no
~100 MHz point.** Getting past 75 MHz SPI therefore requires changing the
system clock, which is the step below — the two were never separable.

### Step 2 — 360 MHz at 1.30 V (the real ceiling)

pico-286's "High" profile: **`CPU=360`, `VREG=15` (= 1.30 V), `PSRAM_SPI=90`,
`PSRAM_FUDGE=1`**. 360/2 = 180 MHz SM = **90 MHz SPI**, an exact divide-by-2,
with fudge correctly on (above the 83 MHz threshold). Marked *worked but not
soaked*.

That is the attractive package: ~90% of the PSRAM gain (90 vs 99 MHz) **plus
20% more emulator CPU**, at our existing voltage and inside the SDK's supported
range. Roughly 1.2x core dynamic power against today, versus ~2x for
396 MHz/1.60 V.

Three prerequisites, none of them the PSRAM divisor:

**A — flash divisor (see section 2; blocking).** Ours is 2, so 360 MHz would
put flash at **180 MHz**. This must be settled first and it is already
questionable at 300.

**B — pin `clk_peri`.** The SDK's `set_sys_clock_pll()` ties `clk_peri` to
`clk_sys` **undivided** ("reference clock for UART and SPI serial"). We pin it
nowhere — no `clock_configure` in `samples/v3/` or `samples/fatfs/`. The LCD and
PSRAM are PIO and immune (own clkdivs), but the **SD card is hardware `spi0`**
(`mmc_pico_spi.c:127`), so it rides the system clock. freesci-archive's symptom
at 396 MHz was serial dying right after the clock print and a HardFault on load
— corrupt SD reads feeding the resource loader.

**C — pin the LCD.** `boot_menu.cpp:216,232` set
`set_spi_speed(SYS_CLK_FREQ / 4)`, so LCD SPI would go 75 -> 90 MHz as a side
effect. The LCD is PIO-based (`picocalc.cpp:261`, `div = sys/2/speed`), so pin
it at 75 MHz explicitly first, and let a panel overclock be its own experiment.

**Ordering matters when raising:** voltage first, then clock — and flash before
both. pico-286's README is explicit that from a 396 build, `VREG=15` before
`CPU=360` momentarily runs 396 MHz at 1.30 V and hangs. We are raising rather
than lowering, so: flash timing, then (unchanged) voltage, then clock.

### Does the inherited table survive a change of `clk_sys`?

freesci records as a wrong guess that *holding the SPI rate constant does not
hold the sampling phase*: the PIO input synchronizer is clocked by `clk_sys`, so
its latency shrinks ~15 ns -> ~5 ns and MISO arrives most of a bit period early.
Taken at face value this would sink the whole inherit-the-table approach, since
pico-286 measured its table at 396 MHz `clk_sys` and we would apply it at 300.

**It probably does not apply to our copy.** Our driver bypasses the synchronizer
on MISO (`rp2040-psram/psram_spi.pio:86`,
`hw_set_bits(&pio->input_sync_bypass, 1u << pin_miso)`), which removes the
`clk_sys`-dependent term. What remains — pad, PCB and PSRAM t_CO delay — is fixed
nanoseconds, so holding SCK constant does hold the phase to first order.
freesci's copy is an older fork (it has no `PSRAM_FUDGE` equivalent) and likely
lacks the bypass.

This is reasoning, not a measurement, and it is the assumption the inherit
decision rests on — so it is the first thing to doubt if an inherited operating
point misbehaves. It also identifies the dependency precisely: the table travels
across `clk_sys` **because of** the bypass, not automatically.

**Also recorded, from freesci's other wrong guess:** deriving the divisor from
the *requested* clock rather than the achieved one puts SPI somewhere absurd if
`set_sys_clock_khz` falls back — a dead bus that looks exactly like an overclock
failure. `psram_loader.cpp:160` derives from the compile-time `SYS_CLK_FREQ`.
Our `set_sys_clock_khz(..., true)` asserts rather than falling back, so we would
halt instead of running wrong, but deriving from `clock_get_hz(clk_sys)` is
strictly more robust and is the same lesson as the build-system trap below.

### Build-system trap to adopt regardless

pico-286 commit `59bc839`: their 99 MHz soak silently ran at 50 MHz because the
clock was a literal rather than a CMake cache variable, so `-D` was ignored
while CMake cheerfully reported the requested value — the same trap
freesci-archive documents. Their mitigation: print the SPI rate **read back
from the PIO clock divider register**, not recomputed from the define. A knob
that fails to apply should be visible on the device. Worth doing whichever step
we take.

### Resolved along the way

`NOTES-from-infones.md` §4 asks what our LCD PIO clkdiv actually resolves to.
`setup_pio` computes `sys/2/(sys/4)` = exactly **2.0**, so our 75 MHz LCD SPI
is real, not a rounded-down 80 as happened in the InfoNES port.

---

## 2. Flash divisor — a live question about what we ship today

**Not just an overclock prerequisite.** `pico2.h:75` sets
**`PICO_FLASH_SPI_CLKDIV 2`** and we override it nowhere, so if that is what is
in effect at our 300 MHz, flash is running at **150 MHz** — above the ~133 MHz
these parts are typically rated for.

The board plainly works, so this is a question, not a known defect. But "works
at room temperature on one board" is exactly how marginal XIP presents, and the
divisor does **not** adjust when the clock changes: pico-286's README states a
300 MHz build gets divisor 3 (100 MHz flash, *"only safe up to 300 MHz"*) and a
396 build gets divisor 4, and that raising the clock at runtime without setting
flash first puts it at 120 MHz (at 360) or 132 MHz (at 396) *while the code
doing it is executing from flash*. That is what freesci-archive's
dead-before-serial at 252 MHz was.

**Independent corroboration.** The PicoCalc forum's *Overclocking Pico 2*
thread (<https://forum.clockworkpi.com/t/overclocking-pico-2/18226>) has users
reporting that *"so many peripherals (notably flash and USB) stop working at
relatively low overclock levels for reasons completely unrelated to heat"* —
which is this, from a source with no connection to pico-286 or freesci.

**Cheap and worth doing first:** print `QMI_M0_TIMING` (or the derived flash
clock) at boot, so the number is visible on the device rather than inferred from
a header. This is the same lesson as the build-system trap in section 1 — a knob
that silently fails to apply should be observable.

Then, if it is 150 MHz: decide whether to set the divisor explicitly. It is
blocking for section 1 step 2, since 360 MHz at divisor 2 would be 180 MHz.

---

## 3. QPI — the largest single PSRAM win available, and the pins are already free

pico-286 `e262c7b` verified against the **ClockworkPi Mainboard V2.0 schematic**
that the part is an **ESP-PSRAM64H** with all four data lines routed:
RAM_TX/RAM_RX/RAM_IO2/RAM_IO3 land on chip pins 5/2/3/7 = SIO0–SIO3.

pico-286's projection was ~3.3x, from a 32-bit access costing **72 cycles
single-bit against 22 in QPI** (~5.5 -> ~18 MB/s at 99 MHz). **That is too
optimistic** — see the measured numbers below: the same-board, same-width ratio
is **1.6-2.1x**. Still the largest single PSRAM win available, and it matters
more here than there because PSRAM misses sit on Core 0's critical path inside
`cpu::service()`.

**GP4/GP5 are already free on our side.** Our build defines `DISABLE_NUNCHUCK`
precisely because those pins conflict with PSRAM on this PCB — so the pins the
extra data lines need are the ones we already gave up. The blocker is software.

### What the vendored library already gives us (checked 2026-09-21)

We are pinned at **`419375d`, which *is* `polpo/rp2040-psram` `origin/main`** —
fetched and confirmed empty `HEAD..origin/main`. Upstream has **not** added QPI
driver support, and there is no newer version to move to. Its last commits are
"Add PSRAM_DEBUG define" and, before it, "Init miso pin; necessary for RP2350
support".

But the `.pio` file already ships both halves of the PIO-level work:

- **`.program qspi_psram`** (`psram_spi.pio:93`) — complete, with 4-bit write and
  read loops and the same falling-edge read note above 83 MHz.
- **`pio_qspi_psram_cs_init()`** (`psram_spi.pio:113`) — a full state-machine
  init: out/in/set pins across the 4 SIO lines, 2-bit sideset, shift config,
  pindirs, `pio_gpio_init` on all six pins, and the input-sync bypass across
  `0xf << pin_sio0`.

It is **dead code in the vendored copy**: `psram_spi.c:58` adds
`spi_psram_program`/`spi_psram_fudge_program` and line 78 calls the 1-bit
`pio_spi_psram_cs_init`. Nothing references the QSPI program or its helper.

**Our pin mapping fits the helper's constraints exactly**, which was not a given
— it requires CS and SCK adjacent (2-bit sideset) and the four SIO lines on
consecutive GPIOs:

| `pio_qspi_psram_cs_init` requires | ours (`samples/v3/CMakeLists.txt:71-74`) |
|---|---|
| `pin_cs`, `pin_cs+1` = CS, SCK | GP20, GP21 |
| `pin_sio0 .. pin_sio0+3` consecutive | GP2, GP3, GP4, GP5 |

MOSI=GP2 and MISO=GP3 are SIO0/SIO1, so SIO2/SIO3 land on **GP4/GP5** — the
pins `DISABLE_NUNCHUCK` already freed. The schematic routing pico-286 read and
the PIO helper's consecutive-GPIO requirement agree on this board.

**So the remaining work is the protocol layer, not the PIO layer:**

1. Enter QPI mode — ESP-PSRAM64H command `0x35`, sent over the existing 1-bit
   path before switching programs.
2. QPI-mode read/write wrappers — the current `psram_read`/`psram_write` build
   1-bit command frames.
3. QPI Read ID — which is the cheap feasibility probe anyway.

Smaller than "write a QPI driver", and it lowers the risk on this section. It
does **not** change the sequencing: GP4/GP5 have never been driven on *our*
board, so a hardware check still comes first — though it is now a better test
than a bare Read ID (see below).

*(Method note: an earlier pass grepped the C driver for `qpi`, which cannot
match `qspi_psram`. The conclusion was right by luck. Re-checked for `qspi` —
still absent from `psram_spi.h`/`psram_spi.c`.)*

Caveats worth carrying, all from `e262c7b`:

- A schematic proves *intent*, not a particular board.
- ~~Those two lines have never been driven, so a fault would be invisible~~
  — superseded: they carry real quad traffic on at least one PicoCalc (below).
  Still unverified on *our* board.
- A **QPI Read ID probe settles it** without touching the memory path — do that
  before any driver work.
- Validate with throughput **and** error count together: that separates "QPI
  never engaged" from "engaged but the extra lines are broken" from "works". A
  correctness-only test silently passes the first.

### It is already done, on our exact hardware (checked 2026-09-21)

**`polpo/rp2040-psram` PR #15, "Initial QSPI(QPI) support"** by **shtirlic**
(branch `shtirlic:qspi`, open since 2025-08-18, +264/-111 across `psram_spi.c`,
`psram_spi.h`, `psram_spi.pio`) — <https://github.com/polpo/rp2040-psram/pull/15>

Tested by its author on **ESP-PSRAM64H on a PicoCalc with Pico 2**: our chip,
our board. Measured **read** throughput from the PR body:

| sysclock | width | SPI | QSPI | ratio |
|---|---|---|---|---|
| 150 MHz | 32-bit | 2.26 MB/s | 3.68 MB/s | 1.63x |
| 150 MHz | 128-bit | 5.20 MB/s | 10.92 MB/s | **2.10x** |
| 230 MHz | 32-bit | 3.47 MB/s | 5.64 MB/s | 1.63x |
| 230 MHz | 128-bit | 7.97 MB/s | 16.75 MB/s | **2.10x** |

Two things follow.

**The realistic gain is 1.6-2.1x, not 3.3x.** These are same-board, same-clock,
same-width A/B numbers, which is much stronger evidence than a cycle count.

**The electrical risk is largely retired.** GP4/GP5 carry real quad traffic on a
PicoCalc, so the schematic's intent is borne out on hardware. What remains is
whether *our* board is sound, which is a much smaller question.

Author's own caveats: the code is *"pretty rough"*, you must enter SPI mode to
switch into QPI and back out again, variable-byte operations were incomplete,
and it has sat unmerged for over a year. So this is a starting point to be
reviewed and adapted, not a dependency to take.

**Second implementation for reference:** `siska-tech/koto-psram`, a `no_std`
Rust driver for "RP2040 boards with PicoCalc-style QPI PSRAM"
(<https://github.com/siska-tech/koto-psram>). Wrong language for us, but an
independent reading of the QPI entry/exit sequence.

**Pin mapping independently confirmed** by the forum thread *PSRAM on the
PicoCalc* (<https://forum.clockworkpi.com/t/psram-on-the-picocalc/17176>):
CS=GP20, SCK=GP21, MOSI/SIO0=GP2, MISO/SIO1=GP3, board *"connected for QSPI
setup"* per schematic, and the consecutive-GPIO requirement restated. Third
independent source agreeing with the table above.

### Transfer width is a separate lever, and may be cheaper than QPI

The table shows width mattering more than QPI does: at 150 MHz, SPI reads go
**1.34 -> 5.20 MB/s** from 16-bit to 128-bit, a 3.9x swing with no protocol
change at all. The two compound (16-bit SPI 1.34 -> 128-bit QSPI 10.92, ~8x).

Where we sit: `psram_loader.cpp:62` chunks reads at **31 bytes**, because
`psram_read()` writes the bit count into a **`uint8_t`** field
(`psram_spi.h:578`, `read_command[1] = count * 8`, so count <= 31). That is a
**driver-header limitation, not a protocol one** — the same limitation the PR's
wider variants address.

So before concluding QPI is the only path to more bandwidth, establish what our
8 KB bank load actually achieves per byte today. A PRG miss is a bulk
sequential read, which is the best case for wide transfers.

### Why the caches stay valuable afterwards

pico-286 `45f322d` records that memory-mapped PSRAM **cannot** be done on this
board: the RP2350's QMI owns the dedicated QSPI pads and only chip select is a
free GPIO, so a device on ordinary GPIOs is unreachable by QMI regardless of
software. There is no XIP-from-PSRAM path to fall back on, which makes a
software cache the only option rather than a workaround — our PRG victim cache
and full CHR pre-cache are the right shape.

Independently confirmed on the forum (*Does PicoCalc have PSRAM?*,
<https://forum.clockworkpi.com/t/does-picocalc-have-psram/18001>): the PSRAM is
on ordinary GPIOs rather than the QMI bus, so it *"can only be used by copying
data into and out of RAM"*. PicoMite supports PSRAM only on QMI pins 0/8/19/47,
none of which is our GP20 — which is why that firmware cannot use it at all.

They also note an SRAM cache **compounds** with QPI rather than competing:
per-transaction overhead grows as a fraction of a faster transfer, so anything
that batches small accesses amortises more. Both caches keep their value after a
QPI port.

---

## 4. PAL region detection

**Today:** `samples/v3/picocalc_nes.cpp:285` hardcodes `FRAME_DELAY_US = 16666`
and `core/include/shapones/cpu.hpp:8` defines only `CLOCK_FREQ_NTSC`, used at
`core/src/apu.cpp:290,292,293,296`. No region detection anywhere, so a PAL ROM
is paced at 60 Hz when it expects 50 — everything runs ~20% fast, music
included, pitched up with it.

**Cheaper here than in the reference port**, because of an architectural
difference. InfoNES uses a *push* model: the APU emits a fixed number of samples
per emulated frame and blocks when the queue is full, so pacing at 50 Hz starved
the DAC and forced a matching output-rate change. We use a *pull* model — the
PWM DMA IRQ calls `apu::service(buff, len)` on demand — so the output rate is
fixed and independent of frame rate. **No sample-rate change needed; pitch stays
correct by itself.**

Work:
- header-only region detect from the first 16 bytes, trusting **only a NES 2.0
  header** (byte 7 bits 2-3 == 2, then byte 12 bits 0-1). iNES 1.0's PAL bit is
  clear in practically every dump, so reading it mislabels more often than it
  helps. `Unknown` changes no behaviour.
- `FRAME_DELAY_US` 16666 -> 19997 (50.007 Hz) for PAL
- add `CLOCK_FREQ_PAL = 1662607`, thread through the four `apu.cpp` sites

**Known limit, accept going in:** this fixes *speed*, not *timing*. The core
still runs 262 scanlines; a PAL machine has 312 and correspondingly more
vblank, so games timing raster effects to the longer frame still misbehave.
Real PAL support in the core is a separate, much larger item — leave it off
until something demands it.

**Freebie:** a region letter per row in the boot menu (`N`/`P`/`M`/`D`, and `?`
for an iNES 1.0 header) costs one 16-byte read per file, and sharing the
detector guarantees the displayed label and the pacing used cannot disagree.
`?` rather than `N` for unknown is the whole point.

---

## 5. Noise channel level

**Today:** `core/src/apu.cpp:468` returns `vol >> 1` — noise at 0.5 of a pulse,
close to the 2A03's true 0.66 — and `picocalc_nes.cpp:36` runs the APU at
`SPK_PWM_FREQ = 22050`, the same rate as the reference project, which produced
"brushy" percussion there.

The cause is **not** the mix. The noise LFSR clocks far above 22050 Hz, so it is
sampled well below its own rate and aliases into broadband hiss — correct
amplitude, wrong spectrum. What was tried there, in order: 1.00 brushy; 0.66
brushy and indistinguishable; the chip's real nonlinear `pulse_table`/`tnd_table`
mixer brushy and within 4%; **0.10 clean**. Only the level helped.

**Cheap step:** make the weight a build-time constant so it can be A/B'd, and
try ~`vol / 10`. Understand it as a fudge trading correct amplitude for
tolerable spectrum, not a fix.

**Real fix is the sample rate.** At 44100 the aliasing halves and noise can run
at its proper level. But the APU cost lands in the PWM DMA IRQ, on whichever
core services it — and Core 0 is already our bottleneck (PSRAM misses run
there). **Measure before committing**; this could cost fps on SMB3. Do not
bundle it with the cheap step. Neither project has tried it.

---

## 6. Open bus / Bubble Bobble — a documentation fix, not a code fix

`CLAUDE.md` records that reading write-only PPU registers
($2000/$2001/$2003/$2005/$2006) must return the last byte on the CPU data bus,
and that returning 0 "breaks games (e.g. Bubble Bobble) that RTI into PPU
address space and execute the byte there as an opcode".

**Bubble Bobble runs fine on InfoNES, which has no open-bus implementation** —
it returns `wAddr >> 8` at the end of `K6502_Read`. That approximation could not
possibly rescue a game genuinely executing from $2xxx: the fetched opcode at
$2000 would be `0x20` = `JSR $2020`, then again, forever, until the stack
overflows. It survives because **the game never goes there** — so whatever sent
the CPU into PPU register space happened *before* the read. The open-bus value
is a symptom, not the cause.

**Prime suspect: `addr_t = uint_fast16_t`, 32-bit on Cortex-M.** The PC does not
wrap at $FFFF by itself, which is why `CLAUDE.md` lists five sites needing an
explicit `& 0xFFFF`. InfoNES has no such exposure (`WORD PC`, `BYTE SP` wrap for
free). A single missed mask produces exactly the reported symptom: a corrupted
return address and an RTI landing somewhere absurd. Several masking fixes landed
around the same time as open bus, so open bus may simply be the change that made
the symptom disappear.

**Experiment (~10 min):** with the current CPU fixes in place, set the
write-only reads back to 0 and try Bubble Bobble.
- Works now -> open bus was never load-bearing; keep it for accuracy, fix the
  `CLAUDE.md` note, which is pointing future readers at the wrong cause.
- Still breaks -> the game reads those registers as *data* and 0 is simply the
  wrong value; `wAddr >> 8` works because it is right, and true open bus is
  righter.

Worth knowing either way: `wAddr >> 8` reproduces true open bus *exactly* for
absolute, absolute-indexed and indirect-indexed addressing, because the
address's high byte is the last thing on the bus before the read. Returning 0
matches nothing at all, which is why that was visibly broken.

---

## 7. Control-block DMA for the LCD

**Today:** the interlaced path issues ~150 single-row async DMAs per frame, each
started and *blocking-waited* by the CPU (`picocalc_nes.cpp:232-247`,
`finish_write_data()`).

This is the shape that cost the InfoNES port 60 fps — between rows the bus sits
idle while the code returns from a blocking wait, scales pixels and re-arms the
channel; a few microseconds each, ~150-232 times a frame, which is milliseconds,
and they are exactly the milliseconds that decide 60 fps.

**Less urgent for us than for them**, and the difference matters: their
arithmetic assumed a ~100%-busy bus (232 rows x 640 B = 148 KB = 15.8 ms against
16.64 ms). Interlaced, we move ~96 KB ~= 10.2 ms against 16.6 ms, so we have
~6 ms of slack and currently sit at the frame-pacing cap anyway. We *are* paying
the per-row gaps; they are eating slack we happen to have.

**Where it becomes the main event:** non-interlaced 60 fps. A full frame is
192 KB ~= 20 ms, over budget, and no scheduler fixes that. Then a control-block
DMA — one channel reprogramming another from a descriptor table, streaming a
whole field with zero CPU per row — is the answer, and it also removes the
interlace combing on fast vertical motion.

**Staging, if we do it:** chained DMA first, then tear avoidance. With one frame
buffer the next frame is rendered into the memory the current one is being sent
from, and the writer eventually overtakes the reader near the bottom of the
screen. Double buffering is the textbook answer and does not fit (+145 KB). The
free fix: the DMA channel's `read_addr` says exactly where the transfer is, so
hold the emulator off a row only while the transfer is still short of it — a few
hundred microseconds occasionally, nothing the rest of the time
(`wait_for_row_sent()`, commit `9dbbff5` over there).

Multi-day item. Schedule after PAL. Only pays off if interlace combing becomes
unacceptable.

---

## Dead end — do not repeat

The InfoNES port spent four rounds trying to *schedule around* a bandwidth
shortfall instead of removing it: skip-next-frame-on-miss (picture froze after
~1 min); tolerate lateness under 1/8 period (nothing ever skipped, 45 fps, sound
breaking up); under 1/64 (froze again); drop frames on audio queue level then at
a steered cadence (correct on average, felt sluggish — irregular judder reads
far worse than a steady lower rate).

Two lessons:

- **A dropped frame does not finish early.** When audio paces the emulator the
  throttle holds the core for the full period whether or not the frame was
  drawn, so an accumulated offset can never be worked off: **a deadline that
  carries lateness forward is late for ever.** We already get this right —
  `picocalc_nes.cpp:296` re-bases from `get_absolute_time()` each frame rather
  than accumulating. **Leave it alone.**
- If the transfer cannot fit the frame, **fix the transfer**. Every scheduler is
  a way of choosing which symptom to have.

---

## Suggested order

**flash-divisor check (2) -> PSRAM step 1 (1) -> PAL (4) -> open-bus experiment
(6) -> noise constant (5) -> QPI probe (3) -> 360 MHz at 1.30 V (1 step 2) ->
QPI driver (3) -> control-block DMA (7)**

The flash check goes first because it is the only item that questions the
configuration we ship **today**, and it is a print statement. PSRAM step 1 next:
one line, device-verified on this exact PCB at this exact clock and voltage.

Then the small independent items. The QPI check comes before either of the big
builds, because a negative result removes section 3 entirely and changes what
360 MHz is worth. It is now better than a bare Read ID probe: **build PR #15's
own benchmark as a standalone firmware** and run it on our board. That answers
in one flash whether QPI engages, whether our GP4/GP5 are sound, and what SPI
and QSPI actually achieve *here* at *our* clock — with no change to shapones at
all. 360 MHz is gated on the flash answer.

Only the QPI driver and the control-block DMA are real projects.
