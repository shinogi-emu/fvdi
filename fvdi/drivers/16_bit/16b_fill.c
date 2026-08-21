/* 
 * A 16 bit graphics fill routine, by Johan Klockars.
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
#define PIXEL_32    long

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
 * One row of a patterned fill.
 *
 * The straightforward version tests one pattern bit, branches on it and
 * stores one 16-bit pixel, for every pixel - which measured 8.9x slower
 * than the solid path over the same rectangle, on the same call.
 *
 * The pattern repeats every 16 pixels, so expand ONE period per row into
 * eight longs and then repeat those.  Because a period is 16 pixels and
 * each pass writes exactly 16, the block realigns every pass: the same
 * eight longs serve the whole row, with no re-rotation and no branch per
 * pixel.  The odd leading pixel is written on its own first so that every
 * store after it is long-aligned.
 */
static void pattern_run(PIXEL **addrp, int n, unsigned short pw, int phase,
                        PIXEL foreground, PIXEL background)
{
    unsigned long blk[8];
    PIXEL cell[16];
    PIXEL *d = *addrp;
    unsigned long *q;
    int k;

    if (n <= 0)
        return;

    if ((long)d & 2)
    {
        *d++ = (pw & (0x8000u >> (phase & 15))) ? foreground : background;
        phase++;
        if (--n == 0)
        {
            *addrp = d;
            return;
        }
    }

    for (k = 0; k < 16; k++)
        cell[k] = (pw & (0x8000u >> ((phase + k) & 15))) ? foreground : background;
    for (k = 0; k < 8; k++)
        blk[k] = ((unsigned long)(unsigned short)cell[2 * k] << 16) |
                 (unsigned short)cell[2 * k + 1];

    q = (unsigned long *)d;
    for (k = n >> 4; k > 0; k--)
    {
        q[0] = blk[0]; q[1] = blk[1]; q[2] = blk[2]; q[3] = blk[3];
        q[4] = blk[4]; q[5] = blk[5]; q[6] = blk[6]; q[7] = blk[7];
        q += 8;
    }
    d = (PIXEL *)q;
    n &= 15;

    k = 0;
    while (n >= 2)
    {
        *(unsigned long *)d = blk[k];
        d += 2;
        k++;
        n -= 2;
    }
    if (n)
        *d = cell[2 * k];

    *addrp = d + n;
}

/*
 * Solid horizontal run of one colour.
 *
 * This is the hottest loop in the driver, and not only because of
 * rectangle fills: engine/draw.s _hline clips a horizontal line and then
 * calls straight into the driver's fill with a one-pixel-high rectangle,
 * and engine/line.c drives circle and polygon scanlines through the same
 * hline.  So every box border, window frame, underline, separator,
 * circle and filled polygon on the screen arrives here.
 *
 * Pixels are 16 bits, so they go two at a time as longs.  The odd
 * leading pixel is written on its own first: it costs one store and it
 * means every store after it is long-aligned, which matters more than
 * the store it saves.  The body is unrolled eight longs deep to keep
 * loop overhead off the critical path.
 */
static void solid_run(PIXEL **addrp, int n, PIXEL colour)
{
    PIXEL *d = *addrp;
    unsigned long pair;
    unsigned long *q;
    int pairs;

    if (n <= 0)
        return;

    if ((long)d & 2)
    {
        *d++ = colour;
        if (--n == 0)
        {
            *addrp = d;
            return;
        }
    }

    pair = ((unsigned long)(unsigned short)colour << 16) |
            (unsigned short)colour;
    q = (unsigned long *)d;

    for (pairs = n >> 1; pairs >= 8; pairs -= 8)
    {
        q[0] = pair; q[1] = pair; q[2] = pair; q[3] = pair;
        q[4] = pair; q[5] = pair; q[6] = pair; q[7] = pair;
        q += 8;
    }
    while (pairs-- > 0)
        *q++ = pair;

    d = (PIXEL *)q;
    if (n & 1)
        *d++ = colour;

    *addrp = d;
}

#ifdef BOTH
static void s_fill_replace(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;

    int phase = x & 0x000f;

    (void) addr_fast;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0xffff:
#ifdef BOTH
            solid_run(&addr_fast, w, foreground);
#endif
            solid_run(&addr, w, foreground);
            (void) j;
            break;
        default:
#ifdef BOTH
            pattern_run(&addr_fast, w, pattern_word, phase, foreground, background);
#endif
            pattern_run(&addr, w, pattern_word, phase, foreground, background);
            (void) j;
            (void) mask;
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
    }
}

static void s_fill_transparent(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;

    (void) addr_fast;
    (void) background;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0xffff:
            for(j = w - 1; j >= 0; j--) {
#ifdef BOTH
                *addr_fast = foreground;
                addr_fast++;
#endif
                *addr = foreground;
                addr++;
            }
            break;
        default:
            mask = x;
            for(j = w - 1; j >= 0; j--) {
                if (pattern_word & mask) {
#ifdef BOTH
                    *addr_fast = foreground;
                    addr_fast++;
#endif
                    *addr = foreground;
                    addr++;
                } else {
#ifdef BOTH
                    addr_fast++;
#endif
                    addr++;
                }
                if (!(mask >>= 1))
                    mask = 0x8000;
            }
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
    }
}

static void s_fill_xor(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;
    PIXEL v;

    (void) addr_fast;
    (void) foreground;
    (void) background;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0xffff:
            for(j = w - 1; j >= 0; j--) {
#ifdef BOTH
                v = ~*addr_fast;
#else
                v = ~*addr;
#endif
#ifdef BOTH
                *addr_fast = v;
                addr_fast++;
#endif
                *addr = v;
                addr++;
            }
            break;
        default:
            mask = x;
            for(j = w - 1; j >= 0; j--) {
                if (pattern_word & mask) {
#ifdef BOTH
                    v = ~*addr_fast;
#else
                    v = ~*addr;
#endif
#ifdef BOTH
                    *addr_fast = v;
                    addr_fast++;
#endif
                    *addr = v;
                    addr++;
                } else {
#ifdef BOTH
                    addr_fast++;
#endif
                    addr++;
                }
                if (!(mask >>= 1))
                    mask = 0x8000;
            }
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
    }
}

static void s_fill_revtransp(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;

    (void) addr_fast;
    (void) background;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0x0000:
            for(j = w - 1; j >= 0; j--) {
#ifdef BOTH
                *addr_fast = foreground;
                addr_fast++;
#endif
                *addr = foreground;
                addr++;
            }
            break;
        default:
            mask = x;
            for(j = w - 1; j >= 0; j--) {
                if (!(pattern_word & mask)) {
#ifdef BOTH
                    *addr_fast = foreground;
                    addr_fast++;
#endif
                    *addr = foreground;
                    addr++;
                } else {
#ifdef BOTH
                    addr_fast++;
#endif
                    addr++;
                }
                if (!(mask >>= 1))
                    mask = 0x8000;
            }
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
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

static void fill_replace(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;

    int phase = x & 0x000f;

    (void) addr_fast;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0xffff:
#ifdef BOTH
            solid_run(&addr_fast, w, foreground);
#endif
            solid_run(&addr, w, foreground);
            (void) j;
            break;
        default:
#ifdef BOTH
            pattern_run(&addr_fast, w, pattern_word, phase, foreground, background);
#endif
            pattern_run(&addr, w, pattern_word, phase, foreground, background);
            (void) j;
            (void) mask;
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
    }
}

static void fill_transparent(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;

    (void) addr_fast;
    (void) background;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0xffff:
            for(j = w - 1; j >= 0; j--) {
#ifdef BOTH
                *addr_fast = foreground;
                addr_fast++;
#endif
                *addr = foreground;
                addr++;
            }
            break;
        default:
            mask = x;
            for(j = w - 1; j >= 0; j--) {
                if (pattern_word & mask) {
#ifdef BOTH
                    *addr_fast = foreground;
                    addr_fast++;
#endif
                    *addr = foreground;
                    addr++;
                } else {
#ifdef BOTH
                    addr_fast++;
#endif
                    addr++;
                }
                if (!(mask >>= 1))
                    mask = 0x8000;
            }
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
    }
}

static void fill_xor(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;
    PIXEL v;

    (void) addr_fast;
    (void) foreground;
    (void) background;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0xffff:
            for(j = w - 1; j >= 0; j--) {
#ifdef BOTH
                v = ~*addr_fast;
#else
                v = ~*addr;
#endif
#ifdef BOTH
                *addr_fast = v;
                addr_fast++;
#endif
                *addr = v;
                addr++;
            }
            break;
        default:
            mask = x;
            for(j = w - 1; j >= 0; j--) {
                if (pattern_word & mask) {
#ifdef BOTH
                    v = ~*addr_fast;
#else
                    v = ~*addr;
#endif
#ifdef BOTH
                    *addr_fast = v;
                    addr_fast++;
#endif
                    *addr = v;
                    addr++;
                } else {
#ifdef BOTH
                    addr_fast++;
#endif
                    addr++;
                }
                if (!(mask >>= 1))
                    mask = 0x8000;
            }
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
    }
}

static void fill_revtransp(PIXEL *addr, PIXEL *addr_fast, int line_add, short *pattern, int x, int y, int w, int h, PIXEL foreground, PIXEL background)
{
    int i, j;
    unsigned short pattern_word, mask;

    (void) addr_fast;
    (void) background;
    i = y;
    h = y + h;
    x = 1 << (15 - (x & 0x000f));

    /* Tell gcc that this cannot happen (already checked in c_fill_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    for(; i < h; i++) {
        pattern_word = pattern[i & 0x000f];
        switch (pattern_word) {
        case 0x0000:
            for(j = w - 1; j >= 0; j--) {
#ifdef BOTH
                *addr_fast = foreground;
                addr_fast++;
#endif
                *addr = foreground;
                addr++;
            }
            break;
        default:
            mask = x;
            for(j = w - 1; j >= 0; j--) {
                if (!(pattern_word & mask)) {
#ifdef BOTH
                    *addr_fast = foreground;
                    addr_fast++;
#endif
                    *addr = foreground;
                    addr++;
                } else {
#ifdef BOTH
                    addr_fast++;
#endif
                    addr++;
                }
                if (!(mask >>= 1))
                    mask = 0x8000;
            }
            break;
        }
#ifdef BOTH
        addr_fast += line_add;
#endif
        addr += line_add;
    }
}


#ifdef BOTH_WAS_ON
#define BOTH
#endif

long CDECL c_fill_area(Virtual *vwk, long x, long y, long w, long h,
                       short *pattern, long colour, long mode, long interior_style)
{
    Workstation *wk;
    PIXEL *addr, *addr_fast;
    unsigned long foreground, background;
    long line_add;
    unsigned long pos;
    short *table;

    if (w <= 0 || h <= 0)
        return 1;

    (void) interior_style;
    table = 0;
    if ((long) vwk & 1) {
        if ((y & 0xffff) != 0)
            return -1;      /* Don't know about this kind of table operation */
        table = (short *)x;
        (void) table;
        h = (y >> 16) & 0xffff;
        vwk = (Virtual *)((long)vwk - 1);
        return -1;          /* Don't know about anything yet */
    }

    c_get_colours(vwk, colour, &foreground, &background);

    wk = vwk->real_address;

    pos = (short)y * (long)wk->screen.wrap + x * 2;
    addr = wk->screen.mfdb.address;
    line_add = (wk->screen.wrap - w * 2) >> 1;

#ifdef BOTH
    if ((addr_fast = wk->screen.shadow.address) != 0) {

        addr += pos / PIXEL_SIZE;
        addr_fast += pos / PIXEL_SIZE;
        switch (mode) {
        case 1:             /* Replace */
            s_fill_replace(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        case 2:             /* Transparent */
            s_fill_transparent(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        case 3:             /* XOR */
            s_fill_xor(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        case 4:             /* Reverse transparent */
            s_fill_revtransp(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        }
    } else
#endif
    {
        addr += pos / PIXEL_SIZE;
        switch (mode) {
        case 1:             /* Replace */
            fill_replace(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        case 2:             /* Transparent */
            fill_transparent(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        case 3:             /* XOR */
            fill_xor(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        case 4:             /* Reverse transparent */
            fill_revtransp(addr, addr_fast, line_add, pattern, x, y, w, h, foreground, background);
            break;
        }
    }
    return 1;       /* Return as completed */
}
