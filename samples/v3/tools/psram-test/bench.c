// PicoCalc PSRAM: SPI vs QPI benchmark and integrity check.
//
// Standalone probe firmware for shapones ROADMAP.md section 3. Answers three
// questions in one flash, without changing anything in shapones:
//
//   1. Does QPI engage at all on this board? (GP4/GP5 have never been driven
//      here; the schematic says they are routed, which is not the same thing.)
//   2. Are those lines electrically sound? (Throughput alone cannot tell
//      "QPI never engaged" from "engaged but a data line is broken" - only
//      throughput AND an error count together can.)
//   3. What do SPI and QPI actually achieve *here*, at *our* clock?
//
// Clock and voltage deliberately match samples/v3 (300 MHz, 1.30 V) so the
// numbers are comparable with shapones rather than with the PR's own runs.
// Code runs from SRAM (PICO_COPY_TO_RAM), so flash timing is not a variable.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "psram_spi.h"

#define BENCH_SYS_CLK_HZ      (300 * 1000 * 1000)

#define PSRAM_SIZE      (8u * 1024 * 1024)
#define BLOCK           8192u           // an 8 KB PRG bank, our real access size
#define N_BLOCKS        32u             // 256 KB total, spread over the whole part
#define BLOCK_STRIDE    (PSRAM_SIZE / N_BLOCKS)

static uint8_t buf[BLOCK];

// Byte pattern derived from the address. Spans all 256 values so a stuck or
// shorted data line shows up, and is address-dependent so a wrong-address read
// does too.
static inline uint8_t pat(uint32_t addr) {
    return (uint8_t)((addr * 31u) ^ (addr >> 8) ^ 0x5Au);
}

// The library's psram_read/psram_write pack the transfer length into a uint8_t
// as bits (SPI) or nibbles (QPI), so the per-call cap differs by mode.
#define RD_CHUNK(quad) ((quad) ? 127u : 31u)
#define WR_CHUNK(quad) ((quad) ? 119u : 27u)

static void block_write(psram_spi_inst_t *s, uint32_t addr, const uint8_t *src, size_t n, bool quad) {
    size_t cap = WR_CHUNK(quad);
    while (n) {
        size_t k = n > cap ? cap : n;
        psram_write(s, addr, src, k);
        addr += k; src += k; n -= k;
    }
}

static void block_read(psram_spi_inst_t *s, uint32_t addr, uint8_t *dst, size_t n, bool quad) {
    size_t cap = RD_CHUNK(quad);
    while (n) {
        size_t k = n > cap ? cap : n;
        psram_read(s, addr, dst, k);
        addr += k; dst += k; n -= k;
    }
}

// Mirrors the PR's psram_qpi_init(), but at a chosen clkdiv instead of a
// hardcoded 1.0 (which would be 150 MHz SCK at our system clock).
static psram_spi_inst_t qpi_init_clkdiv(PIO pio, float clkdiv, bool fudge) {
    psram_spi_inst_t s = psram_spi_init_clkdiv(pio, -1, clkdiv, fudge, false);
    uint8_t enter_qpi[] = { 8, 0, 0x35u };
    pio_spi_write_read_dma_blocking(&s, enter_qpi, 3, 0, 0);
    psram_spi_uninit(s);            // s.quad == false, so no 0xF5 is sent
    return psram_spi_init_clkdiv(pio, -1, clkdiv, fudge, true);
}

// Report the rate the PIO divider is actually programmed to, not the one we
// asked for - a knob that silently fails to apply should be visible.
static float actual_sck_mhz(psram_spi_inst_t *s) {
    uint32_t reg = s->pio->sm[s->sm].clkdiv;
    float div = (float)(reg >> 16) + (float)((reg >> 8) & 0xFF) / 256.0f;
    if (div < 0.001f) div = 1.0f;
    return (float)clock_get_hz(clk_sys) / div / 2.0f / 1e6f;
}

static void run(const char *label, bool quad, float clkdiv, bool fudge) {
    psram_spi_inst_t s = quad ? qpi_init_clkdiv(pio0, clkdiv, fudge)
                              : psram_spi_init_clkdiv(pio0, -1, clkdiv, fudge, false);

    printf("\n--- %s : clkdiv %.2f, fudge %d ---\n", label, (double)clkdiv, (int)fudge);
    printf("    SCK actually programmed: %.2f MHz\n", (double)actual_sck_mhz(&s));

    // Write the pattern.
    for (uint32_t b = 0; b < N_BLOCKS; b++) {
        uint32_t addr = b * BLOCK_STRIDE;
        for (uint32_t i = 0; i < BLOCK; i++) buf[i] = pat(addr + i);
        block_write(&s, addr, buf, BLOCK, quad);
    }

    // Read back, verify, and time it. Timing the verifying pass keeps the
    // compiler from eliding the reads.
    uint32_t errors = 0, first_bad = 0xFFFFFFFFu;
    uint32_t t0 = time_us_32();
    for (uint32_t b = 0; b < N_BLOCKS; b++) {
        uint32_t addr = b * BLOCK_STRIDE;
        block_read(&s, addr, buf, BLOCK, quad);
        for (uint32_t i = 0; i < BLOCK; i++) {
            if (buf[i] != pat(addr + i)) {
                if (errors == 0) first_bad = addr + i;
                errors++;
            }
        }
    }
    uint32_t us = time_us_32() - t0;

    uint32_t total = N_BLOCKS * BLOCK;
    printf("    read %u KB in %u us  =  %u KB/s\n",
           total / 1024u, us, (uint32_t)((uint64_t)total * 1000000u / us / 1024u));
    if (errors == 0) {
        printf("    errors: 0  -- PASS\n");
    } else {
        printf("    errors: %u of %u bytes (%.2f%%), first at 0x%06X  -- FAIL\n",
               errors, total, 100.0 * errors / total, first_bad);
    }

    psram_spi_uninit(s);
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(100);
    set_sys_clock_khz(BENCH_SYS_CLK_HZ / 1000, true);

    stdio_init_all();

    // Wait for the USB host, but do not hang forever if only UART is attached.
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(500);

    printf("\n\n=== PicoCalc PSRAM SPI vs QPI benchmark ===\n");
    printf("sys_clk = %u Hz, vreg = 1.30 V\n", (unsigned)clock_get_hz(clk_sys));
    printf("pins: CS=%d SCK=%d SIO0=%d SIO1=%d SIO2=%d SIO3=%d\n",
           PSRAM_PIN_CS, PSRAM_PIN_SCK, PSRAM_PIN_MOSI,
           PSRAM_PIN_MOSI + 1, PSRAM_PIN_MOSI + 2, PSRAM_PIN_MOSI + 3);
    printf("%u KB verified per config, in %u KB blocks spread over 8 MB\n",
           N_BLOCKS * BLOCK / 1024u, BLOCK / 1024u);

    // clkdiv 3.0 = 50 MHz SCK: what shapones runs today.
    // clkdiv 2.0 = 75 MHz SCK: ROADMAP section 1 step 1.
    // Both are below the 83 MHz threshold above which the fudge program is
    // required, so fudge stays off.
    run("SPI  50 MHz (shapones today)", false, 3.0f, false);
    run("SPI  75 MHz (step 1 target)",  false, 2.0f, false);
    run("QPI  50 MHz",                  true,  3.0f, false);
    run("QPI  75 MHz",                  true,  2.0f, false);

    printf("\n=== done ===\n");
    printf("A QPI row that FAILS while its SPI row passes points at GP4/GP5.\n");
    printf("A QPI row that passes but is no faster means QPI never engaged.\n");

    while (true) tight_loop_contents();
}
