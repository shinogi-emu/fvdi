#!/bin/sh
# run.sh NAME MACHINE MODEPRG|- DRIVER TEST.PRG TAG [auto|aes] [FILE...]
#
# Boot a disposable copy of a drive C template in Hatari, headless, run
# one guest test and print its result lines.
#
#   MACHINE   tt, falcon, ste (colour monitor) or stmono, all 68030/68882
#   MODEPRG   goes in as AUTO\00MODE.PRG, before fVDI (MODE2.PRG, MODE8.PRG,
#             FTCMODE.PRG), or - for the machine's boot resolution
#   DRIVER    bitplane.sys or 16_bit.sys: becomes FVDI.SYS's driver line
#   TEST.PRG  run from the desktop through --auto ("aes", the default), or
#             from AUTO after fVDI ("auto")
#   TAG       the test's name in "FVDI <TAG> GUEST PASS"
#   FILE...   copied into C:\VTEST\ beside the test
#
# Environment:
#   TOS        EmuTOS 1.4 512K image, e.g. etos512us.img (required)
#   BASE       drive C template (required): AUTO\FVDI.PRG, FVDI.SYS with
#              "booted", "cookie nvdi = $0501", a "01r ..." driver line and
#              "fonts = c:\gemsys\fonts" (CRLF), GEMSYS\BITPLANE.SYS,
#              GEMSYS\16_BIT.SYS and GEMSYS\FONTS\ with a TrueType font
#   HATARI     the hatari binary (default: hatari)
#   WORK       where runs are kept (default: ./hatari-runs)
#   FVDI_PRG   a FVDI.PRG to use instead of the template's
#   FVDI_DRV   a driver to use instead of the template's
#   TIMEOUT    seconds to wait for a result (default 300)
#   VBLS       emulated frames before Hatari gives up (default 200000)
#   CPULEVEL   Hatari --cpulevel, 3 (68030) by default; 0 gives a 68000, which
#              takes an address error at once on a word or long access at
#              an odd address
#   HATARI_ARGS  more Hatari options, after the machine's, which they
#              override (e.g. "--memsize 4 --ttram 0")
#
# BIOS device 1 output is caught with --conout 1 in WORK/run-NAME/conout.log.
# Hatari gets its own HOME there, so the user's Hatari settings and NVRAM
# are not touched.
set -u
: "${TOS:?set TOS to an EmuTOS 1.4 512K image}"
: "${BASE:?set BASE to a drive C template}"
H=${HATARI:-hatari}
WORK=${WORK:-$PWD/hatari-runs}
name=$1 machine=$2 mode=$3 driver=$4 prg=$5 tag=$6
shift 6
how=aes
case "${1:-}" in auto|aes) how=$1; shift;; esac
R=$WORK/run-$name
[ -e "$R" ] && { echo "$R exists; pick a new name" >&2; exit 2; }
mkdir -p "$R/home"
cp -a "$BASE" "$R/c"
[ "$mode" != - ] && cp "$mode" "$R/c/AUTO/00MODE.PRG"
sed -i "s/^01r .*\r\$/01r $driver\r/" "$R/c/FVDI.SYS"
[ -n "${FVDI_PRG:-}" ] && cp "$FVDI_PRG" "$R/c/AUTO/FVDI.PRG"
[ -n "${FVDI_DRV:-}" ] && cp "$FVDI_DRV" "$R/c/GEMSYS/$(echo "$driver" | tr a-z A-Z)"
base=$(basename "$prg" | tr a-z A-Z)
mkdir -p "$R/c/VTEST"
if [ "$how" = auto ]; then
    cp "$prg" "$R/c/AUTO/ZZ$base"
    autoarg=""
else
    cp "$prg" "$R/c/VTEST/$base"
    autoarg="--auto C:\\VTEST\\$base"
fi
for f in "$@"; do cp "$f" "$R/c/VTEST/"; done
case $machine in
    tt)     margs="--machine tt --monitor vga --memsize 14 --ttram 16";;
    falcon) margs="--machine falcon --monitor vga --memsize 14 --ttram 16";;
    ste)    margs="--machine ste --monitor rgb --memsize 14";;
    stmono) margs="--machine ste --monitor mono --memsize 14";;
    *) echo "machine: tt, falcon, ste or stmono" >&2; exit 2;;
esac
HOME=$R/home XDG_CONFIG_HOME=$R/home SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    stdbuf -o0 -e0 "$H" $margs ${HATARI_ARGS:-} --cpulevel ${CPULEVEL:-3} --fpu 68882 --tos "$TOS" \
    --cpu-exact false --compatible false --addr24 false --fast-forward true \
    --sound off --confirm-quit false --natfeats on --conout 1 \
    --gemdos-drive C -d "$R/c" $autoarg --run-vbls "${VBLS:-200000}" \
    > "$R/conout.log" 2> "$R/hatari.err" &
pid=$!
t=0
stop="FVDI $tag GUEST PASS|FVDI $tag GUEST SKIPPED|FVDI $tag FAIL"
while [ $t -lt "${TIMEOUT:-300}" ] && kill -0 $pid 2>/dev/null; do
    grep -a -q -E "$stop" "$R/conout.log" 2>/dev/null && break
    t=$((t + 2))
    sleep 2
done
sleep 2
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "== $name ($machine, $driver): ${t}s, $(grep -a -o 'VDI video mode = .*' "$R/hatari.err" | head -1)"
grep -a -E "^$tag|FVDI $tag|Panic|BENCH|VIDEO MODE" "$R/conout.log" | tr -d '\r'
