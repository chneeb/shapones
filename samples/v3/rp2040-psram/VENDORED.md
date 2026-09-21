# Vendored copy of polpo/rp2040-psram

Upstream: <https://github.com/polpo/rp2040-psram>
Vendored at commit `419375d5af2af2c72af63cdab6c6df2c558a3a9c`
("Add PSRAM_DEBUG define to enable debug printing"), which was `origin/main`
at the time — this is upstream's tip, not an older pin.

This was a git submodule until 2026-09-21. It became a checked-in copy because
the PicoCalc needs changes that do not exist upstream and cannot be carried as
a submodule pin:

- **QPI (quad) support** is an unmerged pull request,
  [#15 "Initial QSPI(QPI) support"](https://github.com/polpo/rp2040-psram/pull/15),
  open since 2025-08-18.
- **The quad read section needs a PicoCalc-specific fix** on top of that PR: its
  read loop carries one turnaround clock too many at 50 MHz SCK, so reads come
  back shifted by exactly one nibble. See `ROADMAP.md` section 3.

Keeping the directory path unchanged means `samples/v3/CMakeLists.txt` and every
reference in `CLAUDE.md` and `ROADMAP.md` still resolve.

**Local changes are listed here as they land.** Right now there are none: this
is byte-for-byte upstream `419375d`, so the switch from submodule to vendored
copy is functionally a no-op and can be verified by diffing against upstream.
