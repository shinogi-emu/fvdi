/*
 * A 16 bit graphics blit routine, by Johan Klockars.
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

#ifdef FVDI_DEBUG
static void debug_out(const char *text1, int w, int old_w, int h, int src_x, int src_y, int dst_x, int dst_y)
{
    PRINTF(("%s%d", text1, w));
    if (old_w > 0) {
        PRINTF(("(%d))", old_w));
    }
    PRINTF((",%d from %d,%d to %d,%d\n", h, src_x, src_y, dst_x, dst_y));
}
#endif


#ifdef __mcoldfire__
#define OR_W(val, ptr, dec, inc) \
	" move.w " dec "(%[" ptr "])" inc ",%[x4]\n" \
	" or.l %[" val "],%[x4]\n" \
	" move.w %[x4],(%[" ptr "])\n"
#define DBRA(reg, label) \
        " subq.l #1," reg "\n" \
        " jbpl " label "\n"
#define REGL long
#else
#define OR_W(val, ptr, dec, inc)   " or.w %[" val "]," dec "(%[" ptr "])" inc "\n"
#define DBRA(reg, label) " dbra " reg "," label "\n"
#define REGL short
#endif


/*
 * The copy loops below are unrolled eight longs deep.  On a 68000 a screen
 * copy is limited by the loop overhead rather than by the moves themselves,
 * and 'move.l (a0)+,(a1)+' needs no scratch register, so the unrolling is free.
 */
#define REP2(x) x x
#define REP8(x) x x x x x x x x


/*
 * The 16 raster operations as (source, destination) expressions.
 * 'operation' is fixed for a whole blit, but gcc will not unswitch a switch
 * on it out of the pixel loop, so the loops below are generated once per
 * operation and the dispatch is done by hand, before the loop is entered.
 */
#define ROP_0(s, d)     (0)
#define ROP_1(s, d)     ((s) & (d))
#define ROP_2(s, d)     ((s) & ~(d))
#define ROP_3(s, d)     (s)
#define ROP_4(s, d)     (~(s) & (d))
#define ROP_5(s, d)     (d)
#define ROP_6(s, d)     ((s) ^ (d))
#define ROP_7(s, d)     ((s) | (d))
#define ROP_8(s, d)     (~((s) | (d)))
#define ROP_9(s, d)     (~((s) ^ (d)))
#define ROP_10(s, d)    (~(d))
#define ROP_11(s, d)    ((s) | ~(d))
#define ROP_12(s, d)    (~(s))
#define ROP_13(s, d)    (~(s) | (d))
#define ROP_14(s, d)    (~((s) & (d)))
#define ROP_15(s, d)    (-1)

#define ROP_SWITCH(loop) \
    switch(operation) { \
    case 0: \
    default: \
        loop(ROP_0); \
        break; \
    case 1: \
        loop(ROP_1); \
        break; \
    case 2: \
        loop(ROP_2); \
        break; \
    case 3: \
        loop(ROP_3); \
        break; \
    case 4: \
        loop(ROP_4); \
        break; \
    case 5: \
        loop(ROP_5); \
        break; \
    case 6: \
        loop(ROP_6); \
        break; \
    case 7: \
        loop(ROP_7); \
        break; \
    case 8: \
        loop(ROP_8); \
        break; \
    case 9: \
        loop(ROP_9); \
        break; \
    case 10: \
        loop(ROP_10); \
        break; \
    case 11: \
        loop(ROP_11); \
        break; \
    case 12: \
        loop(ROP_12); \
        break; \
    case 13: \
        loop(ROP_13); \
        break; \
    case 14: \
        loop(ROP_14); \
        break; \
    case 15: \
        loop(ROP_15); \
        break; \
    }


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

static void s_blit_copy(PIXEL *src_addr, int src_line_add,
    PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
    short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
#ifdef BOTH
    PIXEL_32 v32;

#define COPY_LONG \
            " move.l (%[src_addr])+,%[v32]\n" \
            " move.l %[v32],(%[dst_addr])+\n" \
            " move.l %[v32],(%[dst_addr_fast])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,%[v32]\n" \
            " move.w %[v32],(%[dst_addr])+\n" \
            " move.w %[v32],(%[dst_addr_fast])+\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l (%[src_addr])+,(%[dst_addr])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,(%[dst_addr])+\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void s_blit_or(PIXEL *src_addr, int src_line_add,
        PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
        short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
    PIXEL_32 v32;

#ifdef BOTH
#define COPY_LONG \
            " move.l (%[src_addr])+,%[v32]\n" \
            " or.l %[v32],(%[dst_addr])+\n" \
            " or.l %[v32],(%[dst_addr_fast])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,%[v32]\n" \
            OR_W("v32","dst_addr","","+") \
            OR_W("v32","dst_addr_fast","","+")
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l (%[src_addr])+,%[v32]\n" \
            " or.l %[v32],(%[dst_addr])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,%[v32]\n" \
            OR_W("v32","dst_addr","","+")
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void s_blit(PIXEL *src_addr, int src_line_add,
     PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
     short int w, short int h, short int operation)
{
    short int i, j;
    PIXEL v, vs, vd;
    PIXEL_32 v32, v32s, v32d;
    PIXEL_32 *src_addr32;
    PIXEL_32 *dst_addr32;
#ifdef BOTH
    PIXEL_32 *dst_addr_fast32;
#endif

#ifdef BOTH
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *src_addr++; \
            vd = *dst_addr_fast; \
            v = rop(vs, vd); \
            *dst_addr_fast++ = v; \
            *dst_addr++ = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        dst_addr_fast32 = (PIXEL_32 *)dst_addr_fast; \
        for(j = (w >> 1) - 1; j >= 0; j--) { \
            v32s = *src_addr32++; \
            v32d = *dst_addr_fast32; \
            v32 = rop(v32s, v32d); \
            *dst_addr_fast32++ = v32; \
            *dst_addr32++ = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast = (PIXEL *)dst_addr_fast32; \
        dst_addr_fast += dst_line_add; \
    }
#else
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *src_addr++; \
            vd = *dst_addr; \
            v = rop(vs, vd); \
            *dst_addr++ = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        for(j = (w >> 1) - 1; j >= 0; j--) { \
            v32s = *src_addr32++; \
            v32d = *dst_addr32; \
            v32 = rop(v32s, v32d); \
            *dst_addr32++ = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
    }
#endif

    (void) dst_addr_fast;
    /* Tell gcc that this cannot happen (already checked in c_blit_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    ROP_SWITCH(BLIT_LOOP);

#undef BLIT_LOOP
}


static void
s_pan_backwards_copy(PIXEL *src_addr, int src_line_add,
                     PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
                     short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
#ifdef BOTH
    PIXEL_32 v32;

#define COPY_LONG \
            " move.l -(%[src_addr]),%[v32]\n" \
            " move.l %[v32],-(%[dst_addr])\n" \
            " move.l %[v32],-(%[dst_addr_fast])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),%[v32]\n" \
            " move.w %[v32],-(%[dst_addr])\n" \
            " move.w %[v32],-(%[dst_addr_fast])\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l -(%[src_addr]),-(%[dst_addr])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),-(%[dst_addr])\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void
s_pan_backwards_or(PIXEL *src_addr, int src_line_add,
                   PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
                   short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
    PIXEL_32 v32;

#ifdef BOTH
#define COPY_LONG \
            " move.l -(%[src_addr]),%[v32]\n" \
            " or.l %[v32],-(%[dst_addr])\n" \
            " or.l %[v32],-(%[dst_addr_fast])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),%[v32]\n" \
            OR_W("v32","dst_addr","-","") \
            OR_W("v32","dst_addr_fast","-","")
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l -(%[src_addr]),%[v32]\n" \
            " or.l %[v32],-(%[dst_addr])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),%[v32]\n" \
            OR_W("v32","dst_addr","-","")
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void
s_pan_backwards(PIXEL *src_addr, int src_line_add,
                PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
                short int w, short int h, short int operation)
{
    short int i, j;
    PIXEL v, vs, vd;
    PIXEL_32 v32, v32s, v32d;
    PIXEL_32 *src_addr32;
    PIXEL_32 *dst_addr32;
#ifdef BOTH
    PIXEL_32 *dst_addr_fast32;
#endif

#ifdef BOTH
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *--src_addr; \
            vd = *--dst_addr_fast; \
            v = rop(vs, vd); \
            *dst_addr_fast = v; \
            *--dst_addr = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        dst_addr_fast32 = (PIXEL_32 *)dst_addr_fast; \
        for (j = (w >> 1) - 1; j >= 0; j--) \
        { \
            v32s = *--src_addr32; \
            v32d = *--dst_addr_fast32; \
            v32 = rop(v32s, v32d); \
            *dst_addr_fast32 = v32; \
            *--dst_addr32 = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast = (PIXEL *)dst_addr_fast32; \
        dst_addr_fast += dst_line_add; \
    }
#else
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *--src_addr; \
            vd = *--dst_addr; \
            v = rop(vs, vd); \
            *dst_addr = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        for (j = (w >> 1) - 1; j >= 0; j--) \
        { \
            v32s = *--src_addr32; \
            v32d = *--dst_addr32; \
            v32 = rop(v32s, v32d); \
            *dst_addr32 = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
    }
#endif

    (void) dst_addr_fast;
    /* Tell gcc that this cannot happen (already checked in c_blit_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    ROP_SWITCH(BLIT_LOOP);

#undef BLIT_LOOP
}

#define BOTH_WAS_ON
#endif
#undef BOTH

/*
 * The functions below are exact copies of those above.
 * The '#undef BOTH' makes sure that this works as it should
 * when no shadow buffer is available
 */

static void blit_copy(PIXEL *src_addr, int src_line_add,
    PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
    short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
#ifdef BOTH
    PIXEL_32 v32;

#define COPY_LONG \
            " move.l (%[src_addr])+,%[v32]\n" \
            " move.l %[v32],(%[dst_addr])+\n" \
            " move.l %[v32],(%[dst_addr_fast])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,%[v32]\n" \
            " move.w %[v32],(%[dst_addr])+\n" \
            " move.w %[v32],(%[dst_addr_fast])+\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l (%[src_addr])+,(%[dst_addr])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,(%[dst_addr])+\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void blit_or(PIXEL *src_addr, int src_line_add,
        PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
        short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
    PIXEL_32 v32;

#ifdef BOTH
#define COPY_LONG \
            " move.l (%[src_addr])+,%[v32]\n" \
            " or.l %[v32],(%[dst_addr])+\n" \
            " or.l %[v32],(%[dst_addr_fast])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,%[v32]\n" \
            OR_W("v32","dst_addr","","+")" \
            OR_W("v32","dst_addr_fast","","+")"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l (%[src_addr])+,%[v32]\n" \
            " or.l %[v32],(%[dst_addr])+\n"
#define COPY_WORD \
            " move.w (%[src_addr])+,%[v32]\n" \
            OR_W("v32","dst_addr","","+")
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void blit_16b(PIXEL *src_addr, int src_line_add,
     PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
     short int w, short int h, short int operation)
{
    short int i, j;
    PIXEL v, vs, vd;
    PIXEL_32 v32, v32s, v32d;
    PIXEL_32 *src_addr32;
    PIXEL_32 *dst_addr32;
#ifdef BOTH
    PIXEL_32 *dst_addr_fast32;
#endif

#ifdef BOTH
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *src_addr++; \
            vd = *dst_addr_fast; \
            v = rop(vs, vd); \
            *dst_addr_fast++ = v; \
            *dst_addr++ = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        dst_addr_fast32 = (PIXEL_32 *)dst_addr_fast; \
        for(j = (w >> 1) - 1; j >= 0; j--) { \
            v32s = *src_addr32++; \
            v32d = *dst_addr_fast32; \
            v32 = rop(v32s, v32d); \
            *dst_addr_fast32++ = v32; \
            *dst_addr32++ = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast = (PIXEL *)dst_addr_fast32; \
        dst_addr_fast += dst_line_add; \
    }
#else
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *src_addr++; \
            vd = *dst_addr; \
            v = rop(vs, vd); \
            *dst_addr++ = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        for(j = (w >> 1) - 1; j >= 0; j--) { \
            v32s = *src_addr32++; \
            v32d = *dst_addr32; \
            v32 = rop(v32s, v32d); \
            *dst_addr32++ = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
    }
#endif

    (void) dst_addr_fast;
    /* Tell gcc that this cannot happen (already checked in c_blit_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    ROP_SWITCH(BLIT_LOOP);

#undef BLIT_LOOP
}


static void
pan_backwards_copy(PIXEL *src_addr, int src_line_add,
                   PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
                   short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
#ifdef BOTH
    PIXEL_32 v32;

#define COPY_LONG \
            " move.l -(%[src_addr]),%[v32]\n" \
            " move.l %[v32],-(%[dst_addr])\n" \
            " move.l %[v32],-(%[dst_addr_fast])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),%[v32]\n" \
            " move.w %[v32],-(%[dst_addr])\n" \
            " move.w %[v32],-(%[dst_addr_fast])\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l -(%[src_addr]),-(%[dst_addr])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),-(%[dst_addr])\n"
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void
pan_backwards_or(PIXEL *src_addr, int src_line_add,
                 PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
                 short int w, short int h)
{
    REGL y, n16, n4, nR, x16, x4, xR;
    PIXEL_32 v32;

#ifdef BOTH
#define COPY_LONG \
            " move.l -(%[src_addr]),%[v32]\n" \
            " or.l %[v32],-(%[dst_addr])\n" \
            " or.l %[v32],-(%[dst_addr_fast])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),%[v32]\n" \
            OR_W("v32","dst_addr","-","") \
            OR_W("v32","dst_addr_fast","-","")
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [dst_addr_fast]"+a"(dst_addr_fast), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast += dst_line_add
#else
#define COPY_LONG \
            " move.l -(%[src_addr]),%[v32]\n" \
            " or.l %[v32],-(%[dst_addr])\n"
#define COPY_WORD \
            " move.w -(%[src_addr]),%[v32]\n" \
            OR_W("v32","dst_addr","-","")
#define COPY_LOOP \
        __asm__ __volatile__( \
            " jbra 2f\n" \
            "1:\n" \
            REP8(COPY_LONG) \
            "2:\n" \
            DBRA("%[x16]","1b") \
            " jbra 4f\n" \
            "3:\n" \
            REP2(COPY_LONG) \
            "4:\n" \
            DBRA("%[x4]","3b") \
            " jbra 6f\n" \
            "5:\n" \
            COPY_WORD \
            "6:\n" \
            DBRA("%[xR]","5b") \
            : [dst_addr]"+a"(dst_addr), \
              [src_addr]"+a"(src_addr), \
              [x16]"+d"(x16), \
              [x4]"+d"(x4), \
              [xR]"+d"(xR), \
              [v32]"=d"(v32) \
            : \
            : "cc", "memory")
#define NEXTLINE \
        src_addr += src_line_add; \
        dst_addr += dst_line_add
#endif

    (void) dst_addr_fast;

    /* The split of w into 16, 4 and 1 pixel steps is the same for every row */
    n16 = w >> 4;
    n4 = (w >> 2) & 3;
    nR = w & 3;
    y = h;
    while (y--)
    {
        x16 = n16;
        x4 = n4;
        xR = nR;
        COPY_LOOP;
        NEXTLINE;
    }

#undef COPY_LONG
#undef COPY_WORD
#undef COPY_LOOP
#undef NEXTLINE
}


static void
pan_backwards(PIXEL *src_addr, int src_line_add,
              PIXEL *dst_addr, PIXEL *dst_addr_fast, int dst_line_add,
              short int w, short int h, short int operation)
{
    short int i, j;
    PIXEL v, vs, vd;
    PIXEL_32 v32, v32s, v32d;
    PIXEL_32 *src_addr32;
    PIXEL_32 *dst_addr32;
#ifdef BOTH
    PIXEL_32 *dst_addr_fast32;
#endif

#ifdef BOTH
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *--src_addr; \
            vd = *--dst_addr_fast; \
            v = rop(vs, vd); \
            *dst_addr_fast = v; \
            *--dst_addr = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        dst_addr_fast32 = (PIXEL_32 *)dst_addr_fast; \
        for (j = (w >> 1) - 1; j >= 0; j--) \
        { \
            v32s = *--src_addr32; \
            v32d = *--dst_addr_fast32; \
            v32 = rop(v32s, v32d); \
            *dst_addr_fast32 = v32; \
            *--dst_addr32 = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
        dst_addr_fast = (PIXEL *)dst_addr_fast32; \
        dst_addr_fast += dst_line_add; \
    }
#else
#define BLIT_LOOP(rop) \
    for (i = h - 1; i >= 0; i--) \
    { \
        if (w & 1) \
        { \
            vs = *--src_addr; \
            vd = *--dst_addr; \
            v = rop(vs, vd); \
            *dst_addr = v; \
        } \
        src_addr32 = (PIXEL_32 *)src_addr; \
        dst_addr32 = (PIXEL_32 *)dst_addr; \
        for (j = (w >> 1) - 1; j >= 0; j--) \
        { \
            v32s = *--src_addr32; \
            v32d = *--dst_addr32; \
            v32 = rop(v32s, v32d); \
            *dst_addr32 = v32; \
        } \
        src_addr = (PIXEL *)src_addr32; \
        dst_addr = (PIXEL *)dst_addr32; \
        src_addr += src_line_add; \
        dst_addr += dst_line_add; \
    }
#endif

    (void) dst_addr_fast;
    /* Tell gcc that this cannot happen (already checked in c_blit_area() below) */
    if (w <= 0 || h <= 0)
        unreachable();
    ROP_SWITCH(BLIT_LOOP);

#undef BLIT_LOOP
}


#ifdef BOTH_WAS_ON
#define BOTH
#endif


long CDECL
c_blit_area(Virtual *vwk, MFDB *src, long src_x, long src_y,
            MFDB *dst, long dst_x, long dst_y,
            long w, long h, long operation)
{
    Workstation *wk;
    PIXEL *src_addr, *dst_addr, *dst_addr_fast;
    int src_wrap, dst_wrap;
    int src_line_add, dst_line_add;
    unsigned long src_pos, dst_pos;
    int to_screen;

    if (w <= 0 || h <= 0)
        return 1;

    wk = vwk->real_address;

    if (!src || !src->address || (src->address == wk->screen.mfdb.address)) {       /* From screen? */
        src_wrap = wk->screen.wrap;
        if (!(src_addr = wk->screen.shadow.address))
            src_addr = wk->screen.mfdb.address;
    } else {
        src_wrap = (long)src->wdwidth * 2 * src->bitplanes;
        src_addr = src->address;
    }
    src_pos = (short)src_y * (long)src_wrap + src_x * PIXEL_SIZE;
    src_line_add = src_wrap - w * PIXEL_SIZE;

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

    if (src_y < dst_y) {
        src_pos += (short)(h - 1) * (long)src_wrap;
        src_line_add -= src_wrap * 2;
        dst_pos += (short)(h - 1) * (long)dst_wrap;
        dst_line_add -= dst_wrap * 2;
    }

    src_addr += src_pos / PIXEL_SIZE;
    dst_addr += dst_pos / PIXEL_SIZE;
    src_line_add /= PIXEL_SIZE;     /* Change into pixel count */
    dst_line_add /= PIXEL_SIZE;

    dst_addr_fast = wk->screen.shadow.address;  /* May not really be to screen at all, but... */

#ifdef FVDI_DEBUG
    if (debug > 1) {
        debug_out("Blitting: ", w, -1, h, src_x, src_y, dst_x, dst_y);
    }
#endif

#ifdef BOTH
    if (!to_screen || !dst_addr_fast) {
#endif
        if ((src_y == dst_y) && (src_x < dst_x)) {
            src_addr += w;		/* To take backward copy into account */
            dst_addr += w;
            src_line_add += 2 * w;
            dst_line_add += 2 * w;
            switch(operation) {
            case 3:
                pan_backwards_copy(src_addr, src_line_add, dst_addr, 0, dst_line_add, w, h);
                break;
            case 7:
                pan_backwards_or(src_addr, src_line_add, dst_addr, 0, dst_line_add, w, h);
                break;
            default:
                pan_backwards(src_addr, src_line_add, dst_addr, 0, dst_line_add, w, h, operation);
                break;
            }
        } else {
            switch(operation) {
            case 3:
                blit_copy(src_addr, src_line_add, dst_addr, 0, dst_line_add, w, h);
                break;
            case 7:
                blit_or(src_addr, src_line_add, dst_addr, 0, dst_line_add, w, h);
                break;
            default:
                blit_16b(src_addr, src_line_add, dst_addr, 0, dst_line_add, w, h, operation);
                break;
            }
        }
#ifdef BOTH
    } else {
        dst_addr_fast += dst_pos / PIXEL_SIZE;
        if ((src_y == dst_y) && (src_x < dst_x)) {
            src_addr += w;      /* To take backward copy into account */
            dst_addr += w;
            dst_addr_fast += w;
            src_line_add += 2 * w;
            dst_line_add += 2 * w;
            switch(operation) {
            case 3:
                s_pan_backwards_copy(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, w, h);
                break;
            case 7:
                s_pan_backwards_or(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, w, h);
                break;
            default:
                s_pan_backwards(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, w, h, operation);
                break;
            }
        } else {
            switch(operation) {
            case 3:
                s_blit_copy(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, w, h);
                break;
            case 7:
                s_blit_or(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, w, h);
                break;
            default:
                s_blit(src_addr, src_line_add, dst_addr, dst_addr_fast, dst_line_add, w, h, operation);
                break;
            }
        }
    }
#else
    (void) to_screen;
    (void) dst_addr_fast;
#endif

    return 1;   /* Return as completed */
}
