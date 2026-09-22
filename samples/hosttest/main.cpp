// Offline shapones harness: run a ROM headless and dump PPU state.
//
// Exists because diagnosing a rendering bug on the device costs a flash cycle
// per hypothesis. core/ is platform-independent, so the whole emulator runs
// here with a debugger attached and no hardware in the loop.
//
//   ./shapones-host rom.nes [frames] [out.ppm]
//
// Prints PPUCTRL/PPUMASK, the palette, a nametable occupancy map and the
// rendered frame's colour histogram, then writes the frame as a PPM.
//
// Single-threaded on purpose: cpu::service() and ppu::service() alternate in
// one loop, so there is no core-0/core-1 race here. A bug that reproduces is
// therefore an emulation bug, not a concurrency one.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "shapones/shapones.hpp"
#include "shapones/ppu.hpp"
#include "shapones/cpu.hpp"
#include "shapones/memory.hpp"
#include "shapones/region.hpp"
#include "shapones/input.hpp"

namespace shapones { void hosttest_set_ines(const uint8_t *p, size_t n); }
#ifdef SHAPONES_PPU_TRACE
extern "C" int ppu_trace_line;
#endif

using namespace shapones;

static std::vector<uint8_t> read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> d(n);
  if (fread(d.data(), 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(1); }
  fclose(f);
  return d;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s rom.nes [frames] [out.ppm] [start]\n", argv[0]); return 2; }
  const char *rom_path = argv[1];
  int frames = (argc > 2) ? atoi(argv[2]) : 120;
  const char *ppm_path = (argc > 3) ? argv[3] : nullptr;
  bool press_start = (argc > 4 && strcmp(argv[4], "start") == 0);

#ifdef SHAPONES_PPU_TRACE
  ppu_trace_line = 120;   // mid-playfield
#endif
  std::vector<uint8_t> rom = read_file(rom_path);
  printf("=== %s (%zu bytes) ===\n", rom_path, rom.size());
  printf("header region: %c\n", region_letter(detect_region(rom.data())));

  hosttest_set_ines(rom.data(), rom.size());

  config_t cfg = get_default_config();
  cfg.apu_sampling_rate = 22050;
  if (init(cfg) != result_t::SUCCESS) { fprintf(stderr, "init failed\n"); return 1; }
  if (memory::map_ines(rom.data()) != result_t::SUCCESS) {
    fprintf(stderr, "map_ines failed\n"); return 1;
  }
  reset();

  static uint8_t line[SCREEN_WIDTH + 1];
  static uint8_t frame[SCREEN_HEIGHT][SCREEN_WIDTH];
  int frame_count = 0;

  // Drive START periodically so menus advance: Tetris needs several presses to
  // get from the copyright screen through the title and menus into a game, and
  // the reported fault is during gameplay, not on a static screen.
  while (frame_count < frames) {
    if (press_start) {
      // Exactly four presses walks Tetris from the copyright screen through
      // the title, game-type and level menus into a game. Any press after that
      // toggles PAUSE, which legitimately blanks the playfield - an extra one
      // looks exactly like the fault being investigated.
      static const int PRESS_AT[] = { 60, 140, 220, 300, 380, 460 };
      input::status_t st;
      st.raw = 0;
      for (int p : PRESS_AT)
        if (frame_count >= p && frame_count < p + 8) st.start = 1;
      input::set_status(0, st);
    }
    cpu::service();
    ppu::status_t st;
    ppu::service(line, false, &st);
    if (st.is_end_of_visible_line() && st.focus_y >= 0 && st.focus_y < SCREEN_HEIGHT) {
      memcpy(frame[st.focus_y], line, SCREEN_WIDTH);
    }
    if (st.is_end_of_frame()) frame_count++;
  }

  printf("\n--- after %d frames ---\n", frames);
  printf("PPUCTRL ($2000) = 0x%02X   bg pattern table %s, sprite pattern table %s\n",
         ppu::debug_ppuctrl(),
         ppu::debug_ppuctrl() & 0x10 ? "$1000" : "$0000",
         ppu::debug_ppuctrl() & 0x08 ? "$1000" : "$0000");
  printf("PPUMASK ($2001) = 0x%02X   background %s, sprites %s\n",
         ppu::debug_ppumask(),
         ppu::debug_ppumask() & 0x08 ? "ON " : "OFF",
         ppu::debug_ppumask() & 0x10 ? "ON" : "OFF");

  printf("\npalette (background banks first):\n");
  for (int b = 0; b < 8; b++) {
    printf("  %s%d:", b < 4 ? "bg" : "sp", b & 3);
    for (int i = 0; i < 4; i++) printf(" %02X", ppu::debug_palette(b * 4 + i));
    printf("\n");
  }

  // Nametable occupancy: how many of the 32x30 tiles are non-zero. A blank
  // background with a populated nametable means we are not drawing what the
  // game wrote; an empty one means the game never wrote it.
  for (int nt = 0; nt < 2; nt++) {
    int nonzero = 0;
    addr_t base = 0x2000 + nt * 0x400;
    for (int i = 0; i < 32 * 30; i++)
      if (memory::vram_read(base + i) != 0) nonzero++;
    printf("nametable %d @0x%04X: %d / 960 tiles non-zero\n", nt, base, nonzero);
  }

  // CHR mapping: which physical 1 KB block each PPU block points at, and
  // whether that block has any non-zero bytes at all. A background that draws
  // as colour 0 everywhere means the pattern data is blank, not that rendering
  // is off.
  printf("\nCHR remap (PPU block -> phys block, and whether it has data):\n");
  for (int i = 0; i < memory::CHRROM_REMAP_TABLE_SIZE; i++) {
    uint32_t phys = memory::chrrom_remap_table[i];
    addr_t base = i << memory::CHRROM_BLOCK_ADDR_BITS;
    int nonzero = 0;
    for (int j = 0; j < (1 << memory::CHRROM_BLOCK_ADDR_BITS); j++)
      if (memory::chrrom_read(base + j) != 0) nonzero++;
    printf("  PPU $%04X -> phys %2u : %4d/%d bytes non-zero\n",
           (unsigned)base, (unsigned)phys, nonzero,
           1 << memory::CHRROM_BLOCK_ADDR_BITS);
  }

  // First rows of each nametable, as tile indices. Sensible structure means
  // the CPU wrote a real screen; garbage means it did not get that far.
  for (int nt = 0; nt < 2; nt++) {
    printf("\nnametable %d, rows 0-3 and 12-13:\n", nt);
    addr_t base = 0x2000 + nt * 0x400;
    for (int r : {0, 1, 2, 3, 12, 13}) {
      printf("  r%-2d:", r);
      for (int c = 0; c < 32; c++)
        printf(" %02X", memory::vram_read(base + r * 32 + c));
      printf("\n");
    }
  }

  // Pattern bytes for tiles the nametable actually uses, from both pattern
  // tables. An all-zero tile renders as colour 0 everywhere, i.e. black.
  {
    const int tiles[] = { 0x7A, 0x67, 0x3B, 0xEF, 0x15 };
    for (int t : tiles) {
      printf("tile %02X:", t);
      for (int half = 0; half < 2; half++) {
        addr_t base = half * 0x1000 + t * 16;
        int nz = 0;
        for (int b = 0; b < 16; b++) if (memory::chrrom_read(base + b)) nz++;
        printf("   $%04X:", (unsigned)base);
        for (int b = 0; b < 4; b++) printf(" %02X", memory::chrrom_read(base + b));
        printf(" (%2d/16 nz)", nz);
      }
      printf("\n");
    }
  }

  // What actually came out of the renderer.
  int hist[64] = {0};
  for (int y = 0; y < SCREEN_HEIGHT; y++)
    for (int x = 0; x < SCREEN_WIDTH; x++) hist[frame[y][x] & 63]++;
  printf("\nrendered frame, most common palette indices:\n");
  for (int n = 0; n < 5; n++) {
    int best = 0;
    for (int i = 1; i < 64; i++) if (hist[i] > hist[best]) best = i;
    if (hist[best] == 0) break;
    printf("  index %2d: %6d px (%.1f%%)\n", best, hist[best],
           100.0 * hist[best] / (SCREEN_WIDTH * SCREEN_HEIGHT));
    hist[best] = 0;
  }

  if (ppm_path) {
    FILE *f = fopen(ppm_path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    for (int y = 0; y < SCREEN_HEIGHT; y++)
      for (int x = 0; x < SCREEN_WIDTH; x++) {
        uint32_t c = NES_PALETTE_24BPP[frame[y][x] & 63];
        uint8_t px[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(px, 1, 3, f);
      }
    fclose(f);
    printf("\nwrote %s\n", ppm_path);
  }
  return 0;
}
