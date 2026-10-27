/*
 * abline - draw a line (general purpose)
 *
 * This routine draws a line between (_X1,_Y1) and (_X2,_Y2).
 * The line is modified by the LN_MASK and WRT_MODE variables.
 * This routine handles all 3 interleaved bitplanes video resolutions.
 *
 * Note that for line-drawing the background color is always 0 (i.e., there
 * is no user-settable background color).  This fact allows coding short-cuts
 * in the implementation of "replace" and "not" modes, resulting in faster
 * execution of their inner loops.
 *
 * This routines is more or less the one from the original VDI asm part.
 * I could not take bresenham, because pixels were set improperly in
 * use with the polygone filling part, did look ugly.  (MAD)
 *
 * input:
 *     X1, Y1, X2, Y2 = coordinates.
 *     num_planes     = number of video planes. (resolution)
 *     LN_MASK        = line mask. (for dashed/dotted lines)
 *     WRT_MODE       = writing mode:
 *                          0 => replace mode.
 *                          1 => or mode.
 *                          2 => xor mode.
 *                          3 => not mode.
 *
 * output:
 *     LN_MASK rotated to proper alignment with (X2,Y2).
 */

#include "fvdi.h"
#include "driver.h"
#include "bitplane.h"


/*
 * Every writing mode, in one plane, is new = (old & keep) ^ set, inside the
 * pixels the line covers:
 *
 *   mode               keep        set (colour bit 1)   set (colour bit 0)
 *   1 replace          0           pattern              0
 *   2 transparent      ~pattern    pattern              0
 *   3 xor              ~0          pattern              pattern
 *   4 rev. transparent ~pattern    0                    pattern
 *
 * so the mode and colour are settled once per line: 'keep' is
 * (pattern & keep_and) ^ keep_xor for all planes, and plane k takes the
 * pattern as its 'set' when bit k of 'sel' is 1.  The planes of a 16 pixel
 * column are interleaved words, so a pixel's word in every plane is one
 * short run of memory.
 */
typedef struct {
    unsigned short *base;       /* screen */
    long wrap;                  /* shorts per line */
    int planes;
    unsigned short keep_and, keep_xor;
    unsigned int sel;
} LineOp;

/* Pattern bits p at the pixels in mask m of the word at addr, every plane */
static inline void put_word(const LineOp *op, unsigned short *addr, unsigned short m, unsigned short p)
{
    unsigned short keep = ((p & op->keep_and) ^ op->keep_xor) | ~m;
    unsigned int sel = op->sel;
    int k;

    p &= m;
    for (k = 0; k < op->planes; k++, sel >>= 1)
        addr[k] = (addr[k] & keep) ^ (sel & 1 ? p : 0);
}

/* A horizontal line, xl <= xr: whole words between the two ends */
static void hline(const LineOp *op, long xl, long xr, long y, unsigned short pattern)
{
    unsigned short *addr = op->base + y * op->wrap + (xl >> 4) * op->planes;
    unsigned short lm = 0xffff >> (xl & 15);
    unsigned short rm = 0xffff << (15 - (xr & 15));
    long n = (xr >> 4) - (xl >> 4);
    int s = xl & 15;
    unsigned short p;

    /* The pattern starts at the left end; turn it to the screen's words */
    p = s ? (unsigned short)((pattern >> s) | (pattern << (16 - s))) : pattern;

    if (n == 0)
    {
        put_word(op, addr, lm & rm, p);
        return;
    }
    put_word(op, addr, lm, p);
    addr += op->planes;
    if (--n > 0)
    {
        unsigned short keep = (p & op->keep_and) ^ op->keep_xor;
        unsigned short set[8];
        unsigned int sel = op->sel;
        int k, planes = op->planes;

        for (k = 0; k < planes; k++, sel >>= 1)
            set[k] = sel & 1 ? p : 0;
        if (keep == 0)
        {
            /* Replace: nothing of the old words is kept */
            while (n-- > 0)
            {
                for (k = 0; k < planes; k++)
                    *addr++ = set[k];
            }
        } else
        {
            while (n-- > 0)
            {
                for (k = 0; k < planes; k++, addr++)
                    *addr = (*addr & keep) ^ set[k];
            }
        }
    }
    put_word(op, addr, rm, p);
}


/*
 * The original loops: one pass along the line for each plane, with the
 * writing mode chosen outside the pixel loop.  With one or two planes a
 * single read-modify-write of memory per pixel is as quick as a line
 * gets, so those depths keep them for every line that is not horizontal.
 */
static void __attribute__((noinline)) line_per_plane(Virtual *vwk, long x1, long y1, long x2, long y2,
            long pattern, unsigned long color, long mode)
{
    void *adr;                  /* Using void pointer is much faster */
    short dx;                   /* Width of rectangle around line */
    short dy;                   /* Height of rectangle around line */
    short xinc;                 /* Positive increase for each x step */
    short yinc;                 /* In/decrease for each y step */
    unsigned short msk;
    int plane;
    unsigned short linemask;    /* Linestyle bits */

    linemask = pattern;                 /* To avoid compiler warning */

    /* Make x axis always going up */
    dy = y2 - y1;
    dx = x2 - x1;
    if (dx < 0)
    {
        /* if delta x < 0 then draw from point 2 to 1 */
        dx = -dx;
        dy = -dy;
        x1 = x2;
        y1 = y2;
    }

    /* Calculate increase values for x and y to add to actual address */
    yinc = vwk->real_address->screen.wrap;
    if (dy < 0)
    {
        dy = -dy;
        yinc = -yinc;
    }
    xinc = vwk->real_address->screen.mfdb.bitplanes << 1;

    adr = vwk->real_address->screen.mfdb.address;
    adr += (x1 / 16) * vwk->real_address->screen.mfdb.bitplanes * 2;
    adr += (long)y1 * vwk->real_address->screen.wrap;
    msk = 0x8000 >> (x1 & 0xf);         /* Initial bit position in word */

    for (plane = vwk->real_address->screen.mfdb.bitplanes - 1; plane >= 0; plane--)
    {
        void *addr;
        short  eps;              /* Epsilon */
        short  e1;               /* Epsilon 1 */
        short  e2;               /* Epsilon 2 */
        short  loopcnt;
        unsigned short bit;

        /* Load values fresh for this bitplane */
        addr = adr;             /* Initial start address for changes */
        bit = msk;              /* Initial bit position in word */
        linemask = pattern;

        /* Skip back one step to make for easier check below */
        linemask = (linemask >> 1) | (linemask << 15);

        if (dx >= dy)
        {
            e1 = 2 * dy;
            eps = -dx;
            e2 = 2 * dx;

            switch(mode)
            {
            case 4:              /* Reverse transparent  */
                if (color & 0x0001)
                {
                    bit = ~bit;
                    for (loopcnt = dx; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr &= bit;
                        bit = (bit >> 1) | (bit << 15);
                        if (!(bit & 0x8000))
                            addr += xinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            addr += yinc;       /* Increment y */
                        }
                    }
                } else
                {
                    for (loopcnt = dx; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr |= bit;
                        bit = (bit >> 1) | (bit << 15);
                        if (bit & 0x8000)
                            addr += xinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            addr += yinc;       /* Increment y */
                        }
                    }
                }
                break;

            case 3:              /* xor */
                for (loopcnt = dx; loopcnt >= 0; loopcnt--)
                {
                    linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                    if (linemask & 0x8000)
                        *(short *)addr ^= bit;
                    bit = (bit >> 1) | (bit << 15);
                    if (bit & 0x8000)
                        addr += xinc;
                    eps += e1;
                    if (eps >= 0)
                    {
                        eps -= e2;
                        addr += yinc;       /* Increment y */
                    }
                }
                break;

            case 2:              /* or */
                if (color & 0x0001)
                {
                    for (loopcnt = dx; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr |= bit;
                        bit = (bit >> 1) | (bit << 15);
                        if (bit & 0x8000)
                            addr += xinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            addr += yinc;       /* Increment y */
                        }
                    }
                } else
                {
                    bit = ~bit;
                    for (loopcnt = dx; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr &= bit;
                        bit = (bit >> 1) | (bit << 15);
                        if (!(bit & 0x8000))
                            addr += xinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            addr += yinc;       /* Increment y */
                        }
                    }
                }
                break;

            case 1:              /* Replace */
                if (color & 0x0001)
                {
                    for (loopcnt = dx; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr |= bit;
                        else
                            *(short *)addr &= ~bit;
                        bit = (bit >> 1) | (bit << 15);
                        if (bit & 0x8000)
                            addr += xinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            addr += yinc;       /* Increment y */
                        }
                    }
                } else
                {
                    bit = ~bit;
                    for (loopcnt = dx; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        *(short *)addr &= bit;
                        bit = (bit >> 1) | (bit << 15);
                        if (!(bit & 0x8000))
                            addr += xinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            addr += yinc;       /* Increment y */
                        }
                    }
                }
            }
        } else
        {
            e1 = 2 * dx;
            eps = -dy;
            e2 = 2 * dy;

            switch (mode)
            {
            case 4:              /* Reverse transparent */
                if (color & 0x0001)
                {
                    bit = ~bit;
                    for (loopcnt = dy; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr &= bit;
                        addr += yinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            bit = (bit >> 1) | (bit << 15);
                            if (!(bit & 0x8000))
                                addr += xinc;
                        }
                    }
                } else
                {
                    for (loopcnt = dy; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr |= bit;
                        addr += yinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            bit = (bit >> 1) | (bit << 15);
                            if (bit & 0x8000)
                                addr += xinc;
                        }
                    }
                }
                break;

            case 3:              /* xor */
                for (loopcnt = dy; loopcnt >= 0; loopcnt--)
                {
                    linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                    if (linemask & 0x8000)
                        *(short *)addr ^= bit;
                    addr += yinc;
                    eps += e1;
                    if (eps >= 0)
                    {
                        eps -= e2;
                        bit = (bit >> 1) | (bit << 15);
                        if (bit & 0x8000)
                            addr += xinc;
                    }
                }
                break;

            case 2:              /* or */
                if (color & 0x0001)
                {
                    for (loopcnt = dy; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr |= bit;
                        addr += yinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            bit = (bit >> 1) | (bit << 15);
                            if (bit & 0x8000)
                                addr += xinc;
                        }
                    }
                } else
                {
                    bit = ~bit;
                    for(loopcnt = dy; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr &= bit;
                        addr += yinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            bit = (bit >> 1) | (bit << 15);
                            if (!(bit & 0x8000))
                                addr += xinc;
                        }
                    }
                }
                break;

            case 1:              /* Replace */
                if (color & 0x0001)
                {
                    for (loopcnt = dy; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        if (linemask & 0x8000)
                            *(short *)addr |= bit;
                        else
                            *(short *)addr &= ~bit;
                        addr += yinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            bit = (bit >> 1) | (bit << 15);
                            if (bit & 0x8000)
                                addr += xinc;
                        }
                    }
                } else
                {
                    bit = ~bit;
                    for (loopcnt = dy; loopcnt >= 0; loopcnt--)
                    {
                        linemask = (linemask >> 15) | (linemask << 1);     /* Get next bit of line style */
                        *(short *)addr &= bit;
                        addr += yinc;
                        eps += e1;
                        if (eps >= 0)
                        {
                            eps -= e2;
                            bit = (bit >> 1) | (bit << 15);
                            if (!(bit & 0x8000))
                                addr += xinc;
                        }
                    }
                }
            }
        }
        adr += 2;
        color >>= 1;    /* Shift color index: next plane */
    }

}


/* A line that is not horizontal, at four planes or more; dx >= 0 */
static void __attribute__((noinline)) line_all_planes(const LineOp *opp, long x1, long y1, long dx, long dy,
            unsigned short pattern)
{
    LineOp op = *opp;
    unsigned short *addr;
    unsigned short linemask, bit;
    long yinc, eps, e1, e2, n;

    yinc = op.wrap;
    if (dy < 0)
    {
        dy = -dy;
        yinc = -yinc;
    }

    addr = op.base + y1 * op.wrap + (x1 >> 4) * op.planes;
    bit = 0x8000 >> (x1 & 15);
    linemask = pattern;
    linemask = (linemask >> 1) | (linemask << 15);     /* Back one, for the loop */

    /*
     * The same steps as the original per-plane loops, taken once for all
     * planes.  Pixels that fall in one word of one row are gathered and
     * written together.
     */
    if (dx >= dy)
    {
        unsigned short m = 0, p = 0;

        e1 = 2 * dy;
        eps = -dx;
        e2 = 2 * dx;
        for (n = dx; n >= 0; n--)
        {
            linemask = (linemask >> 15) | (linemask << 1);
            m |= bit;
            if (linemask & 0x8000)
                p |= bit;
            bit >>= 1;
            if (!bit)
            {
                put_word(&op, addr, m, p);
                m = p = 0;
                bit = 0x8000;
                addr += op.planes;
            }
            eps += e1;
            if (eps >= 0)
            {
                eps -= e2;
                if (m)
                {
                    put_word(&op, addr, m, p);
                    m = p = 0;
                }
                addr += yinc;
            }
        }
        if (m)
            put_word(&op, addr, m, p);
    } else
    {
        /*
         * One pixel a row.  What each plane does with it changes only when
         * the line moves to the next bit, so that is worked out then.  Two
         * neighbouring planes are one long word, handled together: a pixel
         * of the pattern is (old & keep_on) ^ (bits & sel), a gap in it
         * old & keep_off.
         */
        unsigned long keep_on = 0, keep_off = 0, bits = 0, s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        unsigned long selm[4], ka = op.keep_and * 0x10001UL, kx = op.keep_xor * 0x10001UL;
        int k, planes = op.planes, step = 1;

        for (k = 0; k < 4; k++)
            selm[k] = ((op.sel >> (2 * k)) & 1 ? 0xffff0000UL : 0) | ((op.sel >> (2 * k + 1)) & 1 ? 0xffffUL : 0);
        if (planes == 1)
            selm[0] = op.sel & 1 ? 0xffffUL : 0;    /* one word: the low half */

        e1 = 2 * dx;
        eps = -dy;
        e2 = 2 * dy;
        for (n = dy; n >= 0; n--)
        {
            if (step)
            {
                bits = planes == 1 ? bit : bit * 0x10001UL;
                keep_on = ((bits & ka) ^ kx) | ~bits;
                keep_off = kx | ~bits;
                s0 = bits & selm[0];
                s1 = bits & selm[1];
                s2 = bits & selm[2];
                s3 = bits & selm[3];
                step = 0;
            }
            linemask = (linemask >> 15) | (linemask << 1);
            if (linemask & 0x8000)
            {
                switch (planes)
                {
                case 8:
                    ((unsigned long *)addr)[3] = (((unsigned long *)addr)[3] & keep_on) ^ s3;
                    ((unsigned long *)addr)[2] = (((unsigned long *)addr)[2] & keep_on) ^ s2;
                    /* fall through */
                case 4:
                    ((unsigned long *)addr)[1] = (((unsigned long *)addr)[1] & keep_on) ^ s1;
                    /* fall through */
                case 2:
                    ((unsigned long *)addr)[0] = (((unsigned long *)addr)[0] & keep_on) ^ s0;
                    break;
                case 1:
                    *addr = (*addr & (unsigned short)keep_on) ^ (unsigned short)s0;
                    break;
                default:
                    put_word(&op, addr, bit, bit);
                    break;
                }
            } else if (keep_off != 0xffffffffUL)
            {
                switch (planes)
                {
                case 8:
                    ((unsigned long *)addr)[3] &= keep_off;
                    ((unsigned long *)addr)[2] &= keep_off;
                    /* fall through */
                case 4:
                    ((unsigned long *)addr)[1] &= keep_off;
                    /* fall through */
                case 2:
                    ((unsigned long *)addr)[0] &= keep_off;
                    break;
                case 1:
                    *addr &= (unsigned short)keep_off;
                    break;
                default:
                    put_word(&op, addr, bit, 0);
                    break;
                }
            }
            addr += yinc;
            eps += e1;
            if (eps >= 0)
            {
                eps -= e2;
                step = 1;
                bit >>= 1;
                if (!bit)
                {
                    bit = 0x8000;
                    addr += planes;
                }
            }
        }
    }

}


long CDECL c_line_draw(Virtual *vwk, long x1, long y1, long x2, long y2,
            long pattern, long colour, long mode)
{
    Workstation *wk = vwk->real_address;
    LineOp op;
    unsigned long color, dummy;
    long dx, dy;

    /* Don't understand any table operations yet. */
    if ((long) vwk & 1)
        return -1;

    if (mode < 1 || mode > 4)
        return 1;

    if (!clip_line(vwk, &x1, &y1, &x2, &y2))
        return 1;

    c_get_colours(vwk, colour, &color, &dummy);

    if (y1 != y2 && wk->screen.mfdb.bitplanes < 4)
    {
        line_per_plane(vwk, x1, y1, x2, y2, pattern, color, mode);
        return 1;
    }

    op.base = (unsigned short *)wk->screen.mfdb.address;
    op.wrap = wk->screen.wrap >> 1;
    op.planes = wk->screen.mfdb.bitplanes;
    switch (mode)
    {
    case 1:                     /* replace */
        op.keep_and = 0;
        op.keep_xor = 0;
        op.sel = color;
        break;
    case 2:                     /* transparent */
        op.keep_and = 0xffff;
        op.keep_xor = 0xffff;
        op.sel = color;
        break;
    case 3:                     /* xor */
        op.keep_and = 0;
        op.keep_xor = 0xffff;
        op.sel = ~0U;
        break;
    default:                    /* reverse transparent */
        op.keep_and = 0xffff;
        op.keep_xor = 0xffff;
        op.sel = ~color;
        break;
    }

    /* Make x axis always going up */
    dy = y2 - y1;
    dx = x2 - x1;
    if (dx < 0)
    {
        /* if delta x < 0 then draw from point 2 to 1 */
        dx = -dx;
        dy = -dy;
        x1 = x2;
        y1 = y2;
    }

    if (dy == 0)
    {
        hline(&op, x1, x1 + dx, y1, (unsigned short)pattern);
        return 1;
    }

    line_all_planes(&op, x1, y1, dx, dy, (unsigned short)pattern);

    return 1;
}
