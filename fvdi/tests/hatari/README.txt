Guest tests under Hatari
========================

run.sh boots a disposable copy of a drive C template in Hatari, headless,
runs one guest test and prints its result lines.  benchcmp.sh compares two
VDIBENCH logs.  Both are plain sh; see the comments at the top of each.

What is needed
--------------
- Hatari (2.5 or later) and EmuTOS 1.4's 512K image (etos512us.img from
  emutos-512k-1.4.zip).
- The guest programs: make guest GUEST_CPU=020-60 (020-60 runs on the
  68030 the machines below are given).
- A drive C template, BASE:

    AUTO\FVDI.PRG
    FVDI.SYS             booted
                         cookie nvdi = $0501
                         01r bitplane.sys
                         fonts = c:\gemsys\fonts
                         (CRLF line ends; run.sh rewrites the driver line)
    GEMSYS\BITPLANE.SYS
    GEMSYS\16_BIT.SYS
    GEMSYS\FONTS\...     at least one TrueType font, for OUTLINE, REALEXT
                         and VDIBENCH's outline text

Screens
-------
  machine  mode program        driver         screen
  stmono   -                   bitplane.sys   640x400, 1 plane
  ste      MODE2.PRG           bitplane.sys   640x200, 2 planes
  ste      -                   bitplane.sys   320x200, 4 planes
  tt       -                   bitplane.sys   640x480, 4 planes
  tt       MODE8.PRG           bitplane.sys   320x480, 8 planes
  falcon   -                   bitplane.sys   640x480, 4 planes
  falcon   FTCMODE.PRG         16_bit.sys     320x480, 16 bits

Example:

    TOS=etos512us.img BASE=hatari-c HATARI=hatari \
        tests/hatari/run.sh tt8-colors tt MODE8.PRG bitplane.sys COLORS.PRG COLORS

RASTER.PRG runs from AUTO ("auto" after the tag): it checks for a
planar screen itself and then needs check-planar-log.  PENS.PRG says
SKIPPED above 8 planes, where there is no colour table to check pens
against.

Timings
-------
With --fast-forward Hatari's emulated 200 Hz timer is unaffected by how
fast the host is, and VDIBENCH's ticks repeat exactly from run to run.
Compare drivers on the same machine and screen only, and expect every
CRC to match unless a change is meant to alter pixels.
