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

**Measured on our own board, 2026-09-21** (standalone benchmark, 256 KB
verified in 8 KB blocks spread over the full 8 MB, 300 MHz / 1.30 V).
**Caveat: these timings include the CPU verify pass** — see the note at the end
of section 3. They understate read throughput and compress the ratio; treat the
zero-error results as sound and the KB/s as a floor, not a measurement:

| config | throughput | errors |
|---|---|---|
| SPI 50 MHz (clkdiv 3.0, today) | **4097 KB/s** | 0 |
| SPI 75 MHz (clkdiv 2.0, step 1) | **5546 KB/s** | 0 |

**+35% for a one-line change, with zero errors.** Step 1 is confirmed here, not
merely inherited.

Two corrections to this section fall out of those numbers:

- **Our baseline is 4097 KB/s, not the 2674 KB/s inherited from pico-286** at
  the same 50 MHz. Our 31-byte chunking evidently does better than whatever
  they measured. So the inherited table's *absolute* figures do not transfer to
  us; treat it as ratios only.
- **1.5x the clock gives 1.35x the throughput**, so the driver is partly
  transaction-bound here exactly as freesci-archive reported. Extrapolating,
  99 MHz SPI would land near the **low** end of the 1.25-1.9x range above.

It was also corroborated independently. pico-286 `1a8dfbd` records as a
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

## 2. Flash runs at 150 MHz today — confirmed in the build, not a device test

**Settled statically on 2026-09-21. No measurement needed.** An earlier draft
called for printing `QMI_M0_TIMING` at boot; that was unnecessary, because the
value is decided at build time and is visible in our own compiled artifacts.

The chain:

1. `pico2.h:75` sets **`PICO_FLASH_SPI_CLKDIV 2`**, and neither our CMakeLists
   nor our sources override it.
2. Our built boot stage 2 bakes that in. In
   `build/pico-sdk/src/rp2350/boot_stage2/bs2_default.dis`, `_qmi_config` loads
   the literal **`0x40000202`** and stores it to `[r3, #12]`, where `r3` is the
   QMI base (`0x400D0000`) and offset 12 is **`M0_TIMING`**. Decoding it:
   `CLKDIV` (bits 5:0) = **2**, `RXDELAY` (bits 10:8) = 2, `COOLDOWN` = 1.
3. **Nothing rewrites it afterwards.** No runtime write to `M0_TIMING` exists
   anywhere in the SDK sources, and our own code never references QMI at all.
4. `set_sys_clock_khz()` changes `clk_sys` and leaves the divider alone.

So flash = `clk_sys / 2` = **150 MHz at our 300 MHz**, against the ~133 MHz
these parts are typically rated for.

Worth seeing how we got here: at the SDK's default 150 MHz system clock,
divisor 2 gives a perfectly sane **75 MHz** flash. Our overclock to 300 MHz
doubled the flash clock as a side effect, because the divider is not part of
what `set_sys_clock_khz` touches. Nobody chose 150 MHz flash.

The board plainly works, so this is not a known defect. But "works at room
temperature on one board" is exactly how marginal XIP presents, and the
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

**This is now a decision, not an investigation.** The options:

- **Leave it.** It demonstrably works on this board at this clock. Accepts an
  out-of-spec margin that varies with temperature and with the individual part.
- **Set `PICO_FLASH_SPI_CLKDIV=4`** in our CMakeLists -> **75 MHz** flash at
  300 MHz, comfortably in spec and the same figure the stock 150 MHz build
  runs. Costs XIP fill bandwidth, so it should be measured against our fps
  rather than assumed free — we execute from XIP with the cache in front of it.

It is **blocking for section 1 step 2**: 360 MHz at divisor 2 would be 180 MHz
flash. With divisor 4 it would be 90 MHz, which is fine — so if we take the
divisor change, it clears the flash prerequisite for 360 MHz in the same stroke.

**Still worth printing the achieved rate at boot**, not to answer this question
but for the reason in section 1's build-system trap: a knob that silently fails
to apply should be visible on the device. That is a nicety, not a blocker.

---

## 3. QPI — largest win available; integration unfinished, on branch `psram-qpi`

> **Where the code is.** The integration attempt lives on the **`psram-qpi`**
> branch (<https://github.com/chneeb/shapones/tree/psram-qpi>), not on `main`.
> It carries PR #15, the read-phase fix, loader wiring, an init-time marker
> check and an SPI fallback. `main` keeps only the vendored library at upstream
> content, so the branch merges cleanly when this is finished.
>
> **Status: works standalone, does not work in the loader.** Standalone
> firmware reaches ~21.9 MB/s with all four data lines proven sound and the
> read phase corrected. The same sequence inside `psram_loader_init()` fails its
> marker read and falls back to SPI. What differs has not been identified — the
> loader runs on `pio1` with the LCD active on `pio0`, where every standalone
> test ran on `pio0` with nothing else running.
>
> **Next step is not another loader patch.** Take the exact `psram_enter_qpi()`
> sequence into the standalone harness on `pio1` and find where it diverges from
> the version that passed, with deadlines everywhere — so a wrong guess costs a
> printed line rather than an unbootable emulator. Several device flashes were
> spent guessing at this on hardware that had to stay bootable; that is the
> mistake not to repeat.
>
> Test firmware: **`samples/v3/tools/psram-test/`** on the `psram-qpi` branch —
> four standalone tools (bench, probe, realign, variants) with a README
> recording what each one answered. They live on the branch because they need
> the QPI library, which only exists there.

## 3a. Why it is worth finishing

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

### First QPI measurement here: fast, but wrong (2026-09-21)

Same standalone firmware, same 256 KB verify:

| config | throughput | errors |
|---|---|---|
| QPI 50 MHz | 10257 KB/s | **100%** |
| QPI 75 MHz | 12135 KB/s | **93.75%** |

**Throughput does not prove QPI engaged.** The PIO clocks out 4-bit frames at
4-bit speed whether or not the chip ever entered QPI mode, so ~2.5x the SPI rate
is consistent with a chip still listening in SPI and returning nonsense. Three
causes remain open: the chip never entered QPI; it did but SIO2/SIO3 (GP4/GP5)
do not work; or the quad program's sampling phase is wrong at these divisors.
Writes went through QPI too, so we do not even know the array holds the pattern.

~~One clue: at 75 MHz exactly **16384 of 262144 bytes were correct — precisely
1/16**, where chance would give 1/256. 1/16 is what two of the four data lines
returning a constant produces, which would point at GP4/GP5.~~

**Superseded — this was a red herring.** The next subsection shows the data
lines are fine and the real cause is a one-nibble read phase error. The 1/16
figure is what a nibble-shifted read of *this particular pattern* happens to
score. Left here because the reasoning looked sound and was wrong: an error
*rate* was being used to infer a mechanism, when the error *shape* — which the
probe printed and the benchmark did not — named it immediately.

### Diagnosed: QPI works, the read command is one nibble out (2026-09-21)

The probe (`picocalc-bench/probe.c`) settled it.

**QPI works and GP4/GP5 are sound.** Writing the pattern over QPI and reading it
back over SPI gave **zero errors**, with the window scrubbed to `0xA5` in SPI
first so stale data cannot explain the pass. For the array to hold the pattern,
the chip must have accepted a quad-width `0x38` command, address and payload
across **all four data lines**. SPI Read ID returns `0D 5D 53 31 76 37 43 8E` —
`0x0D` AP Memory, `0x5D` KGD pass — confirming the part.

So both hypotheses this section carried are dead, **including the 1/16 clue
above, which was a red herring**: the probe's nibble analysis reports
always-set `0x0` and always-clear `0x0`, meaning every nibble value appeared on
both halves. Nothing is stuck.

**The fault is the QPI read path, and it is a phase error, not corruption.**
Writing over SPI and reading over QPI returns the data *shifted left by exactly
one nibble*:

```
expected  5 B 4 4 6 5 0 6 2 7 C 0 E 1 8 2 A 3 4 C ...
received    B 4 4 6 5 0 6 2 7 C 0 E 1 8 2 A 3 4 C ...
```

`received[i] == expected[i+1]` across the whole window, with one junk nibble
pulled in at the tail. The bytes are right; we start sampling one cycle late.

**Cause.** `read_quad_command` is `{14, 0, 0xEB, 0,0,0, 0,0,0}` — 14 nibbles out:
`0xEB` (2) + 24-bit address (6) + **6 dummy cycles**, which is what the
APS6404L/ESP-PSRAM64H datasheet specifies for Fast Read Quad. But the
`qspi_psram` PIO program **hardcodes the extra read-sync cycle**
(`set pindirs 0 side 0b10 ; Fudge factor of extra clock cycle`) — the same cycle
the SPI side documents as required above 83 MHz and wrong below it. On the SPI
side you choose between `spi_psram` and `spi_psram_fudge`; **the quad program has
no non-fudge variant**. At 50 MHz we therefore get 6 datasheet dummies plus one,
and land a nibble late.

It explains the whole picture: writes are unaffected because the fudge is in the
read path only, which is why the cross-mode write passed; the QPI Read ID came
back all zeros because it is a read too; and 100% wrong at 50 MHz against 93.75%
at 75 MHz is two different wrong phases, not two different faults.

### Confirmed, and it is clock-dependent (2026-09-21)

**Fix (a) as first written — "`read_quad_command[0]` 14 -> 13" — is
impossible, and attempting it hangs the board.** That byte is not a dummy-cycle
knob: it is the count of nibbles the PIO pulls from the TX FIFO, and the command
buffer supplies exactly that many (9 bytes, 2 consumed by `out x,8` / `out y,8`,
leaving 7 payload bytes = 14 nibbles). Raise it and the PIO waits for nibbles
that never arrive, so the read DMA never completes. Lower it and leftover
nibbles desync the next transaction. **The dummy count cannot be changed from C
at all** — it is structural to the PIO program.

So the offset was tested the other way, by correcting it in software
(`picocalc-bench/realign.c`): read one byte early, shift back one nibble,
`byte k = (raw[k] & 0x0F) << 4 | (raw[k+1] >> 4)`.

| SCK | stock QPI read | realigned |
|---|---|---|
| 50 MHz | 16384 errors | **0 errors** |
| 75 MHz | 15360 errors | 15360 errors |

**At 50 MHz the model is exactly right.** A constant one-nibble offset, and once
corrected the data is perfect over 16 KB. The data path and all four lines are
sound; the only fault is read latency.

**At 75 MHz it is a different fault.** The raw nibbles are not shifted — they are
*blended*. Checked numerically against the pattern, the sampled stream satisfies

```
received[i] == true[i] | true[i+1]        (14 of 15 nibbles; the miss is
                                           index 0, the first after turnaround)
```

Every sampled nibble is the **bitwise OR of two consecutive true nibbles**. That
is a setup/hold violation: the sample point straddles the data transition, so any
line high in either nibble reads high. Note the alignment also moved — at 50 MHz
we catch `true[i+1]`, at 75 MHz `true[i]` contaminated by its successor — which
is what a *fixed nanosecond* data delay does as the clock period shrinks.

**So the fix is not a constant latency adjustment.** It is the sample edge, and
the right choice depends on clock rate — exactly what the fudge mechanism exists
for on the SPI side, and exactly the variant the quad program does not have. Fix
(b) stands but is bigger than "drop one turnaround cycle": the quad program needs
a rate-appropriate read phase, and the threshold on this board is somewhere at or
below 75 MHz, not the 83 MHz the comment cites.

### The fix, confirmed on hardware (2026-09-21)

Four PIO read variants were tested (`picocalc-bench/qv.pio`), crossing
turnaround length against sample edge:

**`short/fall` + 3 dummy bytes passes at 50 MHz.** Removing exactly one
turnaround clock from the quad read section — entering the read loop at
`readloop` rather than `readloop_mid`, with `y` reduced by one to keep the
sample count right — reads correctly over 4 KB. That is the realign
experiment's prediction confirmed in the PIO rather than worked around on the
CPU.

**This is enough, and 75 MHz is not needed.** QPI at 50 MHz measured
**21978 KB/s** read-only, against a single-bit ceiling of 6.25 MB/s at that
clock. So a patched quad program at our *existing* 50 MHz gives better than
**3.4x** on PSRAM reads — more than the whole clock-raising ladder in section 1,
with no clock change, no voltage change and no dependence on the flash-divisor
question in section 2. The 75 MHz OR-blending becomes optional optimisation
rather than a blocker.

**The other 14 rows of that run are not evidence.** The harness left the part in
QPI mode after the first combination, so every later one spoke SPI to a chip
listening in QPI. Two causes, both in teardown: `psram_spi_uninit()` sends its
`0xF5` exit *after* unclaiming both DMA channels, and the test additionally
disabled the state machine and removed the program before calling it, so the
exit went nowhere. The library's exit command is also mis-sized — `{8, 0, 0xF5}`
asks for 8 nibbles in QPI framing while supplying 2; correct is `{2, 0, 0xF5}`.

The tell was an internal contradiction: stock at 75 MHz returned wrong *data* in
two earlier firmwares but TIMEOUT here. When a harness disagrees with a simpler
earlier measurement, the harness is wrong. The surviving PASS is still sound —
config 0 wrote its pattern while the chip was still in SPI at boot, that data
survived, and `short/fall` read it back correctly.

`picocalc-bench/variants2.c` corrects it: no library quad path at all, a
correctly sized exit sent through the *running* state machine before any
teardown, and a Read ID check between every combination that reports
`chip-dirty` rather than letting a contaminated row look like a result.

### Re-run on the corrected harness (2026-09-21)

24 combinations, no `TIMEOUT` and no `chip-dirty` anywhere — the contamination
is gone. Result:

- **`short/fall` + 3 dummy bytes + 50 MHz: PASS**, this time against a pattern
  written immediately beforehand with the part verified in SPI mode. Genuine
  end-to-end, not a lucky read of an earlier config's data. **This is the fix.**
- **Every other combination fails**, including all twelve at 75 MHz.

**75 MHz is not a program-structure problem and should not be pursued.** Four
read variants x three dummy counts all fail there. Together with the OR-blending
signature, that points at analog margin rather than cycle counts: four lines
switching together produce more ground bounce than single-bit SPI at the same
clock, and SIO2/SIO3 are repurposed nunchuck pins that were never laid out as a
matched data bus. The untried knobs are drive strength, slew rate and input
hysteresis, all currently hardcoded in the library — but there is no reason to
spend them: **QPI at 50 MHz (~21.9 MB/s) beats SPI at 75 MHz (~5.5 MB/s) by
about 4x**, so the protocol is worth far more than the clock, and 50 MHz is
where we already are.

*Harness note for anyone re-running it:* with 24 configs the result packing
collides with the watchdog resume slot — `scratch[1 + i/10]` reaches
`scratch[3]` at i >= 20, which is where the in-progress index lives. Only the
reprinted summary is affected; the live lines are correct. Trust the live
output.

### Caveat on every throughput figure above (2026-09-21)

**The benchmark timed the verify loop along with the read.** `main.c` put the
per-byte comparison inside the timed region — deliberately, to stop the compiler
eliding the reads, which was the wrong trade. So the 4097 / 5546 KB/s figures for
SPI are read **plus** CPU verification, not read throughput, and the constant CPU
cost also compresses the 50->75 ratio. **The conclusion drawn from it — that the
driver is transaction-bound and 99 MHz would land at the low end of the range —
is probably an artifact and should not be relied on.**

The realign firmware times reads only, and measured **21978 KB/s for QPI at
50 MHz** — against a theoretical quad ceiling of 25 MB/s at that clock, so ~86%
of line rate. QPI is genuinely running at full quad speed. Single-bit SPI cannot
exceed 6.25 MB/s at 50 MHz, so once the read phase is fixed the gain over our
current path is **well above** the 1.6-2.1x recorded earlier.

A clean read-only A/B of SPI against QPI is still owed before any number here is
quoted as final.

**Next:** `picocalc-bench/sweep.c` sweeps `read_quad_command[0]` over 12-16
against clkdiv 3.0 / 2.0 / 1.5, verifying each combination with the pattern
always written over SPI. Failing rows report the nibble offset that *would* have
matched, so even a table of failures names the right value. If 13 passes below
83 MHz and 14 above it, that confirms the hardcoded read-sync cycle and settles
where the threshold sits on this board — which is what fix (b) needs.

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

## 4. PAL region detection — DONE (2026-09-21)

**Implemented.** `core/include/shapones/region.hpp` is header-only and takes the
16-byte header:

- `detect_region()` trusts **only** a NES 2.0 header (byte 7 bits 3-2 == `0b10`,
  then byte 12 bits 1-0). iNES 1.0 returns `UNKNOWN` and changes nothing.
- `region_cpu_clock_hz()` — `CLOCK_FREQ_PAL = 1662607`, `CLOCK_FREQ_DENDY =
  1773448`, added alongside `CLOCK_FREQ_NTSC` in `cpu.hpp`.
- `region_frame_period_us()` — 16666 for NTSC/MULTI/UNKNOWN, 19997 for
  PAL/Dendy.
- `region_letter()` — `N`/`P`/`M`/`D`, `?` for iNES 1.0.

Wiring: `map_ines()` detects the region and calls `apu::set_region()`, which
recomputes the four timer steps from the region's CPU clock; `ppu_loop()` takes
its frame period from `memory::current_region`; the boot menu shows a letter per
row from one 16-byte read per file, using the same detector so the label and the
pacing cannot disagree.

**The output sample rate is untouched**, as predicted: this port pulls samples
on demand from the audio IRQ, so slowing the frame rate cannot starve the DAC
and pitch stays correct by itself. That was the painful part in the reference
project and was free here.

Verified with a host unit test over eight synthetic headers, including the two
that matter: an iNES 1.0 header **with** the byte 9 PAL bit set still reports
`?`, and mapper bits in the high nibble of byte 7 do not break NES 2.0
detection.

**Known limit, accepted:** this fixes *speed*, not *timing*. The core still runs
262 scanlines where a PAL machine has 312, so games timing raster effects to the
longer frame still misbehave. Real PAL support in the core remains a separate,
much larger item.

<details><summary>Original plan</summary>

## 4a. PAL region detection — as originally scoped

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

</details>

## 4b. SD bus clock — DONE (2026-09-21)

Not a roadmap item until it surfaced: adding the per-file region read to the
boot menu made the ROM list visibly slow, which exposed that the SD bus had been
running at **250 kHz** all along.

`mmc_pico_spi.c` follows the standard FatFs pattern — identify the card at a
slow clock, then `FCLK_FAST()` once it answers — but both macros were defined as
`{ }`. So the "set fast clock" line at the end of `disk_initialize()` did
nothing and the card stayed at its 250 kHz init clock forever: ~31 kB/s, which
is why a 384 kB ROM took on the order of ten seconds to load.

Now 400 kHz for identification (the SD spec's limit for that phase) and
**25 MHz** after, the ceiling for default-speed SPI mode. 30 MHz has been run on
PicoCalc hardware in another project, so this is the spec limit rather than a
stretch. `SD_FCLK_HZ` is overridable from CMake because
`samples/pico_ws19804` compiles the same driver on hardware neither of us can
test.

Both rates divide `clk_peri` exactly at 300 MHz. Expect ~100x on SD reads: ROM
loads under a second, and the region scan imperceptible.

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

## 6. Bubble Bobble and Tetris — BOTH FIXED (2026-09-22)

Two real bugs, found in one sitting with `samples/hosttest` and **zero device
flashes**. Five flashes had previously been spent on this without finding
either.

### Tetris: one-screen mirroring used a two-screen mask

`SINGLE_LOWER`/`SINGLE_UPPER` used the `HORIZONTAL` address mask
(`(VRAM_SIZE-1)-(VRAM_SIZE/2)` = `0x7FF`), which leaves bit 10 live and so still
addresses two nametables. One-screen mirroring has to collapse all four
nametable selects onto one 1 kB screen: the mask is `0x3FF`.

Tetris set one-screen-lower, wrote its playfield to nametable 0, and rendered
with nametable select 3, so the fetch landed in a nametable it had never
written. Every background tile read `0xFF` and drew as colour 0 — a black
playfield with only the sprites (falling piece, preview) visible.

### Bubble Bobble: every unrecognised write went to the mapper

`bus_write` ended in a bare `else`, so any address not matched earlier —
`$4017`, `$4020-$7FFF`, anything — was handed to the mapper. On MMC1 that is
destructive rather than merely wrong: its four registers share one shift
register written as a five-bit serial stream, so a stray write shifts a junk
bit in and the game's next real sequence latches early with its bits off by one.

Bubble Bobble writes `$4022/$4023/$4025/$4026/$4017/$4080/$408A` during init, so
PRG bank 6 latched as 13, masked to bank 5, which is filler in this ROM. Its
`JSR $8000` called unused space, the CPU ran away into PPU register space, and
the stack collapsed — the entire "`RTI` to `$334a`" signature, and the torn logo
too, since the wrong bank was mapped from the first frame.

Mappers 0-4 respond only to `$8000-$FFFF`, so that is the guard.

### The open-bus question, settled separately

Open bus and returning 0 are **indistinguishable** — compared on device with a
runtime toggle, and Bubble Bobble failed identically either way because its real
fault was the bus decode. The long-standing `CLAUDE.md` claim that returning 0
breaks that game was never true. Open bus is kept because it is what hardware
does.

### Why the harness mattered

Each step ruled something out, none needing hardware: it reproduced the device
symptom **single-threaded** (so not a race); the stack trace showed `SP` jumping
with no intervening push (so a `TXS`, not a runaway); logging `TXS` showed the
handler's stack restore never running on the second NMI; the PC trace showed
execution entering `$3xxx` from a `JSR $8000`; the ROM showed filler at the
mapped bank and real code one bank over; and the MMC1 write trace showed junk
addresses shifting the register.

Regression-checked against Castlevania, Dr. Mario, DuckTales, Mega Man, Metroid,
Pac-Man, SMB, SMB3, Tetris and Zelda.

**Left open:** `$4017` (APU frame counter) is now ignored rather than
misdelivered to the mapper. It never reached the APU before either, so this is
not a regression, but it is a gap.

## 6b. Earlier framing, kept because the reasoning was wrong in an instructive way

**The open-bus question is settled: it makes no difference.** Compared on device
with a runtime toggle, open bus and returning 0 are indistinguishable — including
for Bubble Bobble, which fails identically either way. The long-standing
`CLAUDE.md` claim that returning 0 breaks that game was never true. Open bus is
kept because it is what hardware does, and the note now says so.

**Bubble Bobble is broken for some other reason, and is parked.** What it does:
the stack falls from `0xfc` to `0x24` (~216 bytes), then an `RTI` returns to
`0x334a` — `$2002` through the mirror — and the CPU executes PPU registers as
opcodes. It is **non-deterministic**: identical firmware, different outcome each
boot, and once fully playable.

Eliminated on hardware, each at the cost of a flash cycle:

| hypothesis | how it was ruled out |
|---|---|
| PAL / region | `roms/ines_region.py --strip` gives the same ROM as iNES 1.0; fails identically. PAL SMB and PAL Zelda (same region *and* mapper) both work. |
| open bus | runtime A/B, indistinguishable |
| MMC1 | PAL Zelda is MMC1 and works |
| `reg_read` data race | real defect, fixed, no change to the symptom |
| deferred PPU writes | synchronous-write mode changed nothing |

**One real fix came out of it**: `ppu::reg_read()` took `SEMAPHORE_PPU` only to
flush its write queue, leaving `reg.status.raw &= 0x7F` as an unsynchronised
read-modify-write against core 1, which writes the same byte under that
semaphore. A lock only one side takes is not a lock. That is fixed and stays.

**If this is picked up again, do not start with another device test.** `core/` is
platform-independent, so the next step is a headless host harness — stub
`host_intf`, load the ROM, run `cpu::service()`/`ppu::service()` single-threaded,
watch for the stack collapse. That also splits the question for free: reproducing
on a single-threaded host proves it is *not* concurrency and can then be chased
with a debugger, at no cost per attempt. Five device cycles went on hypotheses
that a host harness would have tested in minutes.

`-DSHAPONES_TRACE_STACK=1` dumps the last 48 pushes (PC and SP) when the stack
collapses; off by default.

## 6a. Original framing

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

**Done:** PSRAM step 1 (§1), PAL region detection (§4), SD bus clock (§4b),
one-screen mirroring and the mapper write decode (§6), the `reg_read` data race,
and the offline harness (`samples/hosttest`).

**What is left, smallest first:**

1. **Noise channel level (§5)** — a build-time weight and a listening test. No
   dependencies.
2. **Flash-divisor decision (§2)** — deferred, not resolved. Flash runs at
   150 MHz against a ~133 MHz rating. Gates item 4.
3. **Finish QPI (§3)** on the `psram-qpi` branch — the largest single win left.
4. **360 MHz at 1.30 V (§1 step 2)** — blocked on the flash divisor.
5. **Control-block DMA (§7)** — multi-day, only if interlace combing becomes
   unacceptable.

**Use `samples/hosttest` first for anything that is not PSRAM or LCD timing.**
It found two bugs in a sitting that five device flashes had missed. Reproducing
there also splits concurrency from emulation for free, since it is
single-threaded.

**Loose end:** the `reg_read` semaphore's cost was never measured, because the
SMB3 dump on hand is PAL and sits pinned at its 50 Hz cap with headroom.
`roms/ines_region.py --strip` gives the same ROM at 60 Hz for a comparable
number against the old ~55-60 figure.

**Bubble Bobble (§6) is parked**, not scheduled. If it is picked up, start with
a headless host harness, not the device.
