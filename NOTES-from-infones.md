# Things worth stealing from rp2040-ili9341-infones

Notes written after a long debugging session on the sibling project
`~/Source/rp2040-ili9341-infones` (InfoNES on an RP2350 + ST7789, pico-sdk).
Several problems solved there apply here, and one whole dead end is worth not
repeating. Everything below was checked against this repo's code, not assumed.

Reference project: `~/Source/rp2040-ili9341-infones`, target `PICO_RESTOUCH`
(`software/infones/`). Commit hashes below are in that repo.

**Not applicable here** — that project's SD card and LCD share one SPI bus,
which caused a whole class of bugs (stale RX FIFO bytes breaking FatFs, see
`drivers/sdcard/sdcard.c`, commit `269752a`). The Picocalc has the LCD on
GP10–15/PIO0 and the SD on spi0 GP16–19, so none of that applies.

---

## 1. PAL ROMs run ~20% fast (highest value, smallest change)

**Here:** `samples/v3/picocalc_nes.cpp:285` hardcodes `FRAME_DELAY_US = 16666`,
and `core/include/shapones/cpu.hpp:8` defines only `CLOCK_FREQ_NTSC`, used for
the APU timer steps at `core/src/apu.cpp:290,292,293,296`. There is no region
detection anywhere, so a PAL ROM is paced at 60 Hz when it expects 50 —
everything runs ~20% fast, music included, pitched up with it.

**Reference:** `software/infones/NesRegion.h` — header-only, no dependencies,
no Circle or InfoNES types. Hand it the file's first 16 bytes, get a region
back. Used by both ports over there (the emulator side and the ROM menu), so
the letter shown in a file list and the timing actually used can never
disagree.

Detection deliberately trusts **only a NES 2.0 header** (byte 7 bits 2–3 == 2,
then byte 12 bits 0–1). iNES 1.0 has a PAL bit at byte 9 bit 0, but practically
every dump leaves it clear whatever the game is, so reading it mislabels PAL
ROMs as NTSC far more often than it helps. `Unknown` is the honest answer and
the safe one: nothing changes behaviour on it.

**What to change here** — and this is *smaller* than it was over there:

- frame period 16666 → 19997 µs (50.007 Hz) for PAL
- a `CLOCK_FREQ_PAL = 1662607` alongside `CLOCK_FREQ_NTSC`, threaded through
  the four `apu.cpp` sites, so the APU timers are right for the region

**You do not need to touch the output sample rate.** That is the part that was
painful in the reference project and is free here, because of an architectural
difference worth understanding:

- *There*: push model. The APU emits a fixed 367.5 samples per **emulated
  frame** and blocks the emulator when the queue is full, so pacing at 50 Hz
  produced five sixths as many samples per second and starved the DAC.
  The fix was to also open the DAC at 5/6 the rate (22050 → 18350), which
  corrects the pitch in the same stroke. See `applyRegionTiming()` in
  `software/infones/main.cpp:1059`, commits `f513bc9` and `5505b97`.
- *Here*: pull model. The PWM DMA IRQ calls `apu::service(buff, len)` on
  demand, so the output rate is fixed and independent of the frame rate.
  Slowing the frames cannot starve it, and the pitch stays correct by itself.

**Still wrong afterwards, in both projects:** a PAL machine has 312 scanlines
and correspondingly more vblank; the core still runs 262. Games that time
raster effects to the longer frame can misbehave. Fixing that means real PAL
support in the core.

While you are there: a region letter per row in the ROM menu (`N`/`P`/`M`/`D`,
and `?` for an iNES 1.0 header) costs one 16-byte read per file. `?` rather
than `N` for unknown is the entire point — most dumps are iNES 1.0, and
calling those NTSC puts a confident label on exactly the case worth seeing.
Commit `35168c7`.

---

## 2. The noise channel is ~5x louder than what sounds right at 22050 Hz

**Here:** `core/src/apu.cpp:468` returns `vol >> 1`, so noise sits at 0.5 of a
pulse — close to the 2A03's true 0.66 — and `samples/v3/picocalc_nes.cpp:36`
runs the APU at **22050 Hz**, the same rate as the reference project.

That combination is what produced "brushy" percussion over there: drums, hats
and explosions riding over the music. The cause is **not** the mix. The noise
channel's LFSR clocks far above 22050 Hz, so it is sampled well below its own
rate and aliases into broadband hiss — **correct amplitude, wrong spectrum.**

Evidence from the reference project, in the order it was tried:

| noise weight vs one pulse | result |
|---|---|
| 1.00 (the original port) | brushy |
| 0.66 (the chip's linear small-signal ratio) | brushy, indistinguishable |
| the chip's real **nonlinear** mixer (`pulse_table`/`tnd_table`) | brushy, within 4% of the above |
| **0.10** | clean |

The nonlinear mixer is the correct structure and is worth having anyway —
`ApuMixTables` in `software/infones/main.cpp`, commit `9309648` — but measured,
it changes what noise adds over a triangle by 4% (4266 vs 4457 of 32767), so
it fixes nothing on its own. Only the level did. Commit `6538f7c`.

**What to try here:** if percussion sounds harsh, change `vol >> 1` to
something near `vol / 10` and listen. Make it a build-time constant so it can
be A/B'd without hunting through the expression.

**The real fix is the sample rate.** At 44100 the aliasing halves and noise
could run at its proper level with percussion that actually sounds like
percussion. Over there that means `pAPU_QUALITY 3`, at double the APU cost;
here it is `SPK_PWM_FREQ`, and the cost lands on whichever core services the
PWM DMA IRQ. Neither project has tried it yet.

---

## 3. Open bus is probably not why Bubble Bobble broke

`core/CLAUDE.md` records that reading the write-only PPU registers
($2000/$2001/$2003/$2005/$2006) must return the last byte on the CPU data bus,
and that returning 0 "breaks games (e.g. Bubble Bobble) that RTI into PPU
address space and execute the byte there as an opcode".

**Bubble Bobble runs fine on InfoNES, which does not implement open bus at
all.** It returns `wAddr >> 8` — the old upper-address-byte approximation — at
the end of `K6502_Read` in `K6502_rw.h`. That is worth a second look, because
the approximation could not possibly save a game that really executed from
$2xxx: at $2000 the fetched opcode would be `0x20` = JSR absolute with operands
`0x20 0x20`, so `JSR $2020`, then `0x20` again, then `JSR $2020` forever until
the stack overflows. It survives only because **the game never goes there.**

So whatever sends the CPU into PPU register space happens *before* the read.
The open-bus value is a symptom, not the cause.

**Prime suspect: `addr_t = uint_fast16_t`, which is 32-bit on Cortex-M.** The
PC does not wrap at $FFFF by itself, which is why `core/CLAUDE.md` lists five
places needing an explicit `& 0xFFFF` (`fetch()`, `fetch_w()`, `opRTS()`,
`bus_read_w()`, `fetch_rel()`) plus a note about SP wrapping at $FF. InfoNES
has none of that exposure because it uses exact-width types — `WORD PC`
(unsigned short) and `BYTE SP` wrap for free, and `PUSH` is
`K6502_Write(BASE_STACK + SP--, a)` with a byte SP. A single missed mask
produces exactly the reported symptom: a corrupted return address and an RTI
landing somewhere absurd.

If several accuracy fixes landed around the same time, open bus may simply be
the one that made the symptom disappear.

**Why the crude approximation holds up anyway**, which is itself worth knowing:
for `LDA $2000` the CPU fetches the opcode, then the low operand byte `$00`,
then the high byte `$20` — so the last value on the data bus before the read
*is* the address's high byte. `wAddr >> 8` therefore reproduces true open bus
exactly for absolute addressing, and for absolute-indexed and indirect-indexed
too (the pointer's high byte is the last thing fetched). It is only wrong for
addressing modes that cannot reach $2xxx in the first place. Returning 0
matches nothing at all, which is why that was visibly broken.

**Cheap experiment:** with the current CPU fixes in place, set the write-only
reads back to 0 and try Bubble Bobble again.

- Works now → open bus was never load-bearing, and the CLAUDE.md note is
  pointing future readers at the wrong cause. Keep open bus for accuracy, fix
  the note.
- Still breaks → the game reads those registers as *data* and 0 is simply the
  wrong value. Then `wAddr >> 8` works because it is right, and real open bus
  is righter.

Either way the true open-bus implementation is the better code; the question is
only what the note should say about why.

---

## 4. Per-row DMA leaves the bus idle, and it costs milliseconds

**Here:** the interlaced path issues ~150 single-row async DMAs per frame
(`CLAUDE.md`, "Interlaced update"), each started and completed by the CPU.

This is the exact shape that cost the reference project 60 fps, and it took
**four attempts at a frame scheduler** before the penny dropped, so the
arithmetic is worth writing down:

```
232 rows x 640 bytes = 148,480 bytes
at the 75 MHz the SPI actually runs   = 15.8 ms of pure transfer
against a 16.64 ms NTSC frame         = the bus must be busy ~100% of the time
```

It was not busy. Between rows it sat idle while the code returned from a
blocking wait, scaled the pixels and re-armed the channel — a few microseconds
each, 232 times a frame, which is milliseconds, and they are exactly the
milliseconds that decide 60 fps. **No scheduler can recover them.**

(Note the 75: the code requested 80 MHz and got 300/4. The SPI prescaler is
even, so only `sys_clk / 2n` is reachable. Worth checking what your PIO clkdiv
actually resolves to as well.)

**The fix there** was one uninterrupted DMA per frame from a full frame buffer:
`InfoNES_PreDrawLine()` points the emulator at row *line* of the buffer so the
frame accumulates with no copying, and `present_frame()` hands the whole thing
over at the start of vblank. Commit `0ee8220`.

**The equivalent here**, since an interlaced field's rows are not contiguous,
is a **control-block DMA**: one channel reprogramming another from a table of
descriptors, streaming the whole field with zero CPU per row. 96 KB at 75 MHz
is ~10.2 ms, entirely overlapping the next frame's emulation, and it decouples
the transfer from the conversion — which is what makes timing robust rather
than tuned.

### If you do that, you inherit tearing — and there is a free fix

With one frame buffer, the next frame is rendered into the memory the current
one is being sent from. Over there a row takes 68 µs to send and ~64 µs to
emulate, so the writer gains ~5 µs a row — 1.1 ms over 232 rows, against the
~1.4 ms head start vblank gives the transfer. That margin is thinner than the
jitter, so occasionally the writer overtakes the reader near the bottom of the
screen and part of the new frame appears inside the old one.

Double buffering is the textbook answer and did not fit (145 KB more, 21 KB
over the RAM budget). Instead: **the DMA channel's `read_addr` says exactly
where the transfer is**, so hold the emulator off a row only while the
transfer is still short of it. Costs a few hundred microseconds once in a
while and nothing at all the rest of the time. `wait_for_row_sent()` in
`software/infones/main.cpp:257`, commit `9dbbff5`.

---

## 5. The dead end — four schedulers, don't repeat them

The reference project spent four rounds trying to schedule around the
bandwidth shortfall instead of removing it. Each fix produced a new symptom:

| attempt | result |
|---|---|
| skip the next frame if this one missed its deadline | picture froze entirely after ~1 minute; sound perfect |
| tolerate lateness under 1/8 of a period | nothing ever skipped: 45 fps, sound slow and breaking up |
| tolerate under 1/64 | froze again |
| drop frames on the audio queue level, then at a steered cadence | correct on average, felt sluggish — irregular judder reads far worse than a steady lower rate |

Two lessons survive:

- **A dropped frame does not finish early.** When audio paces the emulator, the
  throttle holds the core for the full period whether or not the frame was
  drawn; dropping only parks it somewhere else. So an accumulated timing offset
  can never be worked off, and **a deadline that carries lateness forward is
  late for ever**. This repo already gets that right —
  `samples/v3/picocalc_nes.cpp:286` re-bases from `get_absolute_time()` each
  frame rather than accumulating. Leave it alone.
- If the transfer cannot fit the frame, **fix the transfer**. Every scheduler
  is a way of choosing which symptom to have.
