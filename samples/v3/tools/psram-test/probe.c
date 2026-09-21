// PicoCalc PSRAM QPI diagnostic probe.
//
// The bulk benchmark showed QPI running at quad speed but returning wrong
// data. That cannot distinguish three causes, because the PIO clocks out
// 4-bit frames at 4-bit speed whether or not the chip ever entered QPI mode:
//
//   (a) the chip never entered QPI, so it saw garbage commands;
//   (b) the chip is in QPI but SIO2/SIO3 (GP4/GP5) do not work;
//   (c) the quad program's sampling phase is wrong at these divisors.
//
// Two probes settle it, neither depending on bulk throughput.
//
// PROBE 1 - Read ID in both modes. A known-answer read: ESP-PSRAM64H returns
// manufacturer ID 0x0D. Reading the same ID in SPI and in QPI proves the chip
// entered QPI *and* that all four data lines carry it.
//
// PROBE 2 - cross-mode. Entering QPI does not disturb the array, so writing in
// one mode and reading in the other isolates direction: which of the read path
// and the write path is at fault.
//
// Failing cases dump expected vs received bytes, because the shape of the
// corruption is itself diagnostic - a constant nibble means a dead data line.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "psram_spi.h"

#define BENCH_SYS_CLK_HZ  (300 * 1000 * 1000)
#define CLKDIV      3.0f            // 50 MHz SCK, matches shapones today
#define TEST_ADDR   0x000100u
#define TEST_LEN    4096u

static uint8_t buf[TEST_LEN];

static inline uint8_t pat(uint32_t addr) {
    return (uint8_t)((addr * 31u) ^ (addr >> 8) ^ 0x5Au);
}

#define RD_CHUNK(q) ((q) ? 127u : 31u)
#define WR_CHUNK(q) ((q) ? 119u : 27u)

static void block_write(psram_spi_inst_t *s, uint32_t a, const uint8_t *src, size_t n, bool q) {
    size_t cap = WR_CHUNK(q);
    while (n) { size_t k = n > cap ? cap : n; psram_write(s, a, src, k); a += k; src += k; n -= k; }
}
static void block_read(psram_spi_inst_t *s, uint32_t a, uint8_t *dst, size_t n, bool q) {
    size_t cap = RD_CHUNK(q);
    while (n) { size_t k = n > cap ? cap : n; psram_read(s, a, dst, k); a += k; dst += k; n -= k; }
}

static psram_spi_inst_t spi_init(void)  { return psram_spi_init_clkdiv(pio0, -1, CLKDIV, false, false); }

static psram_spi_inst_t qpi_init(void) {
    psram_spi_inst_t s = psram_spi_init_clkdiv(pio0, -1, CLKDIV, false, false);
    uint8_t enter[] = { 8, 0, 0x35u };
    pio_spi_write_read_dma_blocking(&s, enter, 3, 0, 0);
    psram_spi_uninit(s);                       // quad == false, so no 0xF5
    return psram_spi_init_clkdiv(pio0, -1, CLKDIV, false, true);
}

// Read ID: 0x9F then a 24-bit don't-care address, then `n` bytes back.
// Lengths are bits in SPI mode and nibbles in QPI mode.
static void read_id(psram_spi_inst_t *s, uint8_t *dst, size_t n) {
    uint8_t cmd[6];
    cmd[0] = s->quad ? (uint8_t)(4 * 2) : (uint8_t)(4 * 8);   // 0x9F + 3 address bytes
    cmd[1] = s->quad ? (uint8_t)(n * 2) : (uint8_t)(n * 8);
    cmd[2] = 0x9Fu;
    cmd[3] = cmd[4] = cmd[5] = 0;
    memset(dst, 0, n);
    pio_spi_write_read_dma_blocking(s, cmd, sizeof(cmd), dst, n);
}

static void hexline(const char *label, const uint8_t *p, size_t n) {
    printf("      %s", label);
    for (size_t i = 0; i < n; i++) printf(" %02X", p[i]);
    printf("\n");
}

// Verify buf against the pattern; report count and shape.
static uint32_t verify(uint32_t addr, size_t n) {
    uint32_t errors = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] != pat(addr + i)) errors++;
    if (errors == 0) {
        printf("      0 errors -- PASS\n");
    } else {
        printf("      %u / %u bytes wrong (%.2f%%) -- FAIL\n",
               errors, (unsigned)n, 100.0 * errors / n);
        uint8_t exp[16];
        for (size_t i = 0; i < 16; i++) exp[i] = pat(addr + i);
        hexline("expected:", exp, 16);
        hexline("received:", buf, 16);
        // A data line stuck high or low shows as a constant nibble.
        uint8_t hi_or = 0, hi_and = 0xF, lo_or = 0, lo_and = 0xF;
        for (size_t i = 0; i < n; i++) {
            hi_or |= buf[i] >> 4;   hi_and &= buf[i] >> 4;
            lo_or |= buf[i] & 0xF;  lo_and &= buf[i] & 0xF;
        }
        printf("      nibble bits: high always-set 0x%X always-clear 0x%X | "
               "low always-set 0x%X always-clear 0x%X\n",
               hi_and, (uint8_t)(~hi_or & 0xF), lo_and, (uint8_t)(~lo_or & 0xF));
    }
    return errors;
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(100);
    set_sys_clock_khz(BENCH_SYS_CLK_HZ / 1000, true);
    stdio_init_all();
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(500);

    printf("\n\n=== PicoCalc PSRAM QPI probe ===\n");
    printf("sys_clk %u Hz, SCK %.1f MHz (clkdiv %.1f), %u byte window at 0x%06X\n",
           (unsigned)clock_get_hz(clk_sys),
           (double)(BENCH_SYS_CLK_HZ / CLKDIV / 2 / 1e6), (double)CLKDIV,
           TEST_LEN, TEST_ADDR);

    uint8_t id_spi[8], id_qpi[8];

    // ---- PROBE 1: Read ID -------------------------------------------------
    printf("\n[1] Read ID (0x9F)\n");
    {
        psram_spi_inst_t s = spi_init();
        read_id(&s, id_spi, sizeof(id_spi));
        hexline("SPI mode:", id_spi, sizeof(id_spi));
        psram_spi_uninit(s);
    }
    {
        psram_spi_inst_t s = qpi_init();
        read_id(&s, id_qpi, sizeof(id_qpi));
        hexline("QPI mode:", id_qpi, sizeof(id_qpi));
        psram_spi_uninit(s);                  // sends 0xF5 to leave QPI
    }
    printf("    SPI manufacturer ID 0x%02X (ESP-PSRAM64H should be 0x0D)\n", id_spi[0]);
    if (memcmp(id_spi, id_qpi, sizeof(id_spi)) == 0 && id_spi[0] == 0x0D) {
        printf("    => IDs MATCH: QPI is engaged and all four data lines carry data.\n");
    } else if (id_spi[0] == 0x0D) {
        printf("    => IDs DIFFER: SPI identifies the part, QPI does not read it back.\n");
        printf("       Either the chip never entered QPI, or SIO2/SIO3 are not working.\n");
    } else {
        printf("    => SPI ID is wrong too -- suspect the test itself, not QPI.\n");
    }

    // ---- PROBE 2: cross-mode ----------------------------------------------
    printf("\n[2] Cross-mode read/write\n");

    printf("    control: write SPI, read SPI\n");
    {
        psram_spi_inst_t s = spi_init();
        for (uint32_t i = 0; i < TEST_LEN; i++) buf[i] = pat(TEST_ADDR + i);
        block_write(&s, TEST_ADDR, buf, TEST_LEN, false);
        memset(buf, 0, TEST_LEN);
        block_read(&s, TEST_ADDR, buf, TEST_LEN, false);
        verify(TEST_ADDR, TEST_LEN);
        psram_spi_uninit(s);
    }

    printf("    A: write SPI, read QPI  (isolates the QPI READ path)\n");
    {
        psram_spi_inst_t s = spi_init();
        for (uint32_t i = 0; i < TEST_LEN; i++) buf[i] = pat(TEST_ADDR + i);
        block_write(&s, TEST_ADDR, buf, TEST_LEN, false);
        psram_spi_uninit(s);

        psram_spi_inst_t q = qpi_init();
        memset(buf, 0, TEST_LEN);
        block_read(&q, TEST_ADDR, buf, TEST_LEN, true);
        verify(TEST_ADDR, TEST_LEN);
        psram_spi_uninit(q);
    }

    printf("    B: write QPI, read SPI  (isolates the QPI WRITE path)\n");
    {
        // Scrub first in SPI so stale correct data cannot mask a failed write.
        psram_spi_inst_t s = spi_init();
        memset(buf, 0xA5, TEST_LEN);
        block_write(&s, TEST_ADDR, buf, TEST_LEN, false);
        psram_spi_uninit(s);

        psram_spi_inst_t q = qpi_init();
        for (uint32_t i = 0; i < TEST_LEN; i++) buf[i] = pat(TEST_ADDR + i);
        block_write(&q, TEST_ADDR, buf, TEST_LEN, true);
        psram_spi_uninit(q);

        psram_spi_inst_t s2 = spi_init();
        memset(buf, 0, TEST_LEN);
        block_read(&s2, TEST_ADDR, buf, TEST_LEN, false);
        verify(TEST_ADDR, TEST_LEN);
        psram_spi_uninit(s2);
    }

    printf("\n=== done ===\n");
    printf("A passes, B fails  -> QPI read path fine, write path at fault.\n");
    printf("A fails, B passes  -> QPI write path fine, read path at fault.\n");
    printf("both fail, IDs differ -> chip likely never entered QPI, or GP4/GP5 dead.\n");
    printf("both pass -> QPI works; the bulk benchmark's failure is elsewhere.\n");

    while (true) tight_loop_contents();
}
