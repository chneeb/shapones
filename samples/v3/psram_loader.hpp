#pragma once
#include "ff.h"
#include <stdbool.h>

// Call once from main() before boot_menu
bool psram_loader_init();

// Write a known pattern and read it back; returns true on match.
// Call after psram_loader_init() to verify PSRAM hardware is working.
bool psram_self_test();

// Call from load_nes() when ROM is too large for SRAM.
// fil must be open and positioned at byte 0.
bool psram_load_nes(FIL *fil, uint32_t ines_size);

// Call after each cpu::service() to sync PRG bank switches from PSRAM cache
void psram_sync_prg();

// Call once per frame at VBlank (safe window for Core1) to sync CHR
void psram_sync_chr();

extern bool psram_active;
