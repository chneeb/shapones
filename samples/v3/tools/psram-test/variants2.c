// PicoCalc PSRAM: QPI read-phase variant test, corrected harness.
//
// The first version of this test produced one trustworthy row and fourteen
// bogus TIMEOUTs, because the chip was left in QPI mode after the first
// combination and every later one then talked SPI to a chip listening in QPI.
// Two causes, both in teardown:
//
//   * psram_spi_uninit() sends its 0xF5 exit-QPI AFTER unclaiming both DMA
//     channels, and this harness additionally disabled the state machine and
//     removed the program before calling it - so the exit went nowhere.
//   * the library's exit command is {8, 0, 0xF5}: in QPI framing that byte is a
//     NIBBLE count, so it asks for 8 nibbles and supplies 2. Correct is {2,...}.
//
// This version never uses the library's quad path. It takes one SPI instance
// (which sets up the DMA channels), swaps the state machine over to a variant
// program for the reads, sends a correctly sized exit through the RUNNING state
// machine, and only then tears down. Between every combination it verifies the
// part answers Read ID as 0x0D over SPI, and attempts recovery if not - so a
// contaminated run announces itself instead of quietly producing nonsense.

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/dma.h"
#include "hardware/watchdog.h"

#include "psram_spi.h"
#include "qv.pio.h"

#define BENCH_SYS_CLK_HZ  (300 * 1000 * 1000)
#define TEST_ADDR   0x001000u
#define TEST_LEN    4096u
#define READ_TIMEOUT_US 20000u
#define WD_TIMEOUT_MS   4000u
#define SCRATCH_MAGIC   0x51504932u

static uint8_t buf[TEST_LEN];

static inline uint8_t pat(uint32_t a) { return (uint8_t)((a * 31u) ^ (a >> 8) ^ 0x5Au); }

typedef struct {
    const char *name;
    const pio_program_t *prog;
    pio_sm_config (*cfg)(uint);
    int y_adj;
} variant_t;

static const variant_t VARIANTS[] = {
    { "long/fall (stock)", &qv_long_fall_program,  qv_long_fall_program_get_default_config,   0 },
    { "long/rise        ", &qv_long_rise_program,  qv_long_rise_program_get_default_config,   0 },
    { "short/fall       ", &qv_short_fall_program, qv_short_fall_program_get_default_config, -1 },
    { "short/rise       ", &qv_short_rise_program, qv_short_rise_program_get_default_config, -1 },
};
#define N_VAR 4
static const uint8_t DUMMY_BYTES[] = { 3, 2, 4 };
#define N_DUM 3
static const float CLKDIVS[] = { 3.0f, 2.0f };
#define N_CLK 2
#define N_CFG (N_VAR * N_DUM * N_CLK)

enum { R_NONE = 0, R_PASS, R_FAIL, R_TIMEOUT, R_HUNG, R_DIRTY };
static const char *RNAME[] = { "-", "PASS", "fail", "TIMEOUT", "HUNG", "chip-dirty" };

static void res_set(unsigned i, unsigned v) {
    volatile uint32_t *w = &watchdog_hw->scratch[1 + i / 10];
    unsigned sh = (i % 10) * 3;
    *w = (*w & ~(7u << sh)) | ((v & 7u) << sh);
}
static unsigned res_get(unsigned i) {
    return (watchdog_hw->scratch[1 + i / 10] >> ((i % 10) * 3)) & 7u;
}

static psram_spi_inst_t spi_init(float d) { return psram_spi_init_clkdiv(pio0, -1, d, false, false); }

// Bounded transfer. Returns false on deadline, leaving the SM back at `entry`.
static bool xfer(psram_spi_inst_t *s, uint entry, const uint8_t *cmd, size_t cmd_len,
                 uint8_t *dst, size_t dst_len) {
    dma_channel_transfer_from_buffer_now(s->write_dma_chan, cmd, cmd_len);
    dma_channel_transfer_to_buffer_now(s->read_dma_chan, dst, dst_len);
    absolute_time_t dl = make_timeout_time_us(READ_TIMEOUT_US);
    while ((dma_channel_is_busy(s->write_dma_chan) || dma_channel_is_busy(s->read_dma_chan))
           && absolute_time_diff_us(get_absolute_time(), dl) > 0) tight_loop_contents();
    if (!dma_channel_is_busy(s->write_dma_chan) && !dma_channel_is_busy(s->read_dma_chan))
        return true;
    dma_channel_abort(s->write_dma_chan);
    dma_channel_abort(s->read_dma_chan);
    pio_sm_set_enabled(s->pio, s->sm, false);
    pio_sm_clear_fifos(s->pio, s->sm);
    pio_sm_restart(s->pio, s->sm);
    pio_sm_exec(s->pio, s->sm, pio_encode_jmp(entry));
    pio_sm_set_enabled(s->pio, s->sm, true);
    return false;
}

// Read ID over SPI. Lengths are BITS in SPI framing.
static uint8_t spi_read_id(psram_spi_inst_t *s) {
    uint8_t cmd[6] = { 4 * 8, 8 * 1, 0x9Fu, 0, 0, 0 };
    uint8_t id = 0;
    xfer(s, s->offset, cmd, sizeof(cmd), &id, 1);
    return id;
}

// True if the part answers Read ID over SPI, i.e. it is not stuck in QPI.
static bool chip_in_spi(float div) {
    psram_spi_inst_t s = spi_init(div);
    uint8_t id = spi_read_id(&s);
    psram_spi_uninit(s);        // quad == false: no stray 0xF5, removes the right program
    return id == 0x0Du;
}

// Send a correctly sized exit-QPI through a running QPI state machine.
// In QPI framing the count is NIBBLES: one byte of payload is 2.
static void exit_qpi(psram_spi_inst_t *s, uint entry) {
    uint8_t cmd[3] = { 2, 0, 0xF5u };
    xfer(s, entry, cmd, sizeof(cmd), 0, 0);
}

static bool recover_to_spi(float div) {
    // Load the stock quad program purely to deliver a well-formed exit-QPI.
    psram_spi_inst_t s = spi_init(div);
    uint offs = pio_add_program(pio0, &qv_long_fall_program);
    pio_sm_set_enabled(pio0, s.sm, false);
    qv_sm_init(pio0, s.sm, offs, qv_long_fall_program_get_default_config(offs),
               div, PSRAM_PIN_CS, PSRAM_PIN_MOSI);
    exit_qpi(&s, offs);
    pio_sm_set_enabled(pio0, s.sm, false);
    pio_remove_program(pio0, &qv_long_fall_program, offs);
    psram_spi_uninit(s);
    return chip_in_spi(div);
}

static unsigned run_cfg(unsigned vi, unsigned di, unsigned ci) {
    float div = CLKDIVS[ci];
    const variant_t *v = &VARIANTS[vi];
    uint8_t dummy = DUMMY_BYTES[di];

    if (!chip_in_spi(div)) {
        if (!recover_to_spi(div)) return R_DIRTY;
    }
    watchdog_update();

    psram_spi_inst_t s = spi_init(div);

    // Pattern, written over SPI while the SPI program is still loaded.
    for (uint32_t off = 0; off < TEST_LEN; off += 27) {
        uint32_t n = (TEST_LEN - off) > 27 ? 27 : (TEST_LEN - off);
        uint8_t t[27];
        for (uint32_t i = 0; i < n; i++) t[i] = pat(TEST_ADDR + off + i);
        psram_write(&s, TEST_ADDR + off, t, n);
    }
    watchdog_update();

    // Enter QPI: 8 BITS of payload, SPI framing.
    { uint8_t enter[3] = { 8, 0, 0x35u }; xfer(&s, s.offset, enter, 3, 0, 0); }

    // Swap the state machine onto the variant program. DMA channels are
    // untouched: they only move bytes to and from this SM's FIFOs.
    uint offs = pio_add_program(pio0, v->prog);
    pio_sm_set_enabled(pio0, s.sm, false);
    qv_sm_init(pio0, s.sm, offs, v->cfg(offs), div, PSRAM_PIN_CS, PSRAM_PIN_MOSI);

    unsigned result = R_PASS;
    memset(buf, 0, TEST_LEN);
    for (uint32_t off = 0; off < TEST_LEN; off += 127) {
        uint32_t n = (TEST_LEN - off) > 127 ? 127 : (TEST_LEN - off);
        uint8_t payload = (uint8_t)(1 + 3 + dummy);
        uint8_t cmd[16];
        cmd[0] = (uint8_t)(payload * 2);
        cmd[1] = (uint8_t)((int)(n * 2) + v->y_adj);
        cmd[2] = 0xEBu;
        cmd[3] = (uint8_t)((TEST_ADDR + off) >> 16);
        cmd[4] = (uint8_t)((TEST_ADDR + off) >> 8);
        cmd[5] = (uint8_t)(TEST_ADDR + off);
        memset(cmd + 6, 0, dummy);
        if (!xfer(&s, offs, cmd, 2 + payload, buf + off, n)) { result = R_TIMEOUT; break; }
        watchdog_update();
    }
    if (result == R_PASS) {
        for (uint32_t i = 0; i < TEST_LEN; i++)
            if (buf[i] != pat(TEST_ADDR + i)) { result = R_FAIL; break; }
    }

    // Exit QPI while the SM is still running, then tear down in order.
    exit_qpi(&s, offs);
    pio_sm_set_enabled(pio0, s.sm, false);
    pio_remove_program(pio0, v->prog, offs);
    psram_spi_uninit(s);
    watchdog_update();

    // If the part is not answering over SPI again, this row's successor would
    // be meaningless - say so rather than letting it look like a real result.
    if (!chip_in_spi(div) && !recover_to_spi(div)) return R_DIRTY;
    return result;
}

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(100);
    set_sys_clock_khz(BENCH_SYS_CLK_HZ / 1000, true);
    stdio_init_all();
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(500);

    bool resumed = watchdog_caused_reboot() && watchdog_hw->scratch[0] == SCRATCH_MAGIC;
    unsigned start = 0;
    if (resumed) {
        start = watchdog_hw->scratch[3];
        res_set(start, R_HUNG);
        start++;
    } else {
        watchdog_hw->scratch[0] = SCRATCH_MAGIC;
        watchdog_hw->scratch[1] = watchdog_hw->scratch[2] = 0;
    }

    printf("\n\n=== PicoCalc PSRAM QPI read-phase variants (v2) ===\n");
    if (resumed) printf("(resumed after watchdog reboot; config %u marked HUNG)\n", start - 1);
    printf("sys_clk %u Hz, %u KB per combination, pattern written over SPI\n",
           (unsigned)clock_get_hz(clk_sys), TEST_LEN / 1024u);
    printf("Read ID checked between combinations; stock = long/fall, dummy 3\n");
    printf("boot check: chip answers Read ID over SPI: %s\n\n",
           chip_in_spi(3.0f) ? "yes" : "NO");

    watchdog_enable(WD_TIMEOUT_MS, 1);

    for (unsigned i = start; i < N_CFG; i++) {
        unsigned ci = i % N_CLK, di = (i / N_CLK) % N_DUM, vi = i / (N_CLK * N_DUM);
        watchdog_hw->scratch[3] = i;
        watchdog_update();
        printf("  %-17s dummy %u  SCK %2.0f MHz : ", VARIANTS[vi].name, DUMMY_BYTES[di],
               (double)(BENCH_SYS_CLK_HZ / CLKDIVS[ci] / 2 / 1e6));
        fflush(stdout);
        unsigned r = run_cfg(vi, di, ci);
        res_set(i, r);
        printf("%s\n", RNAME[r]);
    }

    watchdog_disable();

    printf("\n=== summary ===\n");
    for (unsigned i = 0; i < N_CFG; i++) {
        unsigned ci = i % N_CLK, di = (i / N_CLK) % N_DUM, vi = i / (N_CLK * N_DUM);
        printf("  %-17s dummy %u  SCK %2.0f MHz : %s\n", VARIANTS[vi].name, DUMMY_BYTES[di],
               (double)(BENCH_SYS_CLK_HZ / CLKDIVS[ci] / 2 / 1e6), RNAME[res_get(i)]);
    }
    printf("\nExpected: short/fall dummy 3 passes at 50 MHz (already confirmed).\n");
    printf("Any 'chip-dirty' row means the part could not be returned to SPI mode,\n");
    printf("so that row and the ones after it are not evidence.\n");

    while (true) tight_loop_contents();
}
