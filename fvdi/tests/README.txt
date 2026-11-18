Raster transfer tests
=====================

Host checks and guest programs for the raster conversion work described in
../docs/raster.txt.

    make -C fvdi/tests check
    make -C fvdi/tests guest GUEST_CPU=060

"check" runs the host references under ASan/UBSan. Guards cover pixel
storage, staging, clipping, shadows, small blocks and odd byte storage.
Allocator wrappers verify balance; LeakSanitizer is disabled for sandbox
compatibility. Make prints the retained temporary test directory.
alpha.c checks T_BLEND against alpha_vectors.h, the reference vectors
shared with EmuTOS's VDI (written by Shinogi's tests/vdi-nvdi generator,
committed byte for byte; do not edit), in memory and on the fake screen of
transfer.c, whose blit serves T_BLEND's read-backs and checks that each is
written back as the same rectangle. "check" also fails on a plain int in
include/blend.h, which the 16-bit-int drivers include.

REFCHECK.PRG runs these references with the m68k data model, with serial
progress/assertions. RASTER.PRG tests real VDI traps in a disposable boot:
physical workstation before AES, or --aes to register, open a virtual
workstation, lock screen updates and hide the cursor. It overwrites screen
areas and releases resources on exit. Legacy buffers have allocation guards.
GEMDOS allocation probes after planar phases catch corruption invisible in
pixel comparisons.

ALPHA.PRG checks vr_transfer_bits with T_BLEND through the VDI trap
against the same alpha_vectors.h: the vectors' transfers from ARGB and RGBA
into memory in the screen's format, onto the screen with clipping and into
a v_open_bm bitmap, the weight colour of each workstation, modes 0 and 32
of the two formats, and the refusals; then it times a 48x48 icon against
the mask path ("ALPHA TIME" lines). It runs under the AES, in any VDI.
Where bit 3 of work_out[30] is clear (8 planes or fewer, or a VDI without
the blend) only the refusals run and it ends SKIPPED. Validate with
check-alpha-log GUEST_LOG=...; add ALPHA_EXPECT=PASS where the blend must
be there.


Disposable installation
-----------------------

For Shinogi, copy the normal installation to a temporary tree. Replace only
its AUTO/FVDI.PRG, GEMSYS/16_BIT.SYS and root RASTER.PRG. Keep
AUTO/060SP.PRG, MiNT, fonts and desktop settings. Add

    run c:\RASTER.PRG --aes

to its MINT/1-19-CUR/XAAES/XAAES.CNF. Use Shinogi's tools/run-stack.sh with
KEEP_TREE=1, temporary STACK_TREE/STACK_OUT, SHINOGI_CPU=m68060,
SHINOGI_ELF=.../emutos-virt-060.elf, RES=1280x720, NET=1, BOOT_WAIT=60.
Validate with:

    make -C fvdi/tests check-guest-log GUEST_LOG=/absolute/path/to/serial.log

Use the matching current RGB16 driver: an older copy predating its
full-colour component fix fails the CTAB pixel-value assertion. The normal
060 compatibility handler is required. Run REFCHECK in a separate AUTO boot:
the observed setup did not resume AUTO scanning after REFCHECK exited.

For planar tests build "guest GUEST_CPU=020". In a disposable Hatari tree
put FVDI.PRG then RASTER.PRG in AUTO, BITPLANE.SYS in GEMSYS, and
configure:

    booted
    cookie nvdi = $0501
    01r BITPLANE.SYS

Use an EmuTOS 1.4 512K ROM and Hatari

    --cpulevel 3 --fpu 68882 --cpu-exact false --compatible false
    --addr24 false --memsize 14 --fast-forward true --sound off
    --rs232-in /dev/null --rs232-out /temporary/serial.log

The test runtime needs the FPU setting; raster code is integer-only. ST
high/medium/low give 1/2/4 planes; TT low gives 8. For two/eight planes,
install the built MODE2.PRG/MODE8.PRG as AUTO/00MODE.PRG BEFORE fVDI. Use an
ST/STE colour monitor for MODE2, TT with a VGA monitor for MODE8. These
setup programs change resolution and are for disposable boots only.
Hatari's desktop --tos-res setting alone did not change the mode seen by
the AUTO tests. Require the actual "PLANAR SCREEN BENCH depth=N" log entry.
For one plane, use a monochrome monitor without a mode-setup program; for
four planes, use ST/STE low resolution without one. Allow enough VBLs for
the benchmark or stop on its final marker. Validate with check-planar-log.


Benchmark
---------

BENCH.PRG produces the memory-to-memory timings in ../docs/raster.txt.
Validate all nine samples with check-bench-log.
