# PicoCalc PSRAM diagnostics

Standalone firmwares, one per question. Not part of the emulator build.

They exist because diagnosing the PSRAM from inside `picocalc_nes` means a
wrong guess leaves the board unbootable with nothing on screen — which happened
twice. Every tool here bounds its transfers and recovers the state machine on
expiry, so a bad configuration prints a line instead of freezing.

## Build

```bash
mkdir -p build && cd build
PICO_SDK_PATH=~/Source/pico-sdk cmake .. -DPICO_BOARD=pico2
cmake --build . -j$(nproc)
```

Flash one `.uf2` at a time; results come back over USB CDC at any baud. Each
prints a distinct banner as its first line, so there is no doubt which one is
running — mixing them up cost two flash cycles.

## The tools, in the order they were used

| firmware | question | answer it gave |
|---|---|---|
| `psram-bench` | how fast is SPI, and does QPI help? | SPI 50→75 MHz is +35%, zero errors. QPI ran ~2.5x faster and returned wrong data. |
| `psram-probe` | is QPI engaged, and are GP4/GP5 sound? | Yes to both. A QPI write read back over SPI is byte-perfect, so all four data lines work. The fault is the QPI **read** path. |
| `psram-realign` | is the read offset a constant nibble? | At 50 MHz yes — correcting it in software gives zero errors. At 75 MHz no: each nibble is the bitwise OR of two consecutive true nibbles, i.e. analog margin. |
| `psram-variants` | which PIO read phase is right? | `short/fall` with 3 dummy bytes passes at 50 MHz. Nothing passes at 75 MHz. |

`qv.pio` holds the four read-phase variants `psram-variants` walks
(long/short turnaround × falling/rising sample edge).

## Two tools deliberately not kept

- **`sweep.c`** swept `read_quad_command[0]`, which is not a dummy-cycle knob:
  it is the count of nibbles the PIO pulls from the TX FIFO, and the command
  buffer supplies exactly that many. Raising it leaves the PIO waiting for
  nibbles that never arrive, so the read DMA never completes — it hung the
  board with no output. Kept out rather than kept as a trap.
- **`variants.c`** (the first variant matrix) left the part in QPI between
  combinations, so 14 of 16 rows were meaningless. `variants2.c` supersedes it
  and explains the bug in its header.

## Reading results

Throughput alone proves nothing about QPI: the PIO clocks 4-bit frames at 4-bit
speed whether or not the chip ever entered QPI mode. Always read throughput and
error count together. `ROADMAP.md` section 3 has the full evidence trail.
