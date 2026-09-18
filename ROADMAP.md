# ShapoNES / Picocalc roadmap

Ordered by value-per-effort. Each item records what is actually in the tree
today, what to change, and the evidence behind it. Two sibling projects on
comparable hardware supplied most of that evidence:

- `~/Source/rp2040-ili9341-infones` — InfoNES on RP2350 + ST7789 (pico-sdk).
  Findings transcribed in `NOTES-from-infones.md`.
- `~/Source/pico-286` — 286 emulator, **same Clockworkpi Picocalc PCB**, and
  crucially the *same* `rp2040-psram` library (its `psram_spi.pio` is
  byte-identical to our submodule copy apart from the licence header).

---

## 1. PSRAM SPI clock: we are leaving ~1.9x on the table

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
The current "do not change the divisor" warning is more forbidding than the
evidence supports, and should be rewritten when step 1 lands.

**The coupling is the actual trap:** SM below ~166 MHz needs `fudge=false`,
above needs `fudge=true`. Our call passes `false`. Raising the clock past
83 MHz SPI without also flipping that argument gives a **dead bus**.

### Step 1 — 1.5x, no overclock (do first)

Stay at 300 MHz; change clkdiv 3.0 -> **2.0** => 150 MHz SM = **75 MHz SPI**,
keep `fudge=false`. Exact integer divisor (no fractional jitter), comfortably
under the 83 MHz fudge threshold, and bracketed by the verified 66 and 79 MHz
plain passes. One-line change plus the `static_assert` at
`psram_loader.cpp:159`.

### Step 2 — the full 1.9x

100 MHz SPI requires `fudge=true`. At 300 MHz that is clkdiv **1.5** =>
200 MHz SM, which works but is a *fractional* divider: PIO alternates cycle
lengths, putting duty jitter on SCK. pico-286 chose 396 MHz specifically to get
an exact divide-by-2 and avoid this. So this is the one point genuinely worth
soak-testing on our own firmware rather than inheriting.

### Step 3 — sys-clock overclock (keep separate)

396/400 MHz buys an exact ÷2 *and* ~32% more emulator CPU. Costs: pico-286 runs
**VREG 1.60 V**; we are at 1.30 V (`picocalc_nes.cpp:114`). It also drags the
LCD along, since `boot_menu.cpp:216,232` set `set_spi_speed(SYS_CLK_FREQ / 4)`
— LCD SPI would go 75 -> 99 MHz, an untested panel overclock riding on an
unrelated change. The LCD is PIO-based (`picocalc.cpp:261`, `div = sys/2/speed`),
so **pin the LCD to 75 MHz explicitly before any sys-clock bump** so the two
variables cannot move together.

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

## 2. PAL region detection

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

## 3. Noise channel level

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

## 4. Open bus / Bubble Bobble — a documentation fix, not a code fix

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

## 5. Control-block DMA for the LCD

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

**PSRAM step 1 (1) -> PAL (2) -> open-bus experiment (4) -> noise constant (3)
-> PSRAM steps 2/3 (1) -> control-block DMA (5)**

The first four are small and independent. PSRAM step 1 goes first because it is
one line for a verified 1.5x on our worst-case path. Only the last is a real
project.
