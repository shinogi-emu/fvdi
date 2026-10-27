CC = cc
CFLAGS = -O2 -g -Wall -Wextra -Werror -I../include
TEST_BUILD := $(shell mktemp -d /tmp/fvdi-transfer.XXXXXX)
GUEST_CC ?= m68k-atari-mint-gcc
GUEST_CPU ?= 060
GUEST_CFLAGS ?= -m68$(GUEST_CPU) -O2 -Wall -Wextra -Werror -I../include
WRAP_ALLOC = -Wl,--wrap=malloc,--wrap=calloc,--wrap=free

.PHONY: check guest check-guest-log check-planar-log check-bench-log check-fontscan-log check-colors-log check-bitmaps-log check-realext-log check-nvdimisc-log check-outline-log check-vdibench-log
check:
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		transfer.c indexed.c legacy.c ../engine/transfer.c ../engine/itable.c ../engine/ctab.c ../engine/legacy_raster.c ../engine/colour.c $(WRAP_ALLOC) -o $(TEST_BUILD)/transfer-test
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BUILD)/transfer-test

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
	$(GUEST_CC) $(GUEST_CFLAGS) vdibench_guest.c -lgem -o $(TEST_BUILD)/VDIBENCH.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) transfer.c indexed.c legacy.c ../engine/transfer.c ../engine/itable.c ../engine/ctab.c ../engine/legacy_raster.c ../engine/colour.c $(WRAP_ALLOC) -o $(TEST_BUILD)/REFCHECK.PRG

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

# VDIBENCH.PRG: lines, text, blits and fills timed with the 200 Hz timer,
# each followed by a CRC-32 of the screen.  Compare two drivers by their
# logs: the ticks may differ, the CRCs must not.
check-vdibench-log:
	test -n "$(GUEST_LOG)"
	! grep -Eq 'FVDI VDIBENCH FAIL|Panic' "$(GUEST_LOG)"
	grep -Fq 'FVDI VDIBENCH GUEST PASS' "$(GUEST_LOG)"
	test "$$(grep -c '^VDIBENCH [a-z-]* ticks=' "$(GUEST_LOG)")" -ge 15
