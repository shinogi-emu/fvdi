/* Host checks of vr_transfer_bits with T_BLEND (64) against the shared alpha
 * vectors (alpha_vectors.h): the arithmetic of include/blend.h on every
 * 16-bit layout, blending into memory, the refusals, the routing of the
 * arithmetic modes in vr_transfer_bits, ARGB/RGBA sources in modes 0 and 32
 * and the weight colour.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fvdi.h"
#include "function.h"
#include "blend.h"
#include "itable.h"
#include "test_support.h"
#include "alpha_vectors.h"

long raster_allocations(void);
long raster_pool_takes(void);
void CDECL vr_transfer_bits(Virtual *, GCBITMAP *, GCBITMAP *, RECT16 *, RECT16 *, long);
void call_draw_line(Virtual *, DrvLine *);

#define RGB32       0x03421820UL
#define RGB565      0x03021010UL
#define RGB555      0x03420f10UL
#define REVERSED    0x00800000UL
#define INDEX8      0x01020808UL

/* engine/default.c is linked for vr_transfer_bits; its line retry is not used here. */
void call_draw_line(Virtual *v, DrvLine *line)
{
    (void)v; (void)line;
    abort();
}

static Workstation alpha_wk;
static Virtual alpha_vwk;
static long comparisons;

/* zlib's CRC-32, one byte at a time */
static unsigned long crc_byte(unsigned long crc, unsigned char byte)
{
    int i;
    crc ^= byte;
    for (i = 0; i < 8; i++)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320UL : 0);
    return crc;
}

/* The checksum's unit: a scalar as 4 bytes, big-endian, a negative one sign-extended. */
static unsigned long crc_word(unsigned long crc, long value)
{
    unsigned long u = (unsigned long)value & 0xffffffffUL;
    crc = crc_byte(crc, (unsigned char)(u >> 24));
    crc = crc_byte(crc, (unsigned char)(u >> 16));
    crc = crc_byte(crc, (unsigned char)(u >> 8));
    return crc_byte(crc, (unsigned char)u);
}

static void vectors_sum_ok(void)
{
    unsigned long crc = 0xffffffffUL;
    int i, j, k, t;

    for (i = 0; i < ALPHA_NWEIGHTS; i++)
        for (j = 0; j < 3; j++)
            crc = crc_word(crc, alpha_weight[i][j]);
    for (i = 0; i < ALPHA_NWEIGHTS; i++)
        for (j = 0; j < 256; j++)
            for (k = 0; k < 3; k++)
                crc = crc_word(crc, alpha_k[i][j][k]);
    for (i = 0; i < ALPHA_NPIXELS; i++)
    {
        crc = crc_word(crc, (long)alpha_pixel[i].argb);
        crc = crc_word(crc, alpha_pixel[i].d565);
        crc = crc_word(crc, alpha_pixel[i].e565);
        crc = crc_word(crc, alpha_pixel[i].d555);
        crc = crc_word(crc, alpha_pixel[i].e555);
        crc = crc_word(crc, alpha_pixel[i].weight);
    }
    for (t = 0; t < 4; t++)
    {
        crc = crc_word(crc, alpha_src[t].w);
        crc = crc_word(crc, alpha_src[t].h);
        for (i = 0; i < alpha_src[t].w * alpha_src[t].h; i++)
            crc = crc_word(crc, (long)alpha_src[t].argb[i]);
    }
    for (t = 0; t < 3; t++)
        for (i = 0; i < ALPHA_IMG_H; i++)
            for (j = 0; j < ALPHA_IMG_W; j++)
                crc = crc_word(crc, alpha_bg565[t][i][j]);
    for (t = 0; t < 3; t++)
        for (i = 0; i < ALPHA_IMG_H; i++)
            for (j = 0; j < ALPHA_IMG_W; j++)
                crc = crc_word(crc, alpha_bg555[t][i][j]);
    for (t = 0; t < ALPHA_NXFER; t++)
    {
        const AlphaXfer *x = &alpha_xfer[t];
        crc = crc_word(crc, x->src);
        crc = crc_word(crc, x->bg);
        crc = crc_word(crc, x->weight);
        crc = crc_word(crc, x->clip);
        for (i = 0; i < 4; i++)
            crc = crc_word(crc, x->sr[i]);
        for (i = 0; i < 4; i++)
            crc = crc_word(crc, x->dr[i]);
        for (i = 0; i < 4; i++)
            crc = crc_word(crc, x->cr[i]);
        crc = crc_word(crc, (long)x->crc565);
        crc = crc_word(crc, (long)x->crc555);
    }
    for (i = 0; i < 5; i++)
        for (j = 0; j < 7; j++)
            crc = crc_word(crc, alpha_ramp565[i][j]);
    for (t = 0; t < ALPHA_NREFUSAL; t++)
    {
        crc = crc_word(crc, (long)alpha_refusal[t].px_format);
        crc = crc_word(crc, alpha_refusal[t].mode);
        crc = crc_word(crc, alpha_refusal[t].dest);
    }
    assert((crc ^ 0xffffffffUL) == ALPHA_VECTORS_SUM);
    assert(PX_ARGB32 == ALPHA_PX_ARGB32 && PX_RGBA32 == ALPHA_PX_RGBA32 && T_BLEND == ALPHA_T_BLEND);
}

static void blend_k_table(void)
{
    long w, a, c, step;

    for (w = 0; w < ALPHA_NWEIGHTS; w++)
        for (a = 0; a < 256; a++)
            for (c = 0; c < 3; c++)
            {
                assert(blend_k(a, blend_wprime(alpha_weight[w][c])) == alpha_k[w][a][c]);
                comparisons++;
            }
    /* The table's weights never meet a rounding tie (wa * Wc' = 32768 modulo
     * 65536), so check the formula over the other weights as well: all of
     * them on the host, a sample on the 68k.
     */
#ifdef __m68k__
    step = 61;
#else
    step = 1;
#endif
    for (w = 0; w < 65536; w += step)
    {
        long wp = w + (w >> 15);
        assert(blend_wprime(w) == (unsigned long)wp);
        for (a = 0; a < 256; a++)
            assert(blend_k(a, wp) == (((a + (a >> 7)) * wp + 32768L) >> 16));
        comparisons += 257;
    }
    assert(blend_k(2, blend_wprime(0x4000)) == 1 && blend_k(1, blend_wprime(0x8000)) == 1);
    assert(blend_k(255, blend_wprime(0xffff)) == 256 && blend_k(255, blend_wprime(0)) == 0);
}

static unsigned short swap16(unsigned short v)
{
    return (unsigned short)((v << 8) | (v >> 8));
}

/* A 16-bit pixel in memory, in either byte order (reversed: low byte first). */
static void put16(unsigned char *p, unsigned short v, int reversed)
{
    p[reversed ? 1 : 0] = v >> 8;
    p[reversed ? 0 : 1] = v & 255;
}

static unsigned short get16(const unsigned char *p, int reversed)
{
    return (unsigned short)((p[reversed ? 1 : 0] << 8) | p[reversed ? 0 : 1]);
}

/* A source pixel 0xAARRGGBB in memory: bytes A, R, G, B (PX_ARGB32 and the
 * spare byte of xRGB32) or R, G, B, A (PX_RGBA32).
 */
static void put_source(unsigned char *p, unsigned long argb, unsigned long format)
{
    if (format == PX_RGBA32)
        argb = ((argb << 8) | (argb >> 24)) & 0xffffffffUL;
    p[0] = (unsigned char)(argb >> 24);
    p[1] = (unsigned char)(argb >> 16);
    p[2] = (unsigned char)(argb >> 8);
    p[3] = (unsigned char)argb;
}

static GCBITMAP bitmap(void *pixels, long w, long h, long bits, unsigned long format)
{
    GCBITMAP bm;
    memset(&bm, 0, sizeof(bm));
    bm.magic = 0x6362746dL;
    bm.length = sizeof(bm);
    bm.addr = pixels;
    bm.width = w * bits / 8;
    bm.bits = bits;
    bm.px_format = format;
    bm.xmax = w;
    bm.ymax = h;
    return bm;
}

/* The weight of the vectors' table on a workstation; index 1 (0x8000 on
 * every channel) is the default, used by leaving nothing stored.
 */
static void set_weight(Virtual *v, int index)
{
    COLOR_ENTRY e;

    x_colours_close(v);
    if (index == 1)
        return;
    memset(&e, 0, sizeof(e));
    e.rgb.red = alpha_weight[index][0];
    e.rgb.green = alpha_weight[index][1];
    e.rgb.blue = alpha_weight[index][2];
    assert(lib_vs_x_color(v, 3, 1, &e) == 1);
}

/* Draw one source pixel onto one destination pixel through transfer_blend. */
static unsigned short blend_one(unsigned long argb, unsigned long src_format,
                                unsigned short before, unsigned long dst_format)
{
    unsigned char s[4], d[2];
    GCBITMAP src = bitmap(s, 1, 1, 32, src_format);
    GCBITMAP dst = bitmap(d, 1, 1, 16, dst_format);
    RECT16 r = { 0, 0, 0, 0 };
    int reversed = (dst_format & REVERSED) != 0;

    put_source(s, argb, src_format);
    put16(d, before, reversed);
    transfer_blend(&alpha_vwk, &src, &dst, &r, &r, T_BLEND);
    return get16(d, reversed);
}

static const BlendPacking packing_565 = BLEND_PACKING_565;
static const BlendPacking packing_555 = { 0x7c00, 0x03e0, 9, 6, 0 };

static void blend_pixels_565_555(void)
{
    int i, c, f, weight = -1;
    static const unsigned long formats[2] = { PX_ARGB32, PX_RGBA32 };

    for (i = 0; i < ALPHA_NPIXELS; i++)
    {
        const AlphaPixel *p = &alpha_pixel[i];
        unsigned short a = (unsigned short)(p->argb >> 24), k[3];

        for (c = 0; c < 3; c++)
            k[c] = blend_k(a, blend_wprime(alpha_weight[p->weight][c]));
        if (a)
        {
            /* blend.h writes every pixel; its callers skip alpha 0 */
            assert(blend_pixel16(p->d565, p->argb & 0xffffffUL, k, &packing_565) == p->e565);
            assert(blend_pixel16(p->d555, p->argb & 0xffffffUL, k, &packing_555) == p->e555);
        } else
            assert(p->e565 == p->d565 && p->e555 == p->d555);
        /* The same through the engine: the write rule, alpha 0 untouched */
        if (p->weight != weight)
            set_weight(&alpha_vwk, weight = p->weight);
        for (f = 0; f < 2; f++)
        {
            assert(blend_one(p->argb, formats[f], p->d565, RGB565) == p->e565);
            assert(blend_one(p->argb, formats[f], p->d555, RGB555) == p->e555);
        }
        comparisons += 6;
    }
    x_colours_close(&alpha_vwk);
}

/* The arithmetic of the vectors' header, written out field by field: red
 * (5 bits) at rpos, green (gbits) at gpos, blue (5 bits) at 0.
 */
static unsigned short reference_pixel(unsigned short d, unsigned long argb, const unsigned short *w,
                                      int rpos, int gpos, int gbits)
{
    const int pos[3] = { rpos, gpos, 0 }, bits[3] = { 5, gbits, 5 }, shift[3] = { 16, 8, 0 };
    long a = (long)(argb >> 24), wa = a + (a >> 7);
    unsigned short out = 0;
    int c;

    for (c = 0; c < 3; c++)
    {
        long wp = (long)w[c] + (w[c] >> 15);
        long k = (wa * wp + 32768L) >> 16;
        long dv = (d >> pos[c]) & ((1L << bits[c]) - 1);
        long s = (long)((argb >> shift[c]) & 255) >> (8 - bits[c]);
        long n = dv * 256 + (s - dv) * k + 128;
        assert(n >= 0 && k >= 0 && k <= 256);
        out |= (unsigned short)((n >> 8) << pos[c]);
    }
    return out;
}

static const struct {
    BlendPacking packing;
    int rpos, gpos, gbits;
    unsigned short unused;         /* bits outside the fields */
} layouts[4] = {
    { BLEND_PACKING_565, 11, 5, 6, 0x0000 },
    { { 0x7c00, 0x03e0, 9, 6, 0 }, 10, 5, 5, 0x8000 },
    { { 0xf800, 0x07c0, 8, 5, 0 }, 11, 6, 5, 0x0020 },   /* 5-5-5 with the gap at bit 5 */
    { { 0xf800, 0x03e0, 8, 6, 0 }, 11, 5, 5, 0x0400 }    /* 5-5-5 with the gap at bit 10 */
};

static void blend_pixels_gap_swapped(void)
{
    int i, c, l, reversed;

    for (i = 0; i < ALPHA_NPIXELS; i++)
    {
        const AlphaPixel *p = &alpha_pixel[i];
        unsigned short a = (unsigned short)(p->argb >> 24), k[3];
        const unsigned short *w = alpha_weight[p->weight];

        if (!a)
            continue;
        for (c = 0; c < 3; c++)
            k[c] = blend_k(a, blend_wprime(w[c]));
        for (l = 0; l < 4; l++)
        {
            /* the 555 layout takes the 555 cases, bit 15 included; the rest
               the raw 565 values, which set bit 5 in some of them */
            unsigned short d = l == 1 ? p->d555 : p->d565;
            unsigned short expected = reference_pixel(d, p->argb, w, layouts[l].rpos,
                                                      layouts[l].gpos, layouts[l].gbits);
            assert((expected & layouts[l].unused) == 0);
            if (l == 0)
                assert(expected == p->e565);
            if (l == 1)
                assert(expected == p->e555);
            for (reversed = 0; reversed < 2; reversed++)
            {
                BlendPacking packing = layouts[l].packing;
                packing.reversed = reversed;
                assert(blend_pixel16(reversed ? swap16(d) : d, p->argb & 0xffffffUL, k, &packing) ==
                       (reversed ? swap16(expected) : expected));
                comparisons++;
            }
        }
    }
    /* Byte-swapped memory through the engine */
    set_weight(&alpha_vwk, 0);
    for (i = 0; i < ALPHA_NPIXELS; i++)
    {
        const AlphaPixel *p = &alpha_pixel[i];
        if (p->weight)
            continue;
        assert(blend_one(p->argb, PX_ARGB32, p->d565, RGB565 | REVERSED) == p->e565);
        assert(blend_one(p->argb, PX_RGBA32, p->d555, RGB555 | REVERSED) == p->e555);
        comparisons += 2;
    }
    x_colours_close(&alpha_vwk);
}

/* An alpha_xfer entry drawn into memory: the source with its origin at
 * (-3, 2) and padded rows, the 32 x 24 destination with its origin at (5, -4)
 * and padded rows.  Returns the CRC of the destination as the vectors take
 * it (big-endian pixels); checks that nothing outside the rectangles changed.
 */
#define SRC_STRIDE  (16 * 4 + 8)
#define DST_STRIDE  (ALPHA_IMG_W * 2 + 6)

static unsigned long blend_image(const AlphaXfer *t, unsigned long src_format,
                                 unsigned long dst_format, long mode, int via_vdi)
{
    static unsigned char s[SRC_STRIDE * 12], d[DST_STRIDE * ALPHA_IMG_H];
    static unsigned char s_saved[sizeof(s)];
    int reversed = (dst_format & REVERSED) != 0, is565 = (dst_format & ~REVERSED) == RGB565;
    int sw = alpha_src[t->src].w, sh = alpha_src[t->src].h, x, y;
    GCBITMAP src = bitmap(s, sw, sh, 32, src_format);
    GCBITMAP dst = bitmap(d, ALPHA_IMG_W, ALPHA_IMG_H, 16, dst_format);
    RECT16 sr, dr;
    long allocations = raster_allocations(), takes = raster_pool_takes();
    unsigned long crc = 0xffffffffUL;

    memset(s, 0x5a, sizeof(s));
    memset(d, 0x5a, sizeof(d));
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
            put_source(s + y * SRC_STRIDE + x * 4, alpha_src[t->src].argb[y * sw + x], src_format);
    for (y = 0; y < ALPHA_IMG_H; y++)
        for (x = 0; x < ALPHA_IMG_W; x++)
            put16(d + y * DST_STRIDE + x * 2,
                  is565 ? alpha_bg565[t->bg][y][x] : alpha_bg555[t->bg][y][x], reversed);
    memcpy(s_saved, s, sizeof(s));
    src.width = SRC_STRIDE;
    src.xmin = -3; src.xmax += -3;
    src.ymin = 2; src.ymax += 2;
    dst.width = DST_STRIDE;
    dst.xmin = 5; dst.xmax += 5;
    dst.ymin = -4; dst.ymax += -4;
    sr.x1 = t->sr[0] - 3; sr.y1 = t->sr[1] + 2; sr.x2 = t->sr[2] - 3; sr.y2 = t->sr[3] + 2;
    dr.x1 = t->dr[0] + 5; dr.y1 = t->dr[1] - 4; dr.x2 = t->dr[2] + 5; dr.y2 = t->dr[3] - 4;
    if (via_vdi)
        vr_transfer_bits(&alpha_vwk, &src, &dst, &sr, &dr, mode);
    else
        transfer_blend(&alpha_vwk, &src, &dst, &sr, &dr, mode);
    /* no allocation, no pool block, the source untouched, padding untouched */
    assert(raster_allocations() == allocations && raster_pool_takes() == takes);
    assert(memcmp(s, s_saved, sizeof(s)) == 0);
    for (y = 0; y < ALPHA_IMG_H; y++)
    {
        for (x = 0; x < ALPHA_IMG_W; x++)
        {
            unsigned short v = get16(d + y * DST_STRIDE + x * 2, reversed);
            crc = crc_byte(crc_byte(crc, (unsigned char)(v >> 8)), (unsigned char)v);
        }
        for (x = ALPHA_IMG_W * 2; x < DST_STRIDE; x++)
            assert(d[y * DST_STRIDE + x] == 0x5a);
    }
    return crc ^ 0xffffffffUL;
}

static void blend_memory_images(void)
{
    static const unsigned long formats[2] = { PX_ARGB32, PX_RGBA32 };
    static const unsigned long dests[4] = { RGB565, RGB555, RGB565 | REVERSED, RGB555 | REVERSED };
    int t, f, l, images = 0;

    for (t = 0; t < ALPHA_NXFER; t++)
    {
        const AlphaXfer *x = &alpha_xfer[t];

        if (x->clip)
            continue;           /* memory destinations are never clipped */
        set_weight(&alpha_vwk, x->weight);
        for (f = 0; f < 2; f++)
            for (l = 0; l < 4; l++)
            {
                unsigned long expected = (dests[l] & ~REVERSED) == RGB565 ? x->crc565 : x->crc555;
                /* bit 7 (dithering) changes nothing on 16 bits */
                assert(blend_image(x, formats[f], dests[l], l & 1 ? T_BLEND | 128 : T_BLEND, 0) == expected);
                images++;
            }
        /* the same through vr_transfer_bits */
        assert(blend_image(x, PX_ARGB32, RGB565, T_BLEND, 1) == x->crc565);
        assert(blend_image(x, PX_RGBA32, RGB555 | REVERSED, T_BLEND | 128, 1) == x->crc555);
        images += 2;
    }
    x_colours_close(&alpha_vwk);
    assert(images == (ALPHA_NXFER - 3) * 10);
    comparisons += images;
}

/* Run one call; return whether any byte of the destination or the source
 * memory changed, and put both back.
 */
static unsigned char refusal_src[256], refusal_dst[512];

static int changes(GCBITMAP *src, GCBITMAP *dst, RECT16 *sr, RECT16 *dr, long mode, int via_vdi)
{
    unsigned char s[sizeof(refusal_src)], d[sizeof(refusal_dst)];
    int changed;

    memcpy(s, refusal_src, sizeof(s));
    memcpy(d, refusal_dst, sizeof(d));
    if (via_vdi)
        vr_transfer_bits(&alpha_vwk, src, dst, sr, dr, mode);
    else
        transfer_blend(&alpha_vwk, src, dst, sr, dr, mode);
    changed = memcmp(s, refusal_src, sizeof(s)) || memcmp(d, refusal_dst, sizeof(d));
    memcpy(refusal_src, s, sizeof(s));
    memcpy(refusal_dst, d, sizeof(d));
    return changed;
}

static void refused(GCBITMAP *src, GCBITMAP *dst, RECT16 *sr, RECT16 *dr, long mode)
{
    assert(!changes(src, dst, sr, dr, mode, 0));
    if (src && dst && sr && dr)
        assert(!changes(src, dst, sr, dr, mode, 1));
    comparisons += 2;
}

static void blend_memory_refusals(void)
{
    GCBITMAP src, dst, good_src, good_dst;
    RECT16 sr = { 0, 0, 3, 3 }, dr = { 2, 2, 5, 5 }, r;
    int i;

    for (i = 0; i < (int)sizeof(refusal_src); i++)
        refusal_src[i] = (unsigned char)(i * 37 + 0x81);    /* no zero alpha byte */
    for (i = 0; i < (int)sizeof(refusal_dst); i++)
        refusal_dst[i] = (unsigned char)(i * 11 + 3);
    good_src = bitmap(refusal_src, 4, 4, 32, PX_ARGB32);
    good_dst = bitmap(refusal_dst, 8, 8, 16, RGB565);
    /* the base case draws, also with bit 7 */
    assert(changes(&good_src, &good_dst, &sr, &dr, T_BLEND, 0));
    assert(changes(&good_src, &good_dst, &sr, &dr, T_BLEND, 1));
    assert(changes(&good_src, &good_dst, &sr, &dr, T_BLEND | 128, 0));
    assert(changes(&good_src, &good_dst, &sr, &dr, T_BLEND | 128, 1));

    /* The vectors' refusals.  Before screen blending a screen case is
     * checked into memory: the refusal is the source format's or the mode's.
     */
    for (i = 0; i < ALPHA_NREFUSAL; i++)
    {
        const AlphaRefusal *c = &alpha_refusal[i];
        long bits = c->px_format == 0x01020101UL ? 1 : c->px_format == INDEX8 ? 8 : 32;

        src = bitmap(refusal_src, 4, 4, bits, c->px_format);
        if (bits == 1)
            src.width = 2;
        switch (c->dest)
        {
        case ALPHA_DEST_SCREEN:
        case ALPHA_DEST_MEM16:
            dst = good_dst;
            break;
        case ALPHA_DEST_INDEX8:
            dst = bitmap(refusal_dst, 8, 8, 8, INDEX8);
            break;
        case ALPHA_DEST_ARGB32:
            dst = bitmap(refusal_dst, 8, 8, 32, PX_ARGB32);
            break;
        case ALPHA_DEST_XRGB32:
            /* the old path would copy xRGB32 to xRGB32 whatever the mode */
            dst = bitmap(refusal_dst, 8, 8, 32, RGB32);
            break;
        default:
            continue;           /* a screen of 8 planes or fewer: the guest's */
        }
        refused(&src, &dst, &sr, &dr, c->mode);
    }
    /* The other arithmetic modes, and T_BLEND with T_COLORIZE */
    for (i = 65; i < 128; i++)
    {
        refused(&good_src, &good_dst, &sr, &dr, i);
        refused(&good_src, &good_dst, &sr, &dr, i | 128);
    }
    /* Every direct source format blends; refused variants of them do not */
    src = good_src;
    src.px_format = 0x03922020UL;              /* PX_ARGB32 | PX_REVERSED */
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src.px_format = 0x03822020UL;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src.px_format = 0x04122020UL;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src.px_format = RGB32;
    assert(changes(&src, &good_dst, &sr, &dr, T_BLEND, 0));
    src = bitmap(refusal_src, 4, 4, 16, RGB565 | REVERSED);
    assert(changes(&src, &good_dst, &sr, &dr, T_BLEND, 1));
    src = bitmap(refusal_src, 4, 4, 8, INDEX8);
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    /* source descriptions */
    src = good_src;
    src.bits = 24;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src = good_src;
    src.width = 14;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src = good_src;
    src.addr = refusal_src + 1;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src = good_src;
    src.addr = NULL;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src = good_src;
    src.xmax = src.xmin;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    /* destination descriptions */
    dst = good_dst;
    dst.bits = 15;
    refused(&good_src, &dst, &sr, &dr, T_BLEND);
    dst = good_dst;
    dst.px_format = 0x03421010UL;
    refused(&good_src, &dst, &sr, &dr, T_BLEND);
    dst = good_dst;
    dst.width = 14;
    refused(&good_src, &dst, &sr, &dr, T_BLEND);
    dst = good_dst;
    dst.addr = refusal_dst + 1;
    refused(&good_src, &dst, &sr, &dr, T_BLEND);
    /* overlapping memory */
    dst = good_dst;
    dst.addr = refusal_src + 32;
    refused(&good_src, &dst, &sr, &dr, T_BLEND);
    /* the screen passed as a memory bitmap */
    alpha_wk.screen.mfdb.address = (short *)refusal_dst;
    refused(&good_src, &good_dst, &sr, &dr, T_BLEND);
    alpha_wk.screen.mfdb.address = NULL;
    alpha_wk.screen.shadow.address = refusal_dst;
    refused(&good_src, &good_dst, &sr, &dr, T_BLEND);
    alpha_wk.screen.shadow.address = NULL;
    /* rectangles outside their bitmaps, or empty */
    r = sr; r.x2 = 4;
    refused(&good_src, &good_dst, &r, &dr, T_BLEND);
    r = sr; r.y1 = -1;
    refused(&good_src, &good_dst, &r, &dr, T_BLEND);
    r = sr; r.x2 = -1;
    refused(&good_src, &good_dst, &r, &dr, T_BLEND);
    r = dr; r.x2 = 8;
    refused(&good_src, &good_dst, &sr, &r, T_BLEND);
    r = dr; r.y2 = 8;
    refused(&good_src, &good_dst, &sr, &r, T_BLEND);
    r = dr; r.x1 = -1;
    refused(&good_src, &good_dst, &sr, &r, T_BLEND);
    r = dr; r.y2 = 1;
    refused(&good_src, &good_dst, &sr, &r, T_BLEND);
    /* missing pieces */
    refused(NULL, &good_dst, &sr, &dr, T_BLEND);
    refused(&good_src, &good_dst, NULL, &dr, T_BLEND);
    refused(&good_src, &good_dst, &sr, NULL, T_BLEND);
    /* the screen: not blended yet */
    refused(&good_src, NULL, &sr, &dr, T_BLEND);
    assert(!changes(&good_src, NULL, &sr, &dr, T_BLEND, 1));
}

/* Modes 0 and 32 take ARGB and RGBA sources with the alpha ignored: the
 * same as xRGB32 with the same colours, on the RGB16 and index-8 paths.
 */
static void palette_17(COLOR_TAB *p)
{
    int i;
    memset(p, 0, sizeof(*p));
    p->magic = 0x63746162L;
    p->length = offsetof(COLOR_TAB, colors) + 17 * sizeof(COLOR_ENTRY);
    p->color_space = 1;
    p->no_colors = 17;
    for (i = 0; i < 17; i++)
    {
        p->colors[i].rgb.red = (i * 4177UL + 711) & 65535;
        p->colors[i].rgb.green = (i * 2017UL + 31337) & 65535;
        p->colors[i].rgb.blue = (i * 7919UL + 5133) & 65535;
    }
}

static void modes_0_32_formats(void)
{
    static const unsigned long formats[3] = { RGB32, PX_ARGB32, PX_RGBA32 };
    static const long modes[4] = { 0, 32, 128, 160 };
    static const short sizes[3][2] = { { 7, 5 }, { 13, 9 }, { 4, 3 } };
    static unsigned char s[3][7 * 5 * 4], d[3][13 * 9 * 2];
    COLOR_TAB pal;
    void *itab;
    long allocations = raster_allocations();
    int f, m, z, l, i, via;

    for (i = 0; i < 7 * 5; i++)
    {
        unsigned long argb = (i * 0x9e3779b1UL) & 0xffffffffUL;
        if (i % 5 == 0)
            argb &= 0x00ffffffUL;      /* alpha 0 is drawn too */
        for (f = 0; f < 3; f++)
            put_source(s[f] + i * 4, argb, formats[f]);
    }
    palette_17(&pal);
    itab = itab_create(&alpha_vwk, &pal, 4);
    assert(itab);
    for (z = 0; z < 3; z++)
        for (m = 0; m < 4; m++)
        {
            RECT16 sr = { 0, 0, 6, 4 }, dr;
            dr.x1 = 0; dr.y1 = 0; dr.x2 = sizes[z][0] - 1; dr.y2 = sizes[z][1] - 1;
            /* RGB16: 565 big-endian and 555 byte-swapped, directly and
               through vr_transfer_bits (modes below 64 are not rerouted) */
            for (l = 0; l < 2; l++)
                for (via = 0; via < 2; via++)
                {
                    for (f = 0; f < 3; f++)
                    {
                        GCBITMAP src = bitmap(s[f], 7, 5, 32, formats[f]);
                        GCBITMAP dst = bitmap(d[f], sizes[z][0], sizes[z][1], 16,
                                              l ? RGB555 | REVERSED : RGB565);
                        memset(d[f], 0xe7, sizeof(d[f]));
                        if (via)
                            vr_transfer_bits(&alpha_vwk, &src, &dst, &sr, &dr, modes[m]);
                        else
                            assert(transfer_rgb16(&alpha_vwk, &src, &dst, &sr, &dr, modes[m]) == 1);
                    }
                    for (i = 0; i < sizes[z][0] * sizes[z][1] * 2; i++)
                        if (d[0][i] != 0xe7)
                            break;
                    assert(i < sizes[z][0] * sizes[z][1] * 2);          /* drawn */
                    assert(memcmp(d[0], d[1], sizeof(d[0])) == 0);
                    assert(memcmp(d[0], d[2], sizeof(d[0])) == 0);
                    comparisons += 2;
                }
            /* index 8, with an explicit palette and inverse table */
            for (f = 0; f < 3; f++)
            {
                GCBITMAP src = bitmap(s[f], 7, 5, 32, formats[f]);
                GCBITMAP dst = bitmap(d[f], sizes[z][0], sizes[z][1], 8, INDEX8);
                dst.ctab = &pal;
                dst.itab = itab;
                memset(d[f], 0xe7, sizeof(d[f]));
                assert(transfer_index8(&alpha_vwk, &src, &dst, &sr, &dr, modes[m]) == 1);
            }
            for (i = 0; i < sizes[z][0] * sizes[z][1]; i++)
                assert(d[0][i] < 17);
            assert(memcmp(d[0], d[1], sizeof(d[0])) == 0);
            assert(memcmp(d[0], d[2], sizeof(d[0])) == 0);
            comparisons += 2;
        }
    assert(itab_delete(&alpha_vwk, itab) == 1);
    itab_close(&alpha_vwk);
    ctab_close(&alpha_vwk);
    assert(raster_allocations() == allocations);
}

static void weight_accessor(void)
{
    Virtual other;
    COLOR_ENTRY e;
    unsigned short w[3], expected;
    long allocations = raster_allocations();

    memset(&other, 0, sizeof(other));
    other.real_address = &alpha_wk;
    x_weight(&alpha_vwk, w);
    assert(w[0] == 0x8000 && w[1] == 0x8000 && w[2] == 0x8000);
    memset(&e, 0, sizeof(e));
    e.rgb.red = 0x1234; e.rgb.green = 0xffff; e.rgb.blue = 0x0000;
    assert(lib_vs_x_color(&alpha_vwk, 3, 1, &e) == 1);
    x_weight(&alpha_vwk, w);
    assert(w[0] == 0x1234 && w[1] == 0xffff && w[2] == 0x0000);
    assert(raster_allocations() == allocations + 1);
    /* another workstation keeps the default; the other colours are not the weight */
    x_weight(&other, w);
    assert(w[0] == 0x8000 && w[1] == 0x8000 && w[2] == 0x8000);
    e.rgb.red = e.rgb.green = e.rgb.blue = 0x4321;
    assert(lib_vs_x_color(&alpha_vwk, 0, 1, &e) == 1);
    assert(lib_vs_x_color(&alpha_vwk, 2, 1, &e) == 1);
    x_weight(&alpha_vwk, w);
    assert(w[0] == 0x1234 && w[1] == 0xffff && w[2] == 0x0000);
    /* closing forgets it */
    x_colours_close(&alpha_vwk);
    x_weight(&alpha_vwk, w);
    assert(w[0] == 0x8000 && w[1] == 0x8000 && w[2] == 0x8000);
    assert(raster_allocations() == allocations);
    /* a blend uses its own workstation's weight: the default here, zero on the other */
    set_weight(&other, 2);
    expected = reference_pixel(0x0000, 0xff00f800UL, alpha_weight[1], 11, 5, 6);
    assert(expected != 0x0000 && blend_one(0xff00f800UL, PX_ARGB32, 0x0000, RGB565) == expected);
    x_colours_close(&other);
    assert(raster_allocations() == allocations);
    comparisons += 9;
}

void alpha_tests(void)
{
    long allocations = raster_allocations();

    memset(&alpha_wk, 0, sizeof(alpha_wk));
    memset(&alpha_vwk, 0, sizeof(alpha_vwk));
    alpha_vwk.real_address = &alpha_wk;
    vectors_sum_ok();
    blend_k_table();
    blend_pixels_565_555();
    blend_pixels_gap_swapped();
    blend_memory_images();
    blend_memory_refusals();
    modes_0_32_formats();
    weight_accessor();
    assert(raster_allocations() == allocations);
    printf("%ld alpha comparisons passed (vectors 0x%08lx; k table, pixels in 565/555/gap layouts both byte orders, memory images, refusals, routing, modes 0/32, weight)\n",
           comparisons, (unsigned long)ALPHA_VECTORS_SUM);
}
