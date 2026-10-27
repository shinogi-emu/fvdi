/*
 * Bitplane fill routines
 *
 * Copyright 2005, Johan Klockars 
 * Copyright 2002 The EmuTOS development team
 *
 * This software is licensed under the GNU General Public License.
 * Please, see LICENSE.TXT for further information.
 */

#include "fvdi.h"
#include "relocate.h"
#include "driver.h"
#include "bitplane.h"

#define BOOL int
#define TRUE 1
#define FALSE 0


#define GetMemW(addr) ((unsigned long)*(unsigned short *)(addr))
#define SetMemW(addr, val) *(unsigned short *)(addr) = val


/*
 * draw_rect - draw one or more horizontal lines
 *
 * Rewritten in C instead of assembler this routine is now looking
 * very complicated. Since in assembler you can pass parameters in
 * registers, you have an easy game to be both, fast and flexible.
 *
 * In C we need to double the code a bit, like loops and such things.
 * So, the following code works as follows:
 *
 * 1.  Fill variables with all values for masks, fringes etc.
 * 2.  Decide, if we have the situation of writing just one word
 * 3.  In any case we choose the writing mode now
 * 4.  Then loop through the y axis (just one pass for horzline)
 * 5.  Inner loop through the planes
 * 6a. If just one word, mask out the bits and process the action
 * 6b. else mask out left fringe, write words, process the right fringe
 */

static void __attribute__((noinline)) draw_rect_per_plane(Virtual *vwk, long x1, long y1, long w, long h, short *patternptr, unsigned short fillcolor, long mode)
{
    unsigned short leftmask, rightmask, patmsk;
    unsigned short *addr;
    int x2 = x1 + w - 1;
    int y2 = y1 + h - 1;
    int dx, y, yinc;
    int patadd;               /* advance for multiplane patterns */
    int leftpart;
    int rightpart;
    int planes;
    unsigned short pattern, bits, help;

    mode -= 1;

    /* Get the pattern with which the line is to be drawn. */
    planes = vwk->real_address->screen.mfdb.bitplanes;
    dx = x2 - x1 - 16;      /* Width of line - one word */
    addr = (unsigned short *) vwk->real_address->screen.mfdb.address;
    addr += (x1 / 16) * vwk->real_address->screen.mfdb.bitplanes;
    addr += (long)y1 * vwk->real_address->screen.wrap / 2;
#if 0
    patmsk = vwk->patmsk;                   /* Which pattern to start with */
    patadd = vwk->multifill ? 16 : 0;       /* Multi plane pattern offset */
#else
    patmsk = 0x0f;
    patadd = 0;
#endif
    leftpart  = x1 & 0xf;               /* Left fringe */
    rightpart = x2 & 0xf;               /* Right fringe */
    leftmask  = ~(0xffff >> leftpart);  /* Origin for not left fringe lookup */
    rightmask =  0x7fff >> rightpart;   /* Origin for right fringe lookup */
    yinc = vwk->real_address->screen.wrap / 2 - planes;

    /* Check if we have to process just one single word on screen */
    if (dx + leftpart < 0) {
        switch (mode) {
        case 3:  /* nor */
            for(y = y1; y <= y2; y++ ) {
                int plane;
                int patind = patmsk & y;        /* Pattern to start with */
                unsigned short color = fillcolor;

                /* Init adress counter */
                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    pattern = patternptr[patind];
                        /* Isolate the necessary pixels */
                        bits = *addr;
                        help = bits;

                    if (color & 0x0001) {
                        bits |= ~pattern;       /* Complement of mask with source */
                    } else {
                        bits &= pattern;        /* Complement of mask with source */
                    }
                        help ^= bits;           /* Isolate changed bits */
                        help &= leftmask | rightmask;   /* Isolate changed bits outside of fringe */
                        bits ^= help;           /* Restore them to original states */
                        *addr = bits;
                    addr++;                     /* Next plane */
                    patind += patadd;
                    color >>= 1;
                }
                addr += yinc;                   /* Next scanline */
            }
            break;

        case 2:  /* xor */
            for(y = y1; y <= y2; y++) {
                int plane;
                int patind = patmsk & y;        /* Pattern to start with */

                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    pattern = patternptr[patind];

                    /* Isolate the necessary pixels */
                    bits = *addr;
                    help = bits;
                    bits ^= pattern;
                    help ^= bits;        /* xor result with source - now have pattern */
                    help &= leftmask | rightmask;       /* Isolate bits */
                    bits ^= help;       /* Restore states of bits outside of fringe */
                    *addr = bits;
                    addr++;            /* Next plane */
                    patind += patadd;
                }
                addr += yinc;           /* Next scanline */
            }
            break;

        case 1:  /* or */
            for(y = y1; y <= y2; y++) {
                int plane;
                int patind = patmsk & y;        /* Pattern to start with */
                unsigned short color = fillcolor;

                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    pattern = patternptr[patind];
                        /* Isolate the necessary pixels */
                        bits = *addr;
                        help = bits;

                    if (color & 0x0001) {
			pattern &= ~(leftmask | rightmask);
			bits |= pattern;
                    } else {
                        bits &= ~pattern;        /* Complement of mask with source */
                        help ^= bits;           /* Isolate changed bits */
                        help &= leftmask | rightmask;   /* Isolate bits */
                        bits ^= help;           /* Restore them to original states */
                    }
                    *addr = bits;
                    addr++;               /* Next plane */
                    patind += patadd;
                    color >>= 1;
                }
                addr += yinc;           /* Next scanline */
            }
            break;

        default: /* Replace */
            for(y = y1; y <= y2; y++) {
                int patind = patmsk & y;        /* Pattern to start with */
                int plane;
                unsigned short color = fillcolor;

                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    pattern = 0;

                    /* Isolate the necessary pixels */
                    bits = *addr;
                    if (color & 0x0001) {
                        pattern = patternptr[patind];
#if 1
		    }
#endif
                    bits ^= pattern;
                    bits &= leftmask | rightmask; /* Isolate the bits outside the fringe */
                    bits ^= pattern;    /* Restore the bits outside the fringe */
#if 0
		    } else {
                    bits &= leftmask | rightmask; /* Isolate the bits outside the fringe */
		    }
#endif
                    *addr = bits;
                    addr++;             /* Next plane */
                    patind += patadd;
                    color >>= 1;
                }
                addr += yinc;           /* Next scanline */
            }
        }

    }
    /* Write the left fringe, then some whole words, then the right fringe */
    else {
        leftpart = 16 - leftpart;       /* Precalculate delta for the left */

        switch (mode) {
        case 3:  /* nor */
            for(y = y1; y <= y2; y++) {
                int plane;
                int patind = patmsk & y;        /* Pattern to start with */
                unsigned short color = fillcolor;

                /* Init adress counter */
                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    unsigned short *adr = addr;
                    int pixels = dx;
                    pattern = patternptr[patind];

                    if (color & 0x0001) {
                        int bw;
                        pattern = ~pattern;
                        /* Check if the line is completely contained within one word */

                        /* Draw the left fringe */
                        if (leftmask) {
                            bits = *adr;
                            help = bits;
                            bits |= pattern;    /* Complement of mask with source */
                            help ^= bits;       /* Isolate changed bits */
                            help &= leftmask;   /* Isolate changed bits outside of fringe */
                            bits ^= help;       /* Restore them to original states */
                            *adr = bits;

                            adr += planes;
                            pixels -= leftpart;
                        }
                        /* Full bytes */
                        for(bw = pixels >> 4; bw >= 0; bw--) {
                            *adr |= pattern;
                            adr += planes;
                        }
                        /* Draw the right fringe */
                        if (~rightmask) {
                            bits = *adr;
                            help = bits;
                            bits |= pattern;    /* Complement of mask with source */
                            help ^= bits;       /* Isolate changed bits */
                            help &= rightmask;  /* Isolate changed bits outside of fringe */
                            bits ^= help;       /* Restore them to original states */
                            *adr = bits;
                        }
                        pattern = ~pattern;
                    } else {
                        int bw;

                        /* Draw the left fringe */
                        if (leftmask) {
                            bits = *adr;
                            help = bits;
                            bits &= pattern;    /* Complement of mask with source */
                            help ^= bits;       /* Isolate changed bits */
                            help &= leftmask;   /* Isolate changed bits outside of fringe */
                            bits ^= help;       /* Restore them to original states */
                            *adr = bits;

                            adr += planes;
                            pixels -= leftpart;
                        }
                        /* Full bytes */
                        for(bw = pixels >> 4; bw >= 0; bw--) {
                            *adr &= pattern;
                            adr += planes;
                        }
                        /* Draw the right fringe */
                        if (~rightmask) {
                            bits = *adr;
                            help = bits;
                            bits &= pattern;    /* Complement of mask with source */
                            help ^= bits;       /* Isolate changed bits */
                            help &= rightmask;  /* Isolate changed bits outside of fringe */
                            bits ^= help;       /* Restore them to original states */
                            *adr = bits;
                        }
                    }
                    addr++;                     /* Next plane */
                    patind += patadd;
                    color >>= 1;
                }
                addr += yinc;           /* Next scanline */
            }
            break;

        case 2:  /* xor */
            for(y = y1; y <= y2; y++ ) {
                int plane;
                int patind = patmsk & y;        /* Pattern to start with */

                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    unsigned short *adr = addr;
                    int pixels = dx;
                    int bw;
                    pattern = patternptr[patind];

                    /* Draw the left fringe */
                    if (leftmask) {
                        bits = *adr;
                        help = bits;
                        bits ^= pattern;        /* xor the pattern with the source */
                        help ^= bits;           /* xor result with source - now have pattern */
                        help &= leftmask;       /* Isolate changed bits outside of fringe */
                        bits ^= help;           /* Restore states of bits outside of fringe */
                        *adr = bits;

                        adr += planes;
                        pixels -= leftpart;
                    }
                    /* Full bytes */
                    for(bw = pixels >> 4; bw >= 0; bw--) {
                        *adr ^= pattern;
                        adr += planes;
                    }
                    /* Draw the right fringe */
                    if (~rightmask) {
                        bits = *adr;
                        help = bits;
                        bits ^= pattern;    /* xor the pattern with the source */
                        help ^= bits;               /* xor result with source - now have pattern */
                        help &= rightmask;    /* Isolate changed bits outside of fringe */
                        bits ^= help;               /* Restore states of bits outside of fringe */
                        *adr = bits;
                    }
                    addr++;                  /* Next plane */
                    patind += patadd;
                }
                addr += yinc;           /* Next scanline */
            }
            break;

        case 1:  /* or */
            for(y = y1; y <= y2; y++) {
                int plane;
                int patind = patmsk & y;        /* Pattern to start with */
                unsigned short color = fillcolor;

                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    unsigned short *adr = addr;
                    int pixels = dx;
                    pattern = patternptr[patind];

                    if (color & 0x0001) {
                        int bw;

                        /* Draw the left fringe */
                        if (leftmask) {
                            bits = *adr;
			    help = pattern & ~leftmask;
                            bits |= help;
                            *adr = bits;

                            adr += planes;
                            pixels -= leftpart;
                        }
                        /* Full bytes */
                        for(bw = pixels >> 4; bw >= 0; bw--) {
                            *adr |= pattern;
                            adr += planes;
                        }
                        /* Draw the right fringe */
                        if (~rightmask) {
                            bits = *adr;
			    help = pattern & ~rightmask;
                            bits |= help;
                            *adr = bits;
                        }
                    } else {
                        int bw;

                        pattern = ~pattern;

                        /* Draw the left fringe */
                        if (leftmask) {
                            bits = *adr;
                            help = bits;
                            bits &= pattern;    /* Complement of mask with source */
                            help ^= bits;       /* Isolate changed bits */
                            help &= leftmask;   /* Isolate changed bits outside of fringe */
                            bits ^= help;       /* Restore them to original states */
                            *adr = bits;

                            adr += planes;
                            pixels -= leftpart;
                        }
                        /* Full bytes */
                        for(bw = pixels >> 4; bw >= 0; bw--) {
                            *adr &= pattern;
                            adr += planes;
                        }
                        /* Draw the right fringe */
                        if (~rightmask) {
                            bits = *adr;
                            help = bits;
                            bits &= pattern;    /* Complement of mask with source */
                            help ^= bits;       /* Isolate changed bits */
                            help &= rightmask;  /* Isolate changed bits outside of fringe */
                            bits ^= help;       /* Restore them to original states */
                            *adr = bits;
                        }
                        pattern = ~pattern;
                    }
                    addr++;                     /* Next plane */
                    patind += patadd;
                    color >>= 1;
                }
                addr += yinc;           /* Next scanline */
            }
            break;

        default: /* Replace */
            for(y = y1; y <= y2; y++ ) {
                int patind = patmsk & y;        /* Ppattern to start with */
                int plane;
                unsigned short color = fillcolor;

                for(plane = planes - 1; plane >= 0; plane--) {
                    /* Load values fresh for this bitplane */
                    int bw;
                    unsigned short *adr = addr;
                    int pixels = dx;
                    pattern = 0;

                    if (color & 0x0001)
                        pattern = patternptr[patind];

                    /* Draw the left fringe */
                    if (leftmask) {
                        bits = *adr;
                        bits ^= pattern;        /* xor the pattern with the source */
                        bits &= leftmask;       /* Isolate the bits outside the fringe */
                        bits ^= pattern;        /* Restore the bits outside the fringe */
                        *adr = bits;

                        adr += planes;
                        pixels -= leftpart;
                    }
                    /* Full words */
                    for(bw = pixels >> 4; bw >= 0; bw--) {
                        *adr = pattern;
                        adr += planes;
                    }
                    /* Draw the right fringe */
                    if (~rightmask) {
                        bits = *adr;
                        bits ^= pattern;        /* xor the pattern with the source */
                        bits &= rightmask;      /* isolate the bits outside the fringe */
                        bits ^= pattern;        /* Restore the bits outside the fringe */
                        *adr = bits;
                    }
                    addr++;                     /* Next plane */
                    patind += patadd;
                    color >>= 1;
                }
                addr += yinc;           /* Next scanline */
            }
        }
    }
}



/*
 * A fill more than one word wide, at two planes or more, with the same
 * pattern word on every row it covers - a solid fill, the usual kind.
 * In one plane every mode is new = (old & keep) ^ set inside the pixels
 * filled:
 *
 *   mode               keep        set (colour bit 1)   set (colour bit 0)
 *   replace            0           pattern              0
 *   transparent        ~pattern    pattern              0
 *   xor                ~0          pattern              pattern
 *   reverse transp.    pattern     ~pattern             0
 *
 * keep is the same for every plane, and with one pattern word it is the
 * same for every row, so it is all worked out once.  Rows are then written
 * a column at a time, all planes of a column together: the planes are
 * interleaved, so that is one run of memory, and two planes are one long
 * word.  Patterned and one-plane fills stay with draw_rect_per_plane(),
 * which is quicker for them.
 */
static void __attribute__((noinline)) fill_columns(Virtual *vwk, long x1, long y1, long w, long h,
            unsigned short pat, unsigned short fillcolor, long mode)
{
    Workstation *wk = vwk->real_address;
    int planes = wk->screen.mfdb.bitplanes;
    long x2 = x1 + w - 1;
    long wrap = wk->screen.wrap >> 1;
    long middle = (x2 >> 4) - (x1 >> 4) - 1;
    unsigned short lm = 0xffff >> (x1 & 15);
    unsigned short rm = 0xffff << (15 - (x2 & 15));
    unsigned short *row = (unsigned short *)wk->screen.mfdb.address + y1 * wrap + (x1 >> 4) * planes;
    unsigned short keep, k_l, k_r, set[8], setl[8], setr[8];
    unsigned short color = fillcolor;
    unsigned long keepl, l0, l1, l2, l3;
    int k;

    switch (mode) {
    case 2:     /* transparent */
        keep = ~pat;
        for (k = 0; k < planes; k++, color >>= 1)
            set[k] = color & 1 ? pat : 0;
        break;
    case 3:     /* xor */
        keep = 0xffff;
        for (k = 0; k < planes; k++)
            set[k] = pat;
        break;
    case 4:     /* reverse transparent */
        keep = pat;
        for (k = 0; k < planes; k++, color >>= 1)
            set[k] = color & 1 ? ~pat : 0;
        break;
    default:    /* replace, as draw_rect_per_plane() takes anything else */
        keep = 0;
        for (k = 0; k < planes; k++, color >>= 1)
            set[k] = color & 1 ? pat : 0;
        break;
    }
    for (k = 0; k < planes; k++) {
        setl[k] = set[k] & lm;
        setr[k] = set[k] & rm;
    }
    k_l = keep | ~lm;
    k_r = keep | ~rm;
    keepl = ((unsigned long)keep << 16) | keep;
    l0 = ((unsigned long)set[0] << 16) | set[1];
    l1 = planes > 2 ? ((unsigned long)set[2] << 16) | set[3] : 0;
    l2 = planes > 4 ? ((unsigned long)set[4] << 16) | set[5] : 0;
    l3 = planes > 4 ? ((unsigned long)set[6] << 16) | set[7] : 0;

    for (; h > 0; h--, row += wrap) {
        unsigned short *p = row;
        unsigned long *lp;
        long n = middle;

        for (k = 0; k < planes; k++)
            p[k] = (p[k] & k_l) ^ setl[k];
        lp = (unsigned long *)(p + planes);

        if (keep == 0) {
            switch (planes) {
            case 8:
                for (; n > 0; n--, lp += 4) {
                    lp[0] = l0;
                    lp[1] = l1;
                    lp[2] = l2;
                    lp[3] = l3;
                }
                break;
            case 4:
                for (; n > 0; n--, lp += 2) {
                    lp[0] = l0;
                    lp[1] = l1;
                }
                break;
            default:
                for (; n > 0; n--)
                    *lp++ = l0;
                break;
            }
        } else {
            switch (planes) {
            case 8:
                for (; n > 0; n--, lp += 4) {
                    lp[0] = (lp[0] & keepl) ^ l0;
                    lp[1] = (lp[1] & keepl) ^ l1;
                    lp[2] = (lp[2] & keepl) ^ l2;
                    lp[3] = (lp[3] & keepl) ^ l3;
                }
                break;
            case 4:
                for (; n > 0; n--, lp += 2) {
                    lp[0] = (lp[0] & keepl) ^ l0;
                    lp[1] = (lp[1] & keepl) ^ l1;
                }
                break;
            default:
                for (; n > 0; n--, lp++)
                    *lp = (*lp & keepl) ^ l0;
                break;
            }
        }

        p = (unsigned short *)lp;
        for (k = 0; k < planes; k++)
            p[k] = (p[k] & k_r) ^ setr[k];
    }
}


static void draw_rect(Virtual *vwk, long x1, long y1, long w, long h, short *patternptr, unsigned short fillcolor, long mode)
{
    int planes = vwk->real_address->screen.mfdb.bitplanes;
    long i, rows = h < 16 ? h : 16;
    unsigned short pat = patternptr[y1 & 15];

    if ((planes == 2 || planes == 4 || planes == 8) && ((x1 + w - 1) >> 4) > (x1 >> 4) && h > 0) {
        for (i = 1; i < rows; i++) {
            if ((unsigned short)patternptr[(y1 + i) & 15] != pat)
                break;
        }
        if (i == rows) {
            fill_columns(vwk, x1, y1, w, h, pat, fillcolor, mode);
            return;
        }
    }
    draw_rect_per_plane(vwk, x1, y1, w, h, patternptr, fillcolor, mode);
}

long CDECL c_fill_area(Virtual *vwk, long x, long y, long w, long h, short *pattern,
            long colour, long mode, long interior_style)
{
  unsigned long foreground;
  unsigned long background;

  /* Don't understand any table operations yet */
  if ((long)vwk & 1)
    return -1;

  c_get_colours((Virtual *)((long)vwk & ~1), colour, &foreground, &background);

  (void) interior_style;
  draw_rect(vwk, x, y, w, h, pattern, foreground, mode);

  return 1;
}
