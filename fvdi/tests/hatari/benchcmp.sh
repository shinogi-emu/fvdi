#!/bin/sh
# benchcmp.sh BEFORE.log AFTER.log: VDIBENCH logs side by side.
# Ticks before -> after and the percentage; the screen CRCs must match.
awk '
    { sub(/\r$/, "") }
    FNR == NR {
        if ($1 == "VDIBENCH" && $3 ~ /^ticks=/) { t[$2] = substr($3, 7); c[$2] = substr($4, 5) }
        next
    }
    $1 == "VDIBENCH" && $3 ~ /^ticks=/ && ($2 in t) {
        a = substr($3, 7); pct = (t[$2] + 0) ? 100 * a / t[$2] : 0   # "0" as a string is true
        printf "%-20s %6d -> %6d  %5.1f%%  %s\n", $2, t[$2], a, pct, (c[$2] == substr($4, 5) ? "same pixels" : "PIXELS DIFFER")
    }' "$1" "$2"
