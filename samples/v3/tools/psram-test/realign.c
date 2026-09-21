// PicoCalc PSRAM QPI: prove the one-nibble read offset by correcting it in
// software.
//
// Why not a dummy-cycle sweep: read_quad_command[0] is NOT a free dummy-cycle
// knob. It is the count of nibbles the PIO pulls from the TX FIFO, and the
// command buffer supplies exactly that many (9 bytes, 2 consumed by "out x,8"
// and "out y,8", leaving 7 payload bytes = 14 nibbles). Raise it and the PIO
// waits for nibbles that never arrive, so the read DMA never completes and the
// board hangs - which is what the sweep firmware did. Lower it and leftover
// nibbles desync the next transaction. The dummy count cannot be changed from
// C at all; it is structural to the PIO program.
//
// So this proves the diagnosis a different way. If QPI reads really are shifted
// left by exactly one nibble, then starting the read one byte early and
// shifting the result back by one nibble must reconstruct the data exactly:
//
//   received nibble i == true nibble i+1
//   => byte k = (received[k] & 0x0F) << 4 | (received[k+1] >> 4)
//
// A clean PASS here means the data path and all four lines are perfect and the
// only fault is read latency - after which the sole remaining work is doing
// this shift in the PIO rather than on the CPU. A failure means the offset is
// not a constant single nibble and the model is wrong.
//
// The realignment is deliberately NOT a proposed fix: it costs a pass over the
// buffer and reads one byte extra. It is an experiment.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "psram_spi.h"

#define BENCH_SYS_CLK_HZ  (300 * 1000 * 1000)
#define TEST_ADDR   0x001000u           // must be >= 1: we read one byte early
#define TEST_LEN    16384u

static uint8_t raw[TEST_LEN + 1];
static uint8_t out[TEST_LEN];

static inline uint8_t pat(uint32_t addr) {
    return (uint8_t)((addr * 31u) ^ (addr >> 8) ^ 0x5Au);
}

static psram_spi_inst_t spi_init(float d) { return psram_spi_init_clkdiv(pio0, -1, d, false, false); }

static psram_spi_inst_t qpi_init(float d) {
    psram_spi_inst_t s = spi_init(d);
    uint8_t enter[] = { 8, 0, 0x35u };
    pio_spi_write_read_dma_blocking(&s, enter, 3, 0, 0);
    psram_spi_uninit(s);
    return psram_spi_init_clkdiv(pio0, -1, d, false, true);
}

static void spi_write_pattern(float d) {
    psram_spi_inst_t s = spi_init(d);
    for (uint32_t off = 0; off < TEST_LEN; off += 27) {
        uint32_t n = (TEST_LEN - off) > 27 ? 27 : (TEST_LEN - off);
        uint8_t tmp[27];
        for (uint32_t i = 0; i < n; i++) tmp[i] = pat(TEST_ADDR + off + i);
        psram_write(&s, TEST_ADDR + off, tmp, n);
    }
    psram_spi_uninit(s);
}

static uint32_t qpi_read(psram_spi_inst_t *q, uint32_t addr, uint8_t *dst, uint32_t len) {
    uint32_t t0 = time_us_32();
    for (uint32_t off = 0; off < len; off += 127) {
        uint32_t n = (len - off) > 127 ? 127 : (len - off);
        psram_read(q, addr + off, dst + off, n);
    }
    return time_us_32() - t0;
}

static uint32_t count_errors(const uint8_t *p) {
    uint32_t e = 0;
    for (uint32_t i = 0; i < TEST_LEN; i++) if (p[i] != pat(TEST_ADDR + i)) e++;
    return e;
}

static void run(float div) {
    float sck = (float)BENCH_SYS_CLK_HZ / div / 2.0f / 1e6f;
    printf("\nclkdiv %.2f  ->  SCK %.1f MHz\n", (double)div, (double)sck);

    spi_write_pattern(div);

    // Control: read it back over SPI.
    {
        psram_spi_inst_t s = spi_init(div);
        memset(out, 0, TEST_LEN);
        for (uint32_t off = 0; off < TEST_LEN; off += 31) {
            uint32_t n = (TEST_LEN - off) > 31 ? 31 : (TEST_LEN - off);
            psram_read(&s, TEST_ADDR + off, out + off, n);
        }
        psram_spi_uninit(s);
        printf("  control  SPI read        : %u errors\n", count_errors(out));
    }

    psram_spi_inst_t q = qpi_init(div);

    // 1. Stock QPI read, for the record.
    memset(out, 0, TEST_LEN);
    uint32_t us_stock = qpi_read(&q, TEST_ADDR, out, TEST_LEN);
    printf("  stock    QPI read        : %u errors, %u KB/s\n",
           count_errors(out), (uint32_t)((uint64_t)TEST_LEN * 1000000u / us_stock / 1024u));

    // 2. Start one byte early, then shift the whole buffer back by one nibble.
    memset(raw, 0, TEST_LEN + 1);
    uint32_t us_re = qpi_read(&q, TEST_ADDR - 1, raw, TEST_LEN + 1);
    for (uint32_t k = 0; k < TEST_LEN; k++)
        out[k] = (uint8_t)(((raw[k] & 0x0Fu) << 4) | (raw[k + 1] >> 4));
    uint32_t err = count_errors(out);
    printf("  realigned QPI read       : %u errors, %u KB/s (read only, excl. shift)\n",
           err, (uint32_t)((uint64_t)TEST_LEN * 1000000u / us_re / 1024u));

    if (err == 0) {
        printf("  => PASS: offset is exactly one nibble, data path is perfect.\n");
    } else {
        printf("  => FAIL: a constant one-nibble shift does not explain it here.\n");
        uint8_t exp[8];
        for (int i = 0; i < 8; i++) exp[i] = pat(TEST_ADDR + i);
        printf("     expected:");  for (int i = 0; i < 8; i++) printf(" %02X", exp[i]);
        printf("\n     realigned:"); for (int i = 0; i < 8; i++) printf(" %02X", out[i]);
        printf("\n     raw:      ");  for (int i = 0; i < 8; i++) printf(" %02X", raw[i]);
        printf("\n");
    }

    psram_spi_uninit(q);
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(100);
    set_sys_clock_khz(BENCH_SYS_CLK_HZ / 1000, true);
    stdio_init_all();
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(500);

    printf("\n\n=== PicoCalc PSRAM QPI nibble-realign test ===\n");
    printf("sys_clk %u Hz, %u KB per row, pattern written over SPI\n",
           (unsigned)clock_get_hz(clk_sys), TEST_LEN / 1024u);
    printf("Reads one byte early and shifts back one nibble in software.\n");
    printf("This is an experiment, not a proposed fix.\n");

    run(3.0f);
    run(2.0f);

    printf("\n=== done ===\n");
    printf("Both rows PASS -> the offset is a constant nibble at both clocks,\n");
    printf("  and the fix is to drop one turnaround cycle in the quad PIO program.\n");
    printf("50 passes, 75 fails -> the offset moves with clock: a sampling-phase\n");
    printf("  problem, and the fudge/no-fudge split needs a threshold, not a constant.\n");

    while (true) tight_loop_contents();
}
