#include "psram_loader.hpp"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"

#include "psram_spi.h"
#include "shapones/memory.hpp"
#include "common.hpp"  // SYS_CLK_FREQ, MHZ

// PSRAM pin assignments (GP20=CS, GP21=SCK, GP2=MOSI, GP3=MISO via PIO1)

// PRG slot-index offset: values written into prgrom_remap_table by us are
// always ≥ PRG_SLOT_OFFSET, while mapper-written physical bank indices are
// always < PRG_SLOT_OFFSET (max PRG bank for mappers 0–4 = 63, i.e. 512 KB).
static constexpr int PRG_SLOT_OFFSET = 64;

// PRG SRAM cache: 12 × 8 KB = 96 KB. There are only 4 CPU PRG windows, so the
// extra 8 slots act as a victim cache: when a window switches back to a bank
// that's still resident, it's a free hit with no PSRAM read. This is the main
// lever on MMC3 frame rate — every miss is a slow ~8 KB PSRAM read on Core 0,
// and the PPU is throttled to CPU progress, so misses directly cost fps. SMB3
// gameplay churns > 8 banks (dropped to ~35 fps at 8 slots); 12 covers more of
// the working set. SRAM budget caps this ~12–14 (frame buffer 187 KB + 128 KB
// CHR cache + this); re-check `arm-none-eabi-size` BSS before growing further.
static constexpr int PRG_SLOTS = 12;

// Address mask large enough to cover all slot-encoded PRG indices.
// Slot (64+11) * 8192 + 8191 = 622591 < 0x100000.
static constexpr uint32_t PRG_ADDR_MASK = 0xFFFFF;

static uint8_t prg_sram[PRG_SLOTS * shapones::memory::PRGROM_BLOCK_SIZE];

// Physical bank resident in each slot (-1 = empty).
static int      prg_cache_bank[PRG_SLOTS];
// Slot currently backing each CPU PRG window (-1 = none). A slot listed here is
// "pinned" and must never be chosen as an eviction victim for another window.
static int      prg_window_slot[shapones::memory::PRGROM_REMAP_TABLE_SIZE];
// LRU timestamps (monotonic); higher = more recently used. Empty slots keep 0.
static uint32_t prg_slot_stamp[PRG_SLOTS];
static uint32_t prg_lru_clock;

// CHR full pre-cache: all CHR-ROM data loaded from PSRAM into SRAM at boot.
// Avoids the shifted-pointer slot trick for CHR, eliminating the Core 1
// BusFault that occurs when the mapper writes a raw physical bank index into
// chrrom_remap_table before psram_sync_chr() can fix it.
static uint8_t *chr_full_cache = nullptr;

// PSRAM layout
static uint32_t g_chr_psram_base;  // = prg_phys_size (PRG comes first)
static uint32_t g_chr_phys_size;   // 0 for CHR-RAM games

bool psram_active = false;

static psram_spi_inst_t g_spi;
static const char *g_psram_mode = "?";

// ── small-chunk helpers ──
// The library packs the transfer length into a uint8_t. In SPI framing that
// field counts BITS (so ≤ 31 bytes read, ≤ 27 write); in QPI it counts NIBBLES,
// which is why the same field reaches four times further. Part of QPI's
// throughput win is this larger chunk, not just the four data lines.
static bool psram_xfer(psram_spi_inst_t *s, const uint8_t *cmd, size_t cmd_len,
                       uint8_t *dst, size_t dst_len);

// QPI reaches four times further per call, so the chunk follows the mode we
// actually ended up in - see psram_enter_qpi(), which falls back to SPI.
static inline int rd_chunk() { return g_spi.quad ? 127 : 31; }
static inline int wr_chunk() { return g_spi.quad ? 123 : 27; }

// These use the library's own psram_read()/psram_write(). An earlier attempt
// replaced them with hand-built command buffers going through psram_xfer(), to
// make the boot unhangable - and that broke single-bit SPI reads, which had
// worked for months. The bounded path is kept for probing an uncertain mode
// (see psram_enter_qpi), not for the data path.
static void psram_read_n(uint32_t addr, uint8_t *dst, size_t count) {
    while (count > 0) {
        size_t n = (count > (size_t)rd_chunk()) ? (size_t)rd_chunk() : count;
        psram_read(&g_spi, addr, dst, n);
        addr  += n;
        dst   += n;
        count -= n;
    }
}

static void psram_write_n(uint32_t addr, const uint8_t *src, size_t count) {
    while (count > 0) {
        size_t n = (count > (size_t)wr_chunk()) ? (size_t)wr_chunk() : count;
        psram_write(&g_spi, addr, src, n);
        addr  += n;
        src   += n;
        count -= n;
    }
}

// ── PRG cache (set-associative victim cache) ──
// Point CPU window `win` at whichever slot holds `phys_bank`, loading it from
// PSRAM on a miss. Correctness rule that avoids the earlier wrong-bank crash:
// an eviction victim is only ever a slot NOT pinned by some *other* window's
// current mapping, so repointing `win` can never yank a bank out from under a
// live window. With PRG_SLOTS (8) > windows (4) such a victim always exists.
static void load_prg_bank(int win, uint16_t phys_bank) {
    // Hit: bank already resident.
    for (int s = 0; s < PRG_SLOTS; s++) {
        if (prg_cache_bank[s] == (int)phys_bank) {
            prg_window_slot[win] = s;
            prg_slot_stamp[s] = ++prg_lru_clock;
            shapones::memory::prgrom_remap_table[win] = PRG_SLOT_OFFSET + s;
            return;
        }
    }
    // Miss: pick the least-recently-used slot that no *other* window pins.
    int victim = -1;
    uint32_t best = 0xFFFFFFFFu;
    for (int s = 0; s < PRG_SLOTS; s++) {
        bool pinned = false;
        for (int w = 0; w < shapones::memory::PRGROM_REMAP_TABLE_SIZE; w++) {
            if (w != win && prg_window_slot[w] == s) { pinned = true; break; }
        }
        if (pinned) continue;
        if (prg_slot_stamp[s] < best) { best = prg_slot_stamp[s]; victim = s; }
    }
    uint32_t psram_addr = (uint32_t)phys_bank * shapones::memory::PRGROM_BLOCK_SIZE;
    psram_read_n(psram_addr,
                 prg_sram + victim * shapones::memory::PRGROM_BLOCK_SIZE,
                 shapones::memory::PRGROM_BLOCK_SIZE);
    prg_cache_bank[victim] = phys_bank;
    prg_window_slot[win] = victim;
    prg_slot_stamp[victim] = ++prg_lru_clock;
    shapones::memory::prgrom_remap_table[win] = PRG_SLOT_OFFSET + victim;
}

void psram_sync_prg() {
    for (int i = 0; i < shapones::memory::PRGROM_REMAP_TABLE_SIZE; i++) {
        uint16_t rval = shapones::memory::prgrom_remap_table[i];
        // A raw physical index (< PRG_SLOT_OFFSET) means the mapper remapped this
        // window and the hook hasn't reconciled it yet (or this is the initial
        // load); resolve it now. Slot-encoded values are already in sync because
        // the synchronous hook runs load_prg_bank() on every remap.
        if (rval < PRG_SLOT_OFFSET) {
            load_prg_bank(i, rval);
        }
    }
}

// CHR is fully pre-cached in SRAM so no per-frame sync is needed.
void psram_sync_chr() {}

// ── Init ──

// Bring the part up in QPI at our own clock. The library's psram_qpi_init()
// hardcodes clkdiv 1.0, which would be 150 MHz SCK at a 300 MHz system clock.
// Sequence: come up in SPI, send Enter QPI (0x35, 8 BITS in SPI framing), drop
// the SPI instance, then re-init with the quad program. Quad init deliberately
// skips the 0x66/0x99 reset, which are SPI-mode commands.
// The boot path must not be able to hang. Every psram_read/psram_write in the
// library waits on its DMA forever, so a chip that is in the wrong mode - or
// simply not answering - takes the whole boot with it rather than failing. The
// standalone test firmware never hit this because it bounded every transfer;
// the loader did not, and a stuck boot is exactly what that cost.
static bool psram_xfer(psram_spi_inst_t *s, const uint8_t *cmd, size_t cmd_len,
                       uint8_t *dst, size_t dst_len) {
    constexpr uint32_t TIMEOUT_US = 20000;
    dma_channel_transfer_from_buffer_now(s->write_dma_chan, cmd, cmd_len);
    dma_channel_transfer_to_buffer_now(s->read_dma_chan, dst, dst_len);
    absolute_time_t dl = make_timeout_time_us(TIMEOUT_US);
    while ((dma_channel_is_busy(s->write_dma_chan) || dma_channel_is_busy(s->read_dma_chan))
           && absolute_time_diff_us(get_absolute_time(), dl) > 0) {
        tight_loop_contents();
    }
    if (!dma_channel_is_busy(s->write_dma_chan) && !dma_channel_is_busy(s->read_dma_chan))
        return true;
    dma_channel_abort(s->write_dma_chan);
    dma_channel_abort(s->read_dma_chan);
    pio_sm_set_enabled(s->pio, s->sm, false);
    pio_sm_clear_fifos(s->pio, s->sm);
    pio_sm_restart(s->pio, s->sm);
    pio_sm_exec(s->pio, s->sm, pio_encode_jmp(s->offset));
    pio_sm_set_enabled(s->pio, s->sm, true);
    return false;
}

// The part keeps its mode across a warm reset - a reflash resets the RP2350 but
// not the PSRAM - so a previous run that ended in QPI leaves it in QPI, and the
// next boot's SPI-framed commands land on a chip that is not listening. That is
// what turned a passing self test into a hung one between two builds: the same
// code, a different starting mode. Normalise before assuming anything.
static void psram_force_spi(PIO pio, float clkdiv) {
    psram_spi_inst_t q = psram_spi_init_clkdiv(pio, -1, clkdiv, false, true);
    uint8_t exit_qpi[] = { 2, 0, 0xF5u };   // QPI framing: length counts nibbles
    psram_xfer(&q, exit_qpi, sizeof(exit_qpi), nullptr, 0);
    pio_sm_set_enabled(pio, q.sm, false);
    psram_spi_uninit(q);
}

static constexpr uint32_t QPI_MARKER_ADDR = 0;
static const uint8_t QPI_MARKER[16] = {
    0x5A, 0xA5, 0x0F, 0xF0, 0x33, 0xCC, 0x69, 0x96,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
};

static psram_spi_inst_t psram_enter_qpi(PIO pio, float clkdiv) {
    // Whatever the last run left behind, get the part back to SPI first. The
    // SPI init below then sends the 0x66/0x99 reset from a known state.
    psram_force_spi(pio, clkdiv);

    psram_spi_inst_t s = psram_spi_init_clkdiv(pio, -1, clkdiv, false, false);

    // Marker written over single-bit SPI and read back over QPI below. Both
    // halves of that are device-proven, so it separates the QPI READ path from
    // the QPI WRITE path here at init instead of at first use.
    psram_write(&s, QPI_MARKER_ADDR, QPI_MARKER, sizeof(QPI_MARKER));

    uint8_t enter_qpi[] = { 8, 0, 0x35u };
    pio_spi_write_read_dma_blocking(&s, enter_qpi, 3, 0, 0);

    // Stop the state machine BEFORE its program is torn down.
    // psram_spi_uninit() removes the program from instruction memory without
    // ever disabling the SM (it contains no pio_sm_set_enabled call at all),
    // so the SM carries on executing whatever now sits at those addresses and
    // toggles CS/SCK/SIO at the part - which can put it straight back out of
    // QPI. That is what made the first QPI build read all zeros.
    pio_sm_set_enabled(pio, s.sm, false);
    psram_spi_uninit(s);   // quad == false here, so no stray exit command

    psram_spi_inst_t q = psram_spi_init_clkdiv(pio, -1, clkdiv, false, true);

    uint8_t back[sizeof(QPI_MARKER)];
    memset(back, 0, sizeof(back));
    {   // Bounded equivalent of psram_read(): 0xEB, address, 3 dummy bytes.
        // Length counts nibbles in QPI, minus one for the short read loop.
        uint8_t cmd[9] = { 14, (uint8_t)(sizeof(back) * 2 - 1), 0xEBu,
                           (uint8_t)(QPI_MARKER_ADDR >> 16),
                           (uint8_t)(QPI_MARKER_ADDR >> 8),
                           (uint8_t)QPI_MARKER_ADDR, 0, 0, 0 };
        if (!psram_xfer(&q, cmd, sizeof(cmd), back, sizeof(back)))
            printf("psram: QPI marker read timed out\n");
    }
    if (memcmp(QPI_MARKER, back, sizeof(back)) == 0) {
        printf("psram: QPI read path OK\n");
        g_psram_mode = "QPI";
        return q;
    }

    printf("psram: QPI marker read FAILED, falling back to SPI\n  want:");
    for (unsigned i = 0; i < sizeof(back); i++) printf(" %02X", QPI_MARKER[i]);
    printf("\n  got :");
    for (unsigned i = 0; i < sizeof(back); i++) printf(" %02X", back[i]);
    printf("\n");

    // Leave QPI properly: the exit must go out through the RUNNING state
    // machine, and in QPI framing its length counts nibbles, so one payload
    // byte is 2 - the library's own {8, 0, 0xF5} asks for four times too many.
    uint8_t exit_qpi[] = { 2, 0, 0xF5u };
    pio_spi_write_read_dma_blocking(&q, exit_qpi, 3, 0, 0);
    pio_sm_set_enabled(pio, q.sm, false);
    psram_spi_uninit(q);

    // Single-bit SPI still works and is what shipped before; slower beats dead.
    g_psram_mode = "SPI (QPI unavailable)";
    return psram_spi_init_clkdiv(pio, -1, clkdiv, false, false);
}

bool psram_loader_init() {
    // Reset state that persists through watchdog resets (RP2350 SRAM is not
    // cleared by watchdog).  Without this, a hook registered for a PSRAM game
    // would still fire when the next game (possibly an SRAM game) calls
    // map_ines(), corrupting the remap table.
    psram_active = false;
    shapones::memory::prgrom_bank_switch_hook = nullptr;
    if (chr_full_cache) {
        free(chr_full_cache);
        chr_full_cache = nullptr;
    }
    // Operating point: 100 MHz state-machine clock = 50 MHz SCK, in QPI (quad)
    // mode. clkdiv is SYS_CLK_FREQ / 100 MHz: 2.5 @ 250 MHz, 3.0 @ 300 MHz.
    //
    // QPI moves ~21.9 MB/s here against a 6.25 MB/s ceiling for single-bit SPI
    // at the same clock, so the protocol is worth far more than raising SCK.
    // 75 MHz quad does NOT work on this board — every read variant and dummy
    // count fails, with sampled nibbles coming back as the bitwise OR of two
    // consecutive true nibbles, which is analog margin (four lines switching
    // together, and SIO2/SIO3 on repurposed nunchuck pins), not a cycle count.
    // So do not raise this divisor without re-running the device tests; see
    // ROADMAP.md §3.
    //
    // The vendored library carries a PicoCalc fix in psram_spi.pio: upstream's
    // quad read loop has one turnaround clock too many at this speed and
    // returns data shifted left by exactly one nibble.
    //
    // The divisor holds its sampling phase across a change of SYS_CLK_FREQ only
    // because psram_spi.pio bypasses the PIO input synchronizer (the one
    // clk_sys-dependent term); what is left is fixed-ns pad/PCB/t_CO delay.
    // Lose that bypass and the phase moves with the system clock.
    static_assert(SYS_CLK_FREQ % (100 * MHZ) == 0, "pick a clkdiv for this clock");
    g_spi = psram_enter_qpi(pio1, (float)SYS_CLK_FREQ / (100 * MHZ));
    for (int i = 0; i < PRG_SLOTS; i++) { prg_cache_bank[i] = -1; prg_slot_stamp[i] = 0; }
    for (int i = 0; i < shapones::memory::PRGROM_REMAP_TABLE_SIZE; i++) prg_window_slot[i] = -1;
    prg_lru_clock = 0;
    return true;
}

// ── PRG bank-switch hook ──
// Fires synchronously inside prgrom_remap() after the mapper writes a new
// physical bank index.  We immediately load the bank into an SRAM slot and
// restore the remap_table entry to a slot-encoded value (≥ PRG_SLOT_OFFSET),
// so prgrom_read() never sees a raw physical index that would underflow the
// shifted prgrom pointer.
static void prg_bank_switch_hook(uint32_t cpu_block, uint32_t phys_block) {
    load_prg_bank((int)cpu_block, (uint16_t)phys_block);
}

// ── Self-test ──

// Report *how* a round trip failed, not just that it did. The shape is
// diagnostic: a constant nibble means a dead data line, a one-nibble offset
// means the quad read phase is wrong (see ROADMAP.md §3, which is exactly how
// that bug was found), and all-zeros or all-ones means no data came back at all.
static bool self_test_at(uint32_t addr, const uint8_t *pattern, uint8_t *buf) {
    psram_write_n(addr, pattern, 16);
    memset(buf, 0, 16);
    psram_read_n(addr, buf, 16);
    if (memcmp(pattern, buf, 16) == 0) return true;

    printf("psram_self_test: mismatch at 0x%06lX (mode: %s)\n",
           (unsigned long)addr, g_psram_mode);
    printf("  wrote:");
    for (int i = 0; i < 16; i++) printf(" %02X", pattern[i]);
    printf("\n  read :");
    for (int i = 0; i < 16; i++) printf(" %02X", buf[i]);
    printf("\n");

    // Nibble streams, to spot an offset.
    auto nib = [](const uint8_t *p, int i) -> int {
        return (i & 1) ? (p[i >> 1] & 0xF) : (p[i >> 1] >> 4);
    };
    for (int k = -2; k <= 2; k++) {
        if (k == 0) continue;
        bool match = true;
        for (int i = 2; i < 28 && match; i++) {
            int j = i + k;
            if (j < 0 || j >= 32) continue;
            if (nib(buf, i) != nib(pattern, j)) match = false;
        }
        if (match) { printf("  -> data is offset by %+d nibble(s)\n", k); break; }
    }
    // Stuck lines show as a nibble bit that never changes.
    uint8_t hi_and = 0xF, hi_or = 0, lo_and = 0xF, lo_or = 0;
    for (int i = 0; i < 16; i++) {
        hi_and &= buf[i] >> 4;  hi_or |= buf[i] >> 4;
        lo_and &= buf[i] & 0xF; lo_or |= buf[i] & 0xF;
    }
    printf("  nibble bits: hi always-set %X always-clear %X | lo always-set %X always-clear %X\n",
           hi_and, (uint8_t)(~hi_or & 0xF), lo_and, (uint8_t)(~lo_or & 0xF));
    return false;
}

const char *psram_mode_name() { return g_psram_mode; }

// Bulk read throughput, in the chunk size the active mode actually uses.
// The point is to tell QPI from SPI at a glance: single-bit cannot exceed
// 6.25 MB/s at 50 MHz SCK, so anything above that is QPI.
uint32_t psram_read_kbps() {
    static uint8_t tmp[1024];
    constexpr uint32_t BYTES = 64u * 1024u;
    uint32_t t0 = time_us_32();
    for (uint32_t off = 0; off < BYTES; off += sizeof(tmp))
        psram_read_n(off, tmp, sizeof(tmp));
    uint32_t us = time_us_32() - t0;
    if (us == 0) return 0;
    return (uint32_t)((uint64_t)BYTES * 1000000u / us / 1024u);
}

bool psram_self_test() {
    static uint8_t pattern[] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
    };
    uint8_t buf[16];
    if (!self_test_at(0, pattern, buf)) return false;
    return self_test_at(4096, pattern, buf);
}

// ── Load ROM ──

bool psram_load_nes(FIL *fil, uint32_t ines_size) {
    uint8_t header[16];
    UINT br;
    if (f_read(fil, header, 16, &br) != FR_OK || br != 16) return false;
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S') return false;

    uint32_t prg_pages     = header[4];
    uint32_t prg_phys_size = prg_pages * 16384;
    uint32_t chr_pages     = header[5];
    uint32_t chr_phys_size = chr_pages * 8192;

    g_chr_psram_base = prg_phys_size;
    g_chr_phys_size  = chr_phys_size;

    // Stream PRG + CHR from SD to PSRAM
    uint32_t psram_addr = 0;
    uint32_t remaining  = prg_phys_size + chr_phys_size;
    // Sized for the largest chunk either mode uses; psram_write_n() splits to
    // whatever the active mode actually allows.
    static constexpr UINT SD_CHUNK = 123;
    uint8_t chunk[SD_CHUNK];
    while (remaining > 0) {
        UINT to_read = (remaining > SD_CHUNK) ? SD_CHUNK : (UINT)remaining;
        if (f_read(fil, chunk, to_read, &br) != FR_OK || br != to_read) return false;
        psram_write_n(psram_addr, chunk, br);
        psram_addr += br;
        remaining  -= br;
    }

    // Initialise mapper from the header only.  prgrom/chrrom set by map_ines
    // will be overridden below; mapper init only writes remap_table entries.
    shapones::memory::map_ines(header);

    // ── PRG: redirect to 4-slot SRAM cache backed by PSRAM ──
    // prgrom[(PRG_SLOT_OFFSET+s)*8KB + off] == prg_sram[s*8KB + off]
    shapones::memory::prgrom =
        prg_sram - (size_t)PRG_SLOT_OFFSET * shapones::memory::PRGROM_BLOCK_SIZE;
    shapones::memory::prgrom_phys_addr_mask = PRG_ADDR_MASK;
    shapones::memory::prgrom_phys_size      = prg_phys_size;

    // ── CHR: pre-cache all CHR-ROM data into a malloc'd SRAM buffer ──
    // This eliminates the shifted-pointer slot trick that caused Core 1
    // BusFaults: any mapper-written physical bank index is now a valid direct
    // index into chr_full_cache, so chrrom_read() can never underflow.
    if (chr_phys_size > 0) {
        if (chr_full_cache) { free(chr_full_cache); chr_full_cache = nullptr; }
        chr_full_cache = (uint8_t *)malloc(chr_phys_size);
        if (!chr_full_cache) return false;
        psram_read_n(g_chr_psram_base, chr_full_cache, chr_phys_size);
        shapones::memory::chrrom                = chr_full_cache;
        // chrrom_phys_addr_mask and chrrom_phys_size are already set correctly
        // by map_ines() from the iNES header; no override needed.
    }
    // For CHR-RAM games (chr_phys_size == 0), map_ines() already allocated
    // chrram and set chrrom = chrram; nothing to do here.

    // Load initial PRG banks from PSRAM based on the mapper-set remap table
    for (int i = 0; i < shapones::memory::PRGROM_REMAP_TABLE_SIZE; i++) {
        load_prg_bank(i, shapones::memory::prgrom_remap_table[i]);
    }

    // Register synchronous bank-switch hook so mid-cpu::service() prgrom_remap()
    // calls never leave a raw physical index in the remap table.
    shapones::memory::prgrom_bank_switch_hook = prg_bank_switch_hook;

    psram_active = true;
    return true;
}
