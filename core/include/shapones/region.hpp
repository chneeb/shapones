#ifndef SHAPONES_REGION_HPP
#define SHAPONES_REGION_HPP

#include "shapones/cpu.hpp"

namespace shapones {

enum class region_t : uint8_t {
  UNKNOWN = 0,  // iNES 1.0 header: no trustworthy region information
  NTSC,
  PAL,
  MULTI,  // declared as working on both
  DENDY,
};

// Region from the 16-byte iNES header.
//
// Deliberately trusts ONLY a NES 2.0 header. iNES 1.0 does have a PAL bit at
// byte 9 bit 0, but practically every dump in circulation leaves it clear
// whatever the game is, so reading it mislabels PAL ROMs as NTSC far more often
// than it helps. UNKNOWN is the honest answer and the safe one: nothing changes
// behaviour on it.
static inline region_t detect_region(const uint8_t *header) {
  // NES 2.0 identifies itself in byte 7, bits 3-2 == 0b10.
  if ((header[7] & 0x0C) != 0x08) return region_t::UNKNOWN;
  switch (header[12] & 0x03) {
    case 0: return region_t::NTSC;
    case 1: return region_t::PAL;
    case 2: return region_t::MULTI;
    default: return region_t::DENDY;
  }
}

// One letter for a ROM listing. '?' rather than 'N' for unknown is the whole
// point: most dumps are iNES 1.0, and calling those NTSC puts a confident label
// on exactly the case worth seeing.
static inline char region_letter(region_t r) {
  switch (r) {
    case region_t::NTSC: return 'N';
    case region_t::PAL: return 'P';
    case region_t::MULTI: return 'M';
    case region_t::DENDY: return 'D';
    default: return '?';
  }
}

// CPU clock, which sets the APU timer steps. MULTI and UNKNOWN take NTSC:
// changing nothing is the safe default.
static inline uint32_t region_cpu_clock_hz(region_t r) {
  switch (r) {
    case region_t::PAL: return cpu::CLOCK_FREQ_PAL;
    case region_t::DENDY: return cpu::CLOCK_FREQ_DENDY;
    default: return cpu::CLOCK_FREQ_NTSC;
  }
}

// Frame period in microseconds, for a host that paces frames itself.
// 50.007 Hz against 60.098 Hz.
static inline uint32_t region_frame_period_us(region_t r) {
  switch (r) {
    case region_t::PAL:
    case region_t::DENDY: return 19997;
    default: return 16666;
  }
}

}  // namespace shapones

#endif
