CC = cc
CFLAGS = -O2 -g -Wall -Wextra -Werror -I../include
TEST_BUILD := $(shell mktemp -d /tmp/fvdi-transfer.XXXXXX)
GUEST_CC ?= m68k-atari-mint-gcc
GUEST_CPU ?= 060
GUEST_CFLAGS ?= -m68$(GUEST_CPU) -O2 -Wall -Wextra -Werror -I../include
WRAP_ALLOC = -Wl,--wrap=malloc,--wrap=calloc,--wrap=free

.PHONY: check guest check-guest-log check-planar-log check-bench-log
check:
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		transfer.c indexed.c legacy.c ../engine/transfer.c ../engine/itable.c ../engine/ctab.c ../engine/legacy_raster.c ../engine/colour.c $(WRAP_ALLOC) -o $(TEST_BUILD)/transfer-test
	ASAN_OPTIONS=detect_leaks=0 $(TEST_BUILD)/transfer-test

guest:
	$(GUEST_CC) $(GUEST_CFLAGS) -DRASTER_TEST_REZ=1 video_mode_guest.c -o $(TEST_BUILD)/MODE2.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) -DRASTER_TEST_REZ=7 video_mode_guest.c -o $(TEST_BUILD)/MODE8.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) transfer_guest.c -lgem -o $(TEST_BUILD)/RASTER.PRG
	$(GUEST_CC) $(GUEST_CFLAGS) -DRASTER_BENCH_ONLY=1 transfer_guest.c -lgem -o $(TEST_BUILD)/BENCH.PRG
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
