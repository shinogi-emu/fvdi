CC = cc
CFLAGS = -O2 -g -Wall -Wextra -Werror -I../include
ifndef TEST_BUILD
TEST_BUILD := $(shell mktemp -d /tmp/fvdi-transfer.XXXXXX)
endif
GUEST_CC ?= m68k-atari-mint-gcc
GUEST_CPU ?= 060
GUEST_CFLAGS ?= -m68$(GUEST_CPU) -O2 -Wall -Wextra -Werror -I../include
WRAP_ALLOC = -Wl,--wrap=malloc,--wrap=calloc,--wrap=free

.PHONY: check noint guest check-guest-log check-planar-log check-bench-log check-fontscan-log check-colors-log check-bitmaps-log check-realext-log check-nvdimisc-log check-outline-log check-nvditext-log check-vdibench-log check-pens-log check-blitedge-log check-outledge-log check-fontbusy-log check-edges-log check-nvdicol-log check-scrninfo-log check-bmrgb-log check-xfer-log check-ctab-log check-dither-log check-bezier-log check-aatext-log check-alpha-log
check: noint
	$(CC) $(CFLAGS) -I../drivers/include -fsanitize=address,undefined -fno-omit-frame-pointer \
		transfer.c indexed.c legacy.c alpha.c ../engine/transfer.c ../engine/itable.c ../engine/ctab.c ../engine/legacy_raster.c ../engine/colour.c ../engine/default.c ../drivers/16_bit/16b_blend.c $(WRAP_ALLOC) -o $(TEST_BUILD)/transfer-test
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BUILD)/transfer-test

# blend.h and the 16-bit driver's blend are built with 16-bit ints in the
# drivers: they must not use a plain int.
NOINT = ../include/blend.h ../drivers/16_bit/16b_blend.c
noint:
	@for f in $(NOINT); do test -r $$f || exit 1; done
	@if grep -nE '(^|[^A-Za-z0-9_])(unsigned[[:space:]]+)?int([^A-Za-z0-9_]|$$)' $(NOINT); then \
	    echo "plain int in $(NOINT)"; exit 1; fi

guest:
	$(GUEST_CC) $(GUEST_CFLAGS) -DRASTER_TEST_REZ=1 video_mode_guest.c -o $(TEST_BUILD)/MODE2.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) -DRASTER_TEST_REZ=7 video_mode_guest.c -o $(TEST_BUILD)/MODE8.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) transfer_guest.c -lgem -o $(TEST_BUILD)/RASTER.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) -DRASTER_BENCH_ONLY=1 transfer_guest.c -lgem -o $(TEST_BUILD)/BENCH.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) fontscan_guest.c -lgem -o $(TEST_BUILD)/FONTSCAN.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) colors_guest.c -lgem -o $(TEST_BUILD)/COLORS.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) bitmaps_guest.c -lgem -o $(TEST_BUILD)/BITMAPS.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) realext_guest.c -lgem -o $(TEST_BUILD)/REALEXT.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) nvdimisc_guest.c -lgem -o $(TEST_BUILD)/NVDIMISC.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) outline_guest.c -lgem -o $(TEST_BUILD)/OUTLINE.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) nvditext_guest.c -lgem -o $(TEST_BUILD)/NVDITEXT.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) vdibench_guest.c -lgem -o $(TEST_BUILD)/VDIBENCH.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) -DVDIBENCH_NOBLIT=1 vdibench_guest.c -lgem -o $(TEST_BUILD)/VDIBENCB.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) pens_guest.c -lgem -o $(TEST_BUILD)/PENS.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) blitedge_guest.c -lgem -o $(TEST_BUILD)/BLITEDGE.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) outledge_guest.c -lgem -o $(TEST_BUILD)/OUTLEDGE.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) edges_guest.c -lgem -o $(TEST_BUILD)/EDGES.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) nvdicol_guest.c -lgem -o $(TEST_BUILD)/NVDICOL.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) scrninfo_guest.c -lgem -o $(TEST_BUILD)/SCRNINFO.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) bmrgb_guest.c -lgem -o $(TEST_BUILD)/BMRGB.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) xfer_guest.c -lgem -o $(TEST_BUILD)/XFER.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) ctab_guest.c -lgem -o $(TEST_BUILD)/CTAB.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) dither_guest.c -lgem -o $(TEST_BUILD)/DITHER.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) bezier_guest.c -lgem -o $(TEST_BUILD)/BEZIER.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) aatext_guest.c -lgem -o $(TEST_BUILD)/AATEXT.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) alpha_guest.c -lgem -o $(TEST_BUILD)/ALPHA.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) twice_guest.c -o $(TEST_BUILD)/TWICE.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) fontbusy_guest.c -lgem -o $(TEST_BUILD)/FONTBUSY.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) ftcmode_guest.c -o $(TEST_BUILD)/FTCMODE.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) -I../drivers/include transfer.c indexed.c legacy.c alpha.c ../engine/transfer.c ../engine/itable.c ../engine/ctab.c ../engine/legacy_raster.c ../engine/colour.c ../engine/default.c ../drivers/16_bit/16b_blend.c $(WRAP_ALLOC) -o $(TEST_BUILD)/REFCHECK.PRG

# A surviving emulator / successful screenshot is not a passing raster test.
check-guest-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI TRANSFER FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'TRANSFER AES application registered' "$(GUEST_LOG)"
	grep -Fq 'FVDI INDEXED GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI REMAP GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI CTAB GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI DIRECT SOURCE GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI LEGACY GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI TRANSFER GUEST PASS' "$(GUEST_LOG)"

check-planar-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI TRANSFER FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI DIRECT SOURCE GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI PLANAR GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI LEGACY GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'FVDI TRANSFER GUEST PASS' "$(GUEST_LOG)"
	grep -Fq 'PLANAR SCREEN BENCH depth=' "$(GUEST_LOG)"
	if test -n "$(GUEST_PLANES)"; then grep -Fq 'PLANAR SCREEN BENCH depth=$(GUEST_PLANES) ' "$(GUEST_LOG)"; fi

check-bench-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI TRANSFER FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'RASTER MEMORY BENCH PASS' "$(GUEST_LOG)"
	test "$$(grep -c 'TRANSFER BENCH' "$(GUEST_LOG)")" -eq 9

# FONTSCAN.PRG: a font added at run time through the NVDI cookie's
# scan_fonts/get_font_dir, as HighWire adds web fonts.  See the top of
# fontscan_guest.c for the files it expects on C:.
check-fontscan-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI FONTSCAN FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI FONTSCAN GUEST PASS' "$(GUEST_LOG)"

# COLORS.PRG: NVDI's highlight/min/max/weight colours and the raster
# foreground/background colours.
check-colors-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI COLORS FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI COLORS GUEST PASS' "$(GUEST_LOG)"

# BITMAPS.PRG: off-screen bitmaps through v_opnbm, v_open_bm and
# v_resize_bm.  Needs a 16-bit or deeper screen.
check-bitmaps-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI BITMAPS FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI BITMAPS GUEST PASS' "$(GUEST_LOG)"

# REALEXT.PRG: vqt_real_extent against vqt_f_extent and vqt_fontinfo, in
# the system font and an outline font; vq_ext_devinfo for the screen.
check-realext-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI REALEXT FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI REALEXT GUEST PASS' "$(GUEST_LOG)"

# NVDIMISC.PRG: v_setrgb, vr_clip_rects (16 and 32 bit), v_bez_qual, and
# the colour/pixel-value conversions checked against a drawn pixel.
check-nvdimisc-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI NVDIMISC FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI NVDIMISC GUEST PASS' "$(GUEST_LOG)"

# OUTLINE.PRG: v_getoutline, v_get_outline and vst_width; the outline of an
# 'O' filled with v_bez_fill into a bitmap.  Needs a 16-bit or deeper
# screen for the fill, and an outline font.
check-outline-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI OUTLINE FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI OUTLINE GUEST PASS' "$(GUEST_LOG)"

# NVDITEXT.PRG: the NVDI text calls a web browser uses on an outline font -
# arbitrary sizes, Unicode text, v_ftext and its offsets, advances, widths,
# skew, kerning, character indices, the font header and lookups by id.
check-nvditext-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI NVDITEXT FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI NVDITEXT GUEST PASS' "$(GUEST_LOG)"

# VDIBENCH.PRG: lines, text, blits and fills timed with the 200 Hz timer,
# each followed by a CRC-32 of the screen.  Compare two drivers by their
# logs: the ticks may differ, the CRCs must not.
check-vdibench-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI VDIBENCH FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI VDIBENCH GUEST PASS' "$(GUEST_LOG)"
	test "$$(grep -c '^VDIBENCH [a-z-]* ticks=' "$(GUEST_LOG)")" -ge 15

# PENS.PRG: text, vrt_cpyfm, fills and lines in each pen read back as
# that pen.  Palette screens (8 planes or fewer); SKIPPED above.
check-pens-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI PENS FAIL|Panic' "$(GUEST_LOG)"
	grep -Eq 'FVDI PENS GUEST (PASS|SKIPPED)' "$(GUEST_LOG)"

# BLITEDGE.PRG: rectangle fills and copies, text and Line-A drawing at
# every word offset and at the edges, and the cases of old fVDI bugs
# (copies inside one word, overlapping copies, vrt_cpyfm sources that
# must draw nothing).  Prints one "BLITEDGE <case> crc=<hex>" line per
# drawing case: compare two builds by those lines.
check-blitedge-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI BLITEDGE FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI BLITEDGE GUEST PASS' "$(GUEST_LOG)"
	test "$$(grep -c '^BLITEDGE [a-z0-9-]* crc=' "$(GUEST_LOG)")" -ge 20

# OUTLEDGE.PRG: outline text through the plain VDI calls - fonts that will
# not load, clipping at the screen edges and to a rectangle, rotation,
# writing modes and effects - compared within the run.  Started through
# TWICE.PRG, it runs a second time after the first has ended, and then
# both runs must pass.  See the top of outledge_guest.c.
check-outledge-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI OUTLEDGE FAIL|FVDI TWICE FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI OUTLEDGE GUEST PASS' "$(GUEST_LOG)"
	if grep -Fq 'TWICE run 1' "$(GUEST_LOG)"; then \
		test "$$(grep -c 'FVDI OUTLEDGE GUEST PASS' "$(GUEST_LOG)")" -eq 2 && \
		grep -Fq 'FVDI TWICE GUEST PASS' "$(GUEST_LOG)"; fi

# FONTBUSY.PRG: two programs at once (under MiNT) size outline faces while
# the other keeps the font engine busy; both must keep every face.  See
# the top of fontbusy_guest.c.
check-fontbusy-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI FONTBUSY FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI FONTBUSY GUEST PASS' "$(GUEST_LOG)"
	test "$$(grep -c 'FVDI FONTBUSY CHILD PASS' "$(GUEST_LOG)")" -eq 2

# EDGES.PRG: v_cellarray and v_contourfill return, vq_chcells fits the
# screen, v_curleft moves one column left.
check-edges-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI EDGES FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI EDGES GUEST PASS' "$(GUEST_LOG)"

# NVDICOL.PRG: RGB foreground and background colours as drawn (fill, text,
# lines) into a memory form, in device format; vsf_color after an RGB colour.
check-nvdicol-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI NVDICOL FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI NVDICOL GUEST PASS' "$(GUEST_LOG)"

# SCRNINFO.PRG: vq_scrninfo on a 16-bit screen as fVDI's 16_bit.sys answers,
# and a round trip of pixels built from the described bit positions.
check-scrninfo-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI SCRNINFO FAIL|Panic' "$(GUEST_LOG)"
	grep -Eq 'FVDI SCRNINFO GUEST (PASS|SKIPPED)' "$(GUEST_LOG)"

# BMRGB.PRG: RGB fill, text and line backgrounds in replace mode drawn on a
# 16-bit screen and into two v_open_bm bitmaps (one in the program's memory
# at 2 mod 4 with lines of 2 mod 4 bytes): expected colours, and the three
# pictures equal pixel for pixel.  Skipped on screens with planes.
check-bmrgb-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI BMRGB FAIL|Panic' "$(GUEST_LOG)"
	grep -Eq 'FVDI BMRGB GUEST (PASS|SKIPPED)' "$(GUEST_LOG)"

# XFER.PRG: vr_transfer_bits (direct, indexed and 1-bit sources, into memory,
# onto the screen with a clip and into a bitmap workstation), its refusals and
# vro_cpyfm / vrt_cpyfm with bit 15 of the mode against a reference.
check-xfer-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI XFER FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI XFER GUEST PASS' "$(GUEST_LOG)"

# CTAB.PRG: the colour tables (vs_ctab, vq_ctab and the rest of 205/206) and
# inverse tables (v_create_itab, v_delete_itab) of EmuTOS's VDI: the table
# by pixel value, ids, defaults, tables made and deleted, refusals, and no
# memory left behind by closed workstations.
check-ctab-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI CTAB FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI CTAB GUEST PASS' "$(GUEST_LOG)"

# DITHER.PRG: vr_transfer_bits with error diffusion (mode 128) and onto
# 8-bit indices: against RGB32 references, palettes and inverse tables,
# clipped dithering onto the screen against the whole rectangle in memory.
check-dither-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI DITHER FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI DITHER GUEST PASS' "$(GUEST_LOG)"

# BEZIER.PRG: v_bez, v_bez_fill, v_bez_on/off and v_bez_qual: the points of
# fVDI's subdivision (checked against the cubic), the pixels of v_pline and
# v_fillarea of them, moves, clipping, quality and limits.
check-bezier-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI BEZIER FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI BEZIER GUEST PASS' "$(GUEST_LOG)"

# AATEXT.PRG: anti-aliased outline text on a 16-bit screen, one-bit text on
# fewer planes.  See the top of aatext_guest.c.
check-aatext-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI AATEXT FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI AATEXT GUEST PASS' "$(GUEST_LOG)"

# ALPHA.PRG: vr_transfer_bits with T_BLEND (64), ARGB and RGBA images with
# straight alpha blended onto 16-bit pixels, against alpha_vectors.h.  Where
# bit 3 of work_out[30] is clear only the refusals run and it ends SKIPPED.
# ALPHA_EXPECT=PASS (or SKIPPED) asks for that ending only.
check-alpha-log:
	test -n "$(GUEST_LOG)"
	! grep -aEq 'ALPHA FAIL|Panic' "$(GUEST_LOG)"
	grep -aEq 'FVDI ALPHA GUEST (PASS|SKIPPED)' "$(GUEST_LOG)"
	if test -n "$(ALPHA_EXPECT)"; then grep -aFq 'FVDI ALPHA GUEST $(ALPHA_EXPECT)' "$(GUEST_LOG)"; fi
