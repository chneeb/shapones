#!/usr/bin/env python3
"""Show — or strip — the region field of an iNES / NES 2.0 ROM header.

Mirrors core/include/shapones/region.hpp: only a NES 2.0 header carries
trustworthy region information. iNES 1.0 has a PAL bit at byte 9 bit 0, but
practically every dump leaves it clear whatever the game is, so the emulator
ignores it and reports UNKNOWN.

  ./ines_region.py rom.nes                  # report
  ./ines_region.py rom.nes --strip out.nes  # copy with the NES 2.0 marker cleared

--strip clears bits 3-2 of byte 7, which is the NES 2.0 identifier and nothing
else; the mapper's high nibble lives in bits 7-4 and is untouched. The copy is
byte-identical apart from that, so the emulator treats it as iNES 1.0, reports
'?' and paces it at 60 Hz. That isolates the region code path for a single ROM
without having to find a second dump whose PRG might also differ.
"""
import sys

REGIONS = {0: ("NTSC", "N"), 1: ("PAL", "P"), 2: ("MULTI", "M"), 3: ("DENDY", "D")}


def describe(h):
    if h[:4] != b"NES\x1a":
        return None, "not an iNES file (bad magic)"
    mapper = (h[7] & 0xF0) | (h[6] >> 4)
    prg, chr_ = h[4], h[5]
    lines = [
        f"mapper      : {mapper}",
        f"PRG ROM     : {prg} x 16 kB = {prg * 16} kB",
        f"CHR ROM     : {chr_} x 8 kB = {chr_ * 8} kB"
        + ("  (CHR RAM)" if chr_ == 0 else ""),
        f"byte 7      : 0x{h[7]:02X}",
    ]
    if (h[7] & 0x0C) == 0x08:
        name, letter = REGIONS[h[12] & 3]
        lines.append(f"header      : NES 2.0")
        lines.append(f"byte 12     : 0x{h[12]:02X}")
        lines.append(f"region      : {name}  (menu shows '{letter}')")
    else:
        lines.append("header      : iNES 1.0")
        lines.append("region      : UNKNOWN  (menu shows '?', paced as NTSC)")
    return h, "\n".join(lines)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 16:
        print(f"{path}: too short to hold a header", file=sys.stderr)
        return 1

    h, text = describe(data[:16])
    print(f"{path}:")
    print(text)
    if h is None:
        return 1

    if "--strip" in argv:
        i = argv.index("--strip")
        if i + 1 >= len(argv):
            print("--strip needs an output filename", file=sys.stderr)
            return 2
        out = argv[i + 1]
        if (data[7] & 0x0C) != 0x08:
            print("\nalready iNES 1.0 - nothing to strip")
            return 0
        before = data[7]
        data[7] &= ~0x0C
        with open(out, "wb") as f:
            f.write(data)
        print(f"\nwrote {out}")
        print(f"  byte 7: 0x{before:02X} -> 0x{data[7]:02X}")
        print("  region is now UNKNOWN; everything else is unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
