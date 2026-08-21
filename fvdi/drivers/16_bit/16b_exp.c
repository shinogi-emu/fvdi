/* 
 * A 16 bit graphics mono-expand routine, by Johan Klockars.
 *
 * This file is an example of how to write an
 * fVDI device driver routine in C.
 *
 * You are encouraged to use this file as a starting point
 * for other accelerated features, or even for supporting
 * other graphics modes. This file is therefore put in the
 * public domain. It's not copyrighted or under any sort
 * of license.
 */

#include "fvdi.h"
#include "driver.h"
#include "../bitplane/bitplane.h"

#define PIXEL		short
#define PIXEL_SIZE	sizeof(PIXEL)


/*
 * Make it as easy as possible for the C compiler.
 * The current code is written to produce reasonable results with Lattice C.
 * (long integers, optimize: [x xx] time)
 * - One function for each operation -> more free registers
 * - 'int' is the default type
 * - some compilers aren't very smart when it comes to *, / and %
 * - some compilers can't deal well with *var++ constructs
 */


/*
 * Mono-to-16bpp expansion, two pixels per store, no branch per pixel.
 *
 * This is how every character reaches the screen.  The straightforward
 * version tests one source bit, branches on it, stores one 16-bit pixel,
 * shifts the mask and branches again to refill - so a glyph costs a
 * mispredictable branch per pixel, and glyph edges mispredict constantly.
 *
 * Here the two possible pixels are precomputed into a four-entry table
 * indexed by two source bits, so a pair of pixels is one long store and
 * no branch at all.  The source is read through a 32-bit accumulator
 * because a pair can straddle a word boundary and the run can start at
 * any bit.
 *
 * SOURCE ADVANCE IS LOAD-BEARING.  The original reads one word up front
 * and one more every time the mask wraps, which is 1 + ((bit + w) >> 4)
 * words, and the caller's row stride assumes exactly that.  This
 * computes the same figure rather than counting reads, so the two agree
 * even though this reads words on a different schedule.
 */
static void expand_row_replace(PIXEL *dst, const short *src, int bit, int n,
                               PIXEL foreground, PIXEL background)
{
    unsigned long pair[4];
    PIXEL fgbg[2];
    unsigned long acc;
    unsigned long *q;
    int avail, k;

    if (n <= 0)
        return;

    fgbg[0] = background;
    fgbg[1] = foreground;
    pair[0] = ((unsigned long)(unsigned short)background << 16) | (unsigned short)background;
    pair[1] = ((unsigned long)(unsigned short)background << 16) | (unsigned short)foreground;
    pair[2] = ((unsigned long)(unsigned short)foreground << 16) | (unsigned short)background;
    pair[3] = ((unsigned long)(unsigned short)foreground << 16) | (unsigned short)foreground;

    /* Prime the accumulator so the next pixel's bit is the top bit. */
    acc = (unsigned long)(unsigned short)*src++ << 16;
    acc <<= bit;
    avail = 16 - bit;

    /* Odd leading pixel, so the pair stores below are long-aligned. */
    if ((long)dst & 2)
    {
        if (avail == 0)
        {
            acc = (unsigned long)(unsigned short)*src++ << 16;
            avail = 16;
        }
        *dst++ = fgbg[(acc >> 31) & 1];
        acc <<= 1;
        avail--;
        n--;
    }

    q = (unsigned long *)dst;
    for (k = n >> 1; k > 0; k--)
    {
        if (avail < 2)
        {
            acc |= (unsigned long)(unsigned short)*src++ << (16 - avail);
            avail += 16;
        }
        *q++ = pair[(acc >> 30) & 3];
        acc <<= 2;
        avail -= 2;
    }
    dst = (PIXEL *)q;

    if (n & 1)
    {
        if (avail == 0)
        {
            acc = (unsigned long)(unsigned short)*src++ << 16;
            avail = 16;
        }
        *dst = fgbg[(acc >> 31) & 1];
    }
}

#ifdef BOTH
static void s_replace(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned int expand_word, mask;

    x = 1 << (15 - (x & 0x000f));

    for(i = h - 1; i >= 0; i--) {
        expand_word = *src_addr++;
        mask = x;
        for(j = w - 1; j >= 0; j--) {
            if (expand_word & mask) {
#ifdef BOTH
                *dst_addr_fast++ = foreground;
#endif
                *dst_addr++ = foreground;
            } else {
#ifdef BOTH
                *dst_addr_fast++ = background;
#endif
                *dst_addr++ = background;
            }
            if (!(mask >>= 1)) {
                mask = 0x8000;
                expand_word = *src_addr++;
            }
        }
        src_addr += src_line_add;
        dst_addr += dst_line_add;
#ifdef BOTH
        dst_addr_fast += dst_line_add;
#endif
    }
}

static void s_transparent(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned int expand_word, mask;

    (void) background;
    x = 1 << (15 - (x & 0x000f));

    for(i = h - 1; i >= 0; i--) {
        expand_word = *src_addr++;
        mask = x;
        for(j = w - 1; j >= 0; j--) {
            if (expand_word & mask) {
#ifdef BOTH
                *dst_addr_fast++ = foreground;
#endif
                *dst_addr++ = foreground;
            } else {
#ifdef BOTH
                dst_addr_fast++;
#endif
                dst_addr++;
            }
            if (!(mask >>= 1)) {
                mask = 0x8000;
                expand_word = *src_addr++;
            }
        }
        src_addr += src_line_add;
        dst_addr += dst_line_add;
#ifdef BOTH
        dst_addr_fast += dst_line_add;
#endif
    }
}

static void s_xor(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j, v;
    unsigned int expand_word, mask;

    (void) foreground;
    (void) background;
    x = 1 << (15 - (x & 0x000f));

    for(i = h - 1; i >= 0; i--) {
        expand_word = *src_addr++;
        mask = x;
        for(j = w - 1; j >= 0; j--) {
            if (expand_word & mask) {
#ifdef BOTH
                v = ~*dst_addr_fast;
#else
                v = ~*dst_addr;
#endif
#ifdef BOTH
                *dst_addr_fast++ = v;
#endif
                *dst_addr++ = v;
            } else {
#ifdef BOTH
                dst_addr_fast++;
#endif
                dst_addr++;
            }
            if (!(mask >>= 1)) {
                mask = 0x8000;
                expand_word = *src_addr++;
            }
        }
        src_addr += src_line_add;
        dst_addr += dst_line_add;
#ifdef BOTH
        dst_addr_fast += dst_line_add;
#endif
    }
}

static void s_revtransp(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned int expand_word, mask;

    (void) background;
    x = 1 << (15 - (x & 0x000f));

    for(i = h - 1; i >= 0; i--) {
        expand_word = *src_addr++;
        mask = x;
        for(j = w - 1; j >= 0; j--) {
            if (!(expand_word & mask)) {
#ifdef BOTH
                *dst_addr_fast++ = foreground;
#endif
                *dst_addr++ = foreground;
            } else {
#ifdef BOTH
                dst_addr_fast++;
#endif
                dst_addr++;
            }
            if (!(mask >>= 1)) {
                mask = 0x8000;
                expand_word = *src_addr++;
            }
        }
        src_addr += src_line_add;
        dst_addr += dst_line_add;
#ifdef BOTH
        dst_addr_fast += dst_line_add;
#endif
    }
}

#define BOTH_WAS_ON
#endif
#undef BOTH

/*
 * The functions below are exact copies of those above.
 * The '#undef BOTH' makes sure that this works as it should
 * when no shadow buffer is available
 */

static void replace(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i;
    int bit = x & 0x000f;
    /* Words the bit-at-a-time version would have read for this run: one
     * up front, plus one per mask wrap.  The row stride depends on it. */
    int words = 1 + ((bit + w) >> 4);

    (void) dst_addr_fast;

    for(i = h - 1; i >= 0; i--) {
        expand_row_replace(dst_addr, src_addr, bit, w, foreground, background);
        src_addr += words + src_line_add;
        dst_addr += w + dst_line_add;
    }
}

static void transparent(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned int expand_word, mask;

    (void) dst_addr_fast;
    (void) background;
    x = 1 << (15 - (x & 0x000f));

    for(i = h - 1; i >= 0; i--) {
        expand_word = *src_addr++;
        mask = x;
        for(j = w - 1; j >= 0; j--) {
            if (expand_word & mask) {
#ifdef BOTH
                *dst_addr_fast++ = foreground;
#endif
                *dst_addr++ = foreground;
            } else {
#ifdef BOTH
                dst_addr_fast++;
#endif
                dst_addr++;
            }
            if (!(mask >>= 1)) {
                mask = 0x8000;
                expand_word = *src_addr++;
            }
        }
        src_addr += src_line_add;
        dst_addr += dst_line_add;
#ifdef BOTH
        dst_addr_fast += dst_line_add;
#endif
    }
}

static void xor(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j, v;
    unsigned int expand_word, mask;

    (void) dst_addr_fast;
    (void) foreground;
    (void) background;
    x = 1 << (15 - (x & 0x000f));

    for(i = h - 1; i >= 0; i--) {
        expand_word = *src_addr++;
        mask = x;
        for(j = w - 1; j >= 0; j--) {
            if (expand_word & mask) {
#ifdef BOTH
                v = ~*dst_addr_fast;
#else
                v = ~*dst_addr;
#endif
#ifdef BOTH
                *dst_addr_fast++ = v;
#endif
                *dst_addr++ = v;
            } else {
#ifdef BOTH
                dst_addr_fast++;
#endif
                dst_addr++;
            }
            if (!(mask >>= 1)) {
                mask = 0x8000;
                expand_word = *src_addr++;
            }
        }
        src_addr += src_line_add;
        dst_addr += dst_line_add;
#ifdef BOTH
        dst_addr_fast += dst_line_add;
#endif
    }
}

static void revtransp(short *src_addr, int src_line_add, PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add, int x, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned int expand_word, mask;

    (void) dst_addr_fast;
    (void) background;
    x = 1 << (15 - (x & 0x000f));

    for(i = h - 1; i >= 0; i--) {
        expand_word = *src_addr++;
        mask = x;
        for(j = w - 1; j >= 0; j--) {
            if (!(expand_word & mask)) {
#ifdef BOTH
                *dst_addr_fast++ = foreground;
#endif
                *dst_addr++ = foreground;
            } else {
#ifdef BOTH
                dst_addr_fast++;
#endif
                dst_addr++;
            }
            if (!(mask >>= 1)) {
                mask = 0x8000;
                expand_word = *src_addr++;
            }
        }
        src_addr += src_line_add;
        dst_addr += dst_line_add;
#ifdef BOTH
        dst_addr_fast += dst_line_add;
#endif
    }
}

#ifdef BOTH_WAS_ON
#define BOTH
#endif

long CDECL c_expand_area(Virtual *vwk, MFDB *src, long src_x, long src_y, MFDB *dst, long dst_x, long dst_y, long w, long h, long operation, long colour)
{
    Workstation *wk;
    PIXEL *src_addr, *dst_addr, *dst_addr_fast;
    unsigned long foreground, background;
    int src_wrap, dst_wrap;
    int src_line_add, dst_line_add;
    unsigned long src_pos, dst_pos;
    int to_screen;

    wk = vwk->real_address;

    c_get_colours(vwk, colour, &foreground, &background);

    src_wrap = (long)src->wdwidth * 2;      /* Always monochrome */
    src_addr = src->address;
    src_pos = (short)src_y * (long)src_wrap + (src_x >> 4) * 2;
    src_line_add = src_wrap - (((src_x + w) >> 4) - (src_x >> 4) + 1) * 2;

    to_screen = 0;
    if (!dst || !dst->address || (dst->address == wk->screen.mfdb.address)) {       /* To screen? */
        dst_wrap = wk->screen.wrap;
        dst_addr = wk->screen.mfdb.address;
        to_screen = 1;
    } else {
        dst_wrap = (long)dst->wdwidth * 2 * dst->bitplanes;
        dst_addr = dst->address;
    }
    dst_pos = (short)dst_y * (long)dst_wrap + dst_x * PIXEL_SIZE;
    dst_line_add = dst_wrap - w * PIXEL_SIZE;

    src_addr += src_pos / 2;
    dst_addr += dst_pos / PIXEL_SIZE;
    src_line_add /= 2;
    dst_line_add /= PIXEL_SIZE;         /* Change into pixel count */

    dst_addr_fast = wk->screen.shadow.address;  /* May not really be to screen at all, but... */

#ifdef BOTH
    if (!to_screen || !dst_addr_fast) {
#endif
        switch (operation) {
        case 1:             /* Replace */
            replace(src_addr, src_line_add, dst_addr, 0, dst_line_add, src_x, w, h, foreground, background);
            break;
        case 2:             /* Transparent */
            transparent(src_addr, src_line_add, dst_addr, 0, dst_line_add, src_x, w, h, foreground, background);
            break;
        case 3:             /* XOR */
            xor(src_addr, src_line_add, dst_addr, 0, dst_line_add, src_x, w, h, foreground, background);
            break;
        case 4:             /* Reverse transparent */
            revtransp(src_addr, src_line_add, dst_addr, 0, dst_line_add, src_x, w, h, foreground, background);
            break;
        }
#ifdef BOTH
    } else {
        dst_addr_fast += dst_pos / PIXEL_SIZE;
        switch (operation) {
        case 1:             /* Replace */
            s_replace(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, src_x, w, h, foreground, background);
            break;
        case 2:             /* Transparent */
            s_transparent(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, src_x, w, h, foreground, background);
            break;
        case 3:             /* XOR */
            s_xor(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, src_x, w, h, foreground, background);
            break;
        case 4:             /* Reverse transparent */
            s_revtransp(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, src_x, w, h, foreground, background);
            break;
        }
    }
#else
    (void) to_screen;
    (void) dst_addr_fast;
#endif
    return 1;       /* Return as completed */
}
