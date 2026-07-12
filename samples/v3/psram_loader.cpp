#include "psram_loader.hpp"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"

#include "psram_spi.h"
#include "shapones/memory.hpp"
#include "common.hpp"  // SYS_CLK_FREQ, MHZ

// PSRAM pin assignments (GP20=CS, GP21=SCK, GP2=MOSI, GP3=MISO via PIO1)

// PRG slot-index offset: values written into prgrom_remap_table by us are
// always ≥ PRG_SLOT_OFFSET, while mapper-written physical bank indices are
// always < PRG_SLOT_OFFSET (max PRG bank for mappers 0–4 = 63, i.e. 512 KB).
static constexpr int PRG_SLOT_OFFSET = 64;

// PRG SRAM cache: 8 × 8 KB = 64 KB. There are only 4 CPU PRG windows, so the
// extra 4 slots act as a victim cache: when a window switches back to a bank
// that's still resident, it's a free hit with no PSRAM read. This is the main
// lever on MMC3 frame rate — every miss is a slow ~8 KB PSRAM read on Core 0,
// and the PPU is throttled to CPU progress, so misses directly cost fps.
static constexpr int PRG_SLOTS = 8;

// Address mask large enough to cover all slot-encoded PRG indices.
// Slot (64+7) * 8192 + 8191 = 589823 < 0x100000.
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

// ── small-chunk helpers (uint8_t bit-count field limits: ≤31 read, ≤27 write) ──

static constexpr int RD_CHUNK = 31;
static constexpr int WR_CHUNK = 27;

static void psram_read_n(uint32_t addr, uint8_t *dst, size_t count) {
    while (count > 0) {
        size_t n = (count > RD_CHUNK) ? RD_CHUNK : count;
        psram_read(&g_spi, addr, dst, n);
        addr  += n;
        dst   += n;
        count -= n;
    }
}

static void psram_write_n(uint32_t addr, const uint8_t *src, size_t count) {
    while (count > 0) {
        size_t n = (count > WR_CHUNK) ? WR_CHUNK : count;
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
    // PSRAM reliability at these clocks is a sampling-PHASE problem, not a raw
    // speed one: a timing sweep showed pass/fail oscillating with clkdiv,
    // failing at both fast and slow ends. The known-good operating point is a
    // ~100 MHz state-machine clock (SCK ~50 MHz) with the non-fudge PIO program,
    // so clkdiv is SYS_CLK_FREQ / 100 MHz: 2.5 @ 250 MHz, 3.0 @ 300 MHz. If you
    // change SYS_CLK_FREQ, keep SM ≈ 100 MHz and re-verify with a full bulk
    // read/verify sweep — a 16-byte round-trip is too weak to catch marginal
    // timing.
    static_assert(SYS_CLK_FREQ % (100 * MHZ) == 0, "pick a clkdiv for this clock");
    g_spi = psram_spi_init_clkdiv(pio1, -1, (float)SYS_CLK_FREQ / (100 * MHZ), false);
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

bool psram_self_test() {
    static uint8_t pattern[] = {
        0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
    };
    uint8_t buf[16];
    psram_write_n(0, pattern, 16);
    memset(buf, 0, 16);
    psram_read_n(0, buf, 16);
    if (memcmp(pattern, buf, 16) != 0) return false;

    psram_write_n(4096, pattern, 16);
    memset(buf, 0, 16);
    psram_read_n(4096, buf, 16);
    return memcmp(pattern, buf, 16) == 0;
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
    uint8_t chunk[WR_CHUNK];
    while (remaining > 0) {
        UINT to_read = (remaining > WR_CHUNK) ? WR_CHUNK : (UINT)remaining;
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
