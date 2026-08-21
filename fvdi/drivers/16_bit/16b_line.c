/*
 * A 16 bit graphics line routine, by Johan Klockars.
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

#ifdef BOTH
static void s_line_replace(PIXEL *addr, PIXEL *addr_fast, int count,
                    int d, int incrE, int incrNE, int one_step, int both_step,
                    PIXEL foreground, PIXEL background)
{
#ifdef BOTH
    *addr_fast = foreground;
#endif
    *addr = foreground;
    (void) addr_fast;
    (void) background;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }
}

static void s_line_replace_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                      int d, int incrE, int incrNE, int one_step, int both_step,
                      PIXEL foreground, PIXEL background)
{
    unsigned short mask = 0x8000;

    (void) addr_fast;
    if (pattern & mask) {
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    } else {
#ifdef BOTH
        *addr_fast = background;
#endif
        *addr = background;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (pattern & mask) {
#ifdef BOTH
            *addr_fast = foreground;
#endif
            *addr = foreground;
        } else {
#ifdef BOTH
            *addr_fast = background;
#endif
            *addr = background;
        }
    }
}

static void s_line_transparent(PIXEL *addr, PIXEL *addr_fast, int count,
                        int d, int incrE, int incrNE, int one_step, int both_step,
                        PIXEL foreground, PIXEL background)
{
#ifdef BOTH
    *addr_fast = foreground;
#endif
    *addr = foreground;
    (void) addr_fast;
    (void) background;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }
}

static void s_line_transparent_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                          int d, int incrE, int incrNE, int one_step, int both_step,
                          PIXEL foreground, PIXEL background)
{
    unsigned short mask = 0x8000;

    (void) addr_fast;
    (void) background;
    if (pattern & mask) {
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (pattern & mask) {
#ifdef BOTH
            *addr_fast = foreground;
#endif
            *addr = foreground;
        }
    }
}

static void s_line_xor(PIXEL *addr, PIXEL *addr_fast, int count,
                int d, int incrE, int incrNE, int one_step, int both_step,
                PIXEL foreground, PIXEL background)
{
    int v;

    (void) addr_fast;
    (void) foreground;
    (void) background;
#ifdef BOTH
    v = ~*addr_fast;
#else
    v = ~*addr;
#endif
#ifdef BOTH
    *addr_fast = v;
#endif
    *addr = v;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        v = ~*addr_fast;
#else
        v = ~*addr;
#endif
#ifdef BOTH
        *addr_fast = v;
#endif
        *addr = v;
    }
}

static void s_line_xor_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                  int d, int incrE, int incrNE, int one_step, int both_step,
                  PIXEL foreground, PIXEL background)
{
    int v;
    unsigned short mask = 0x8000;

    (void) addr_fast;
    (void) foreground;
    (void) background;
    if (pattern & mask) {
#ifdef BOTH
        v = ~*addr_fast;
#else
        v = ~*addr;
#endif
#ifdef BOTH
        *addr_fast = v;
#endif
        *addr = v;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (pattern & mask) {
#ifdef BOTH
            v = ~*addr_fast;
#else
            v = ~*addr;
#endif
#ifdef BOTH
            *addr_fast = v;
#endif
            *addr = v;
        }
    }
}

static void s_line_revtransp(PIXEL *addr, PIXEL *addr_fast, int count,
                      int d, int incrE, int incrNE, int one_step, int both_step,
                      PIXEL foreground, PIXEL background)
{
#ifdef BOTH
    *addr_fast = foreground;
#endif
    *addr = foreground;
    (void) addr_fast;
    (void) background;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }
}

static void s_line_revtransp_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                        int d, int incrE, int incrNE, int one_step, int both_step,
                        PIXEL foreground, PIXEL background)
{
    unsigned short mask = 0x8000;

    (void) addr_fast;
    (void) background;
    if (!(pattern & mask)) {
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (!(pattern & mask)) {
#ifdef BOTH
            *addr_fast = foreground;
#endif
            *addr = foreground;
        }
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

static void line_replace(PIXEL *addr, PIXEL *addr_fast, int count,
                    int d, int incrE, int incrNE, int one_step, int both_step,
                    PIXEL foreground, PIXEL background)
{
#ifdef BOTH
    *addr_fast = foreground;
#endif
    *addr = foreground;
    (void) addr_fast;
    (void) background;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }
}

static void line_replace_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                      int d, int incrE, int incrNE, int one_step, int both_step,
                      PIXEL foreground, PIXEL background)
{
    unsigned short mask = 0x8000;

    (void) addr_fast;
    if (pattern & mask) {
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    } else {
#ifdef BOTH
        *addr_fast = background;
#endif
        *addr = background;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (pattern & mask) {
#ifdef BOTH
            *addr_fast = foreground;
#endif
            *addr = foreground;
        } else {
#ifdef BOTH
            *addr_fast = background;
#endif
            *addr = background;
        }
    }
}

static void line_transparent(PIXEL *addr, PIXEL *addr_fast, int count,
                        int d, int incrE, int incrNE, int one_step, int both_step,
                        PIXEL foreground, PIXEL background)
{
#ifdef BOTH
    *addr_fast = foreground;
#endif
    *addr = foreground;
    (void) addr_fast;
    (void) background;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }
}

static void line_transparent_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                          int d, int incrE, int incrNE, int one_step, int both_step,
                          PIXEL foreground, PIXEL background)
{
    unsigned short mask = 0x8000;

    (void) addr_fast;
    (void) foreground;
    (void) background;
    if (pattern & mask) {
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (pattern & mask) {
#ifdef BOTH
            *addr_fast = foreground;
#endif
            *addr = foreground;
        }
    }
}

static void line_xor(PIXEL *addr, PIXEL *addr_fast, int count,
                int d, int incrE, int incrNE, int one_step, int both_step,
                PIXEL foreground, PIXEL background)
{
    int v;

    (void) addr_fast;
    (void) foreground;
    (void) background;
#ifdef BOTH
    v = ~*addr_fast;
#else
    v = ~*addr;
#endif
#ifdef BOTH
    *addr_fast = v;
#endif
    *addr = v;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        v = ~*addr_fast;
#else
        v = ~*addr;
#endif
#ifdef BOTH
        *addr_fast = v;
#endif
        *addr = v;
    }
}

static void line_xor_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                  int d, int incrE, int incrNE, int one_step, int both_step,
                  PIXEL foreground, PIXEL background)
{
    int v;
    unsigned short mask = 0x8000;

    (void) addr_fast;
    (void) foreground;
    (void) background;
    if (pattern & mask) {
#ifdef BOTH
        v = ~*addr_fast;
#else
        v = ~*addr;
#endif
#ifdef BOTH
        *addr_fast = v;
#endif
        *addr = v;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (pattern & mask) {
#ifdef BOTH
            v = ~*addr_fast;
#else
            v = ~*addr;
#endif
#ifdef BOTH
            *addr_fast = v;
#endif
            *addr = v;
        }
    }
}

static void line_revtransp(PIXEL *addr, PIXEL *addr_fast, int count,
                      int d, int incrE, int incrNE, int one_step, int both_step,
                      PIXEL foreground, PIXEL background)
{
#ifdef BOTH
    *addr_fast = foreground;
#endif
    *addr = foreground;
    (void) addr_fast;
    (void) background;

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }
}

static void line_revtransp_p(PIXEL *addr, PIXEL *addr_fast, long pattern, int count,
                        int d, int incrE, int incrNE, int one_step, int both_step,
                        PIXEL foreground, PIXEL background)
{
    unsigned short mask = 0x8000;

    (void) addr_fast;
    (void) background;
    if (!(pattern & mask)) {
#ifdef BOTH
        *addr_fast = foreground;
#endif
        *addr = foreground;
    }

    for(--count; count >= 0; count--) {
        if (d < 0) {
            d += incrE;
#ifdef BOTH
            addr_fast += one_step;
#endif
            addr += one_step;
        } else {
            d += incrNE;
#ifdef BOTH
            addr_fast += both_step;
#endif
            addr += both_step;
        }

        if (!(mask >>= 1))
            mask = 0x8000;

        if (!(pattern & mask)) {
#ifdef BOTH
            *addr_fast = foreground;
#endif
            *addr = foreground;
        }
    }
}

#ifdef BOTH_WAS_ON
#define BOTH
#endif

/*
 * Axis aligned solid runs.
 *
 * The horizontal helper deliberately mirrors solid_run() in 16b_fill.c:
 * an odd leading pixel is written first so that every store after it is
 * long aligned, then the pixels are written in pairs as longs, unrolled
 * eight deep. Vertical runs are strided and cannot be paired, so they
 * only get the loop overhead cut by unrolling four deep.
 */
static void line_solid_run(PIXEL *d, int n, PIXEL colour)
{
    unsigned long pair;
    unsigned long *q;
    int pairs;

    if (n <= 0)
        return;

    if ((long)d & 2) {
        *d++ = colour;
        if (--n == 0)
            return;
    }

    pair = ((unsigned long)(unsigned short)colour << 16) |
            (unsigned short)colour;
    q = (unsigned long *)d;

    for (pairs = n >> 1; pairs >= 8; pairs -= 8) {
        q[0] = pair; q[1] = pair; q[2] = pair; q[3] = pair;
        q[4] = pair; q[5] = pair; q[6] = pair; q[7] = pair;
        q += 8;
    }
    while (pairs-- > 0)
        *q++ = pair;

    if (n & 1)
        *(PIXEL *)q = colour;
}

static void line_solid_col(PIXEL *d, int n, int step, PIXEL colour)
{
    for (; n >= 4; n -= 4) {
        *d = colour; d += step;
        *d = colour; d += step;
        *d = colour; d += step;
        *d = colour; d += step;
    }
    while (n-- > 0) {
        *d = colour;
        d += step;
    }
}

/*
 * XOR (mode 3) complements what is already there and ignores the colour,
 * exactly as line_xor() does. Complementing a long complements both of
 * the pixels it holds, so the horizontal run can still be paired.
 */
static void line_xor_run(PIXEL *d, int n)
{
    unsigned long *q;
    int pairs;

    if (n <= 0)
        return;

    if ((long)d & 2) {
        *d = ~*d;
        d++;
        if (--n == 0)
            return;
    }

    q = (unsigned long *)d;

    for (pairs = n >> 1; pairs >= 8; pairs -= 8) {
        q[0] = ~q[0]; q[1] = ~q[1]; q[2] = ~q[2]; q[3] = ~q[3];
        q[4] = ~q[4]; q[5] = ~q[5]; q[6] = ~q[6]; q[7] = ~q[7];
        q += 8;
    }
    while (pairs-- > 0) {
        *q = ~*q;
        q++;
    }

    if (n & 1) {
        d = (PIXEL *)q;
        *d = ~*d;
    }
}

static void line_xor_col(PIXEL *d, int n, int step)
{
    for (; n >= 4; n -= 4) {
        *d = ~*d; d += step;
        *d = ~*d; d += step;
        *d = ~*d; d += step;
        *d = ~*d; d += step;
    }
    while (n-- > 0) {
        *d = ~*d;
        d += step;
    }
}

#ifdef BOTH
/*
 * With a shadow buffer the XOR source is the shadow, not the screen
 * (see s_line_xor()), so the two buffers cannot be complemented
 * independently. Keep it a plain pixel loop rather than assume the two
 * buffers share the same long alignment.
 */
static void line_xor_run_both(PIXEL *d, PIXEL *d_fast, int n)
{
    int v;

    while (n-- > 0) {
        v = ~*d_fast;
        *d_fast++ = v;
        *d++ = v;
    }
}

static void line_xor_col_both(PIXEL *d, PIXEL *d_fast, int n, int step)
{
    int v;

    while (n-- > 0) {
        v = ~*d_fast;
        *d_fast = v;
        *d = v;
        d_fast += step;
        d += step;
    }
}
#endif


long CDECL c_line_draw(Virtual *vwk, long x1, long y1, long x2, long y2,
                       long pattern, long colour, long mode)
{
    Workstation *wk;
    PIXEL *addr, *addr_fast;
    unsigned long foreground, background;
    int line_add;
    long pos;
    int x_step, y_step;
    int dx, dy;
    int one_step, both_step;
    int d, count;
    int incrE, incrNE;
    int run_step;

    if ((long)vwk & 1) {
        return -1;          /* Don't know about anything yet */
    }

    if (!clip_line(vwk, &x1, &y1, &x2, &y2))
        return 1;

    c_get_colours(vwk, colour, &foreground, &background);

    wk = vwk->real_address;

    pos = (short)y1 * (long)wk->screen.wrap + x1 * 2;
    addr = wk->screen.mfdb.address;
    line_add = wk->screen.wrap >> 1;

    /*
     * Axis aligned solid lines are the bulk of what GEM draws - box
     * borders, window frames, separators, underlines and table rules all
     * reach the driver as single horizontal or vertical segments, and the
     * Bresenham loop below walks them one pixel at a time. They are plain
     * runs of pixels, so deal with them here instead.
     * Only solid lines qualify: a patterned line still needs the mask
     * stepping of the *_p routines and falls through to the general code.
     * Modes 1, 2 and 4 are character for character the same function for a
     * solid line (they all store the foreground unconditionally), so one
     * solid run serves all three; only mode 3 needs its own.
     */
    if ((pattern & 0xffff) == 0xffff && (y1 == y2 || x1 == x2)) {
        if (y1 == y2) {
            /*
             * The Bresenham loop stores the start pixel and then steps
             * dx more times, so the run covers dx + 1 pixels and includes
             * both endpoints. A one pixel line arrives here with count 1.
             * The endpoints may be in either order (clip_line can swap
             * them), so start from the left one.
             */
            count = (int)(x2 - x1);
            if (count < 0) {
                count = -count;
                x1 = x2;
            }
            run_step = 1;
        } else {
            count = (int)(y2 - y1);
            if (count < 0) {
                count = -count;
                y1 = y2;
            }
            run_step = line_add;
        }
        count++;

        /*
         * run_step tells the two apart below: line_add is the screen
         * width in pixels, so it can never be 1.
         */

        pos = (short)y1 * (long)wk->screen.wrap + x1 * 2;
        addr += pos >> 1;

#ifdef BOTH
        if ((addr_fast = wk->screen.shadow.address) != 0) {
            addr_fast += pos >> 1;
            switch (mode) {
            case 1:             /* Replace */
            case 2:             /* Transparent */
            case 4:             /* Reverse transparent */
                if (run_step == 1) {
                    line_solid_run(addr_fast, count, foreground);
                    line_solid_run(addr, count, foreground);
                } else {
                    line_solid_col(addr_fast, count, run_step, foreground);
                    line_solid_col(addr, count, run_step, foreground);
                }
                break;
            case 3:             /* XOR */
                if (run_step == 1)
                    line_xor_run_both(addr, addr_fast, count);
                else
                    line_xor_col_both(addr, addr_fast, count, run_step);
                break;
            }
        } else
#endif
        {
            switch (mode) {
            case 1:             /* Replace */
            case 2:             /* Transparent */
            case 4:             /* Reverse transparent */
                if (run_step == 1)
                    line_solid_run(addr, count, foreground);
                else
                    line_solid_col(addr, count, run_step, foreground);
                break;
            case 3:             /* XOR */
                if (run_step == 1)
                    line_xor_run(addr, count);
                else
                    line_xor_col(addr, count, run_step);
                break;
            }
        }
        return 1;       /* Return as completed */
    }

    x_step = 1;
    y_step = line_add;

    dx = x2 - x1;
    if (dx < 0) {
        dx = -dx;
        x_step = -x_step;
    }
    dy = y2 - y1;
    if (dy < 0) {
        dy = -dy;
        y_step = -y_step;
    }

    if (dx > dy) {
        count = dx;
        one_step = x_step;
        incrE = 2 * dy;
        incrNE = 2 * dy - 2 * dx;
        d = 2 * dy - dx;
    } else {
        count = dy;
        one_step = y_step;
        incrE = 2 * dx;
        incrNE = 2 * dx - 2 * dy;
        d = 2 * dx - dy;
    }
    both_step = x_step + y_step;

#ifdef BOTH
    if ((addr_fast = wk->screen.shadow.address) != 0) {

        addr += pos >> 1;
        addr_fast += pos >> 1;
        if ((pattern & 0xffff) == 0xffff) {
            switch (mode) {
            case 1:             /* Replace */
                s_line_replace(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 2:             /* Transparent */
                s_line_transparent(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 3:             /* XOR */
                s_line_xor(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 4:             /* Reverse transparent */
                s_line_revtransp(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            }
        } else {
            switch (mode) {
            case 1:             /* Replace */
                s_line_replace_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 2:             /* Transparent */
                s_line_transparent_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 3:             /* XOR */
                s_line_xor_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 4:             /* Reverse transparent */
                s_line_revtransp_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            }
        }
    } else
#endif
    {
        addr += pos >> 1;
        if ((pattern & 0xffff) == 0xffff) {
            switch (mode) {
            case 1:             /* Replace */
                line_replace(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 2:             /* Transparent */
                line_transparent(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 3:             /* XOR */
                line_xor(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 4:             /* Reverse transparent */
                line_revtransp(addr, addr_fast, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            }
        } else {
            switch (mode) {
            case 1:             /* Replace */
                line_replace_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 2:             /* Transparent */
                line_transparent_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 3:             /* XOR */
                line_xor_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            case 4:             /* Reverse transparent */
                line_revtransp_p(addr, addr_fast, pattern, count, d, incrE, incrNE, one_step, both_step, foreground, background);
                break;
            }
        }
    }
    return 1;       /* Return as completed */
}
