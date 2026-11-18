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
void raster_pool(long, int);
Virtual *raster_screen(int, int, unsigned char *, int, int, int);
void raster_screen_counts(long *, long *, long *);
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

/* Draw one source pixel onto one destination pixel through transfer_blend.
 * The unions keep both at even addresses whatever the frame's layout.
 */
static unsigned short blend_one(unsigned long argb, unsigned long src_format,
                                unsigned short before, unsigned long dst_format)
{
    union { unsigned long align; unsigned char bytes[4]; } s;
    union { unsigned long align; unsigned char bytes[2]; } d;
    GCBITMAP src = bitmap(s.bytes, 1, 1, 32, src_format);
    GCBITMAP dst = bitmap(d.bytes, 1, 1, 16, dst_format);
    RECT16 r = { 0, 0, 0, 0 };
    int reversed = (dst_format & REVERSED) != 0;

    put_source(s.bytes, argb, src_format);
    put16(d.bytes, before, reversed);
    transfer_blend(&alpha_vwk, &src, &dst, &r, &r, T_BLEND);
    return get16(d.bytes, reversed);
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
#define SRC_BYTES   (SRC_STRIDE * 12)
#define DST_STRIDE  (ALPHA_IMG_W * 2 + 6)

/* The source of an alpha_xfer entry in format: in s (SRC_BYTES, rows of
 * SRC_STRIDE bytes, the padding 0x5a) with its origin at (-3, 2), and the
 * entry's source rectangle there in sr.
 */
static GCBITMAP source_image(const AlphaXfer *t, unsigned long format, unsigned char *s, RECT16 *sr)
{
    int sw = alpha_src[t->src].w, sh = alpha_src[t->src].h, x, y;
    GCBITMAP src = bitmap(s, sw, sh, 32, format);

    memset(s, 0x5a, SRC_BYTES);
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
            put_source(s + y * SRC_STRIDE + x * 4, alpha_src[t->src].argb[y * sw + x], format);
    src.width = SRC_STRIDE;
    src.xmin = -3; src.xmax += -3;
    src.ymin = 2; src.ymax += 2;
    sr->x1 = t->sr[0] - 3; sr->y1 = t->sr[1] + 2; sr->x2 = t->sr[2] - 3; sr->y2 = t->sr[3] + 2;
    return src;
}

static unsigned long blend_image(const AlphaXfer *t, unsigned long src_format,
                                 unsigned long dst_format, long mode, int via_vdi)
{
    static unsigned char s[SRC_BYTES], d[DST_STRIDE * ALPHA_IMG_H];
    static unsigned char s_saved[sizeof(s)];
    int reversed = (dst_format & REVERSED) != 0, is565 = (dst_format & ~REVERSED) == RGB565;
    int x, y;
    RECT16 sr, dr;
    GCBITMAP src = source_image(t, src_format, s, &sr);
    GCBITMAP dst = bitmap(d, ALPHA_IMG_W, ALPHA_IMG_H, 16, dst_format);
    long allocations = raster_allocations(), takes = raster_pool_takes();
    unsigned long crc = 0xffffffffUL;

    memset(d, 0x5a, sizeof(d));
    for (y = 0; y < ALPHA_IMG_H; y++)
        for (x = 0; x < ALPHA_IMG_W; x++)
            put16(d + y * DST_STRIDE + x * 2,
                  is565 ? alpha_bg565[t->bg][y][x] : alpha_bg555[t->bg][y][x], reversed);
    memcpy(s_saved, s, sizeof(s));
    dst.width = DST_STRIDE;
    dst.xmin = 5; dst.xmax += 5;
    dst.ymin = -4; dst.ymax += -4;
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

    /* The vectors' refusals.  A screen case is checked into memory here,
     * since the refusal is the source format's or the mode's, and onto the
     * screen in blend_screen_refusals().
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
    /* Refused variants of the direct formats (which blend_opaque_sources()
       checks); indexed sources */
    src = good_src;
    src.px_format = 0x03922020UL;              /* PX_ARGB32 | PX_REVERSED */
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src.px_format = 0x03822020UL;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
    src.px_format = 0x04122020UL;
    refused(&src, &good_dst, &sr, &dr, T_BLEND);
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
    /* the screen of a workstation without a driver */
    refused(&good_src, NULL, &sr, &dr, T_BLEND);
    assert(!changes(&good_src, NULL, &sr, &dr, T_BLEND, 1));
}

/* Sources without alpha blend as alpha 255: xRGB32 whatever its spare byte,
 * RGB565 and RGB555 (either byte order, bit 15 of 555 ignored) as their
 * colours expanded to 8 bits by bit replication.  Each must give exactly
 * the pixels of the same colours in PX_ARGB32 with alpha 0xff.
 */
static unsigned char expand(unsigned long field, int bits)
{
    return (unsigned char)((field << (8 - bits)) | (field >> (2 * bits - 8)));
}

static void blend_opaque_sources(void)
{
    static const unsigned long sources[5] = {
        RGB32, RGB565, RGB565 | REVERSED, RGB555, RGB555 | REVERSED
    };
    static const unsigned long dests[2] = { RGB565, RGB555 | REVERSED };
    static const int weights[3] = { 0, 1, 3 };
    static union { unsigned long align; unsigned char b[7 * 5 * 4]; } argb, other;
    static union { unsigned long align; unsigned char b[13 * 9 * 2]; } before, d_argb, d_other;
    RECT16 sr = { 0, 0, 6, 4 }, dr = { 0, 0, 12, 8 };
    long allocations = raster_allocations();
    int s, l, w, via, i;

    for (i = 0; i < (int)sizeof(before.b); i++)
        before.b[i] = (unsigned char)(i * 53 + 17);
    for (s = 0; s < 5; s++)
    {
        unsigned long format = sources[s], base = format & ~REVERSED;
        int bits = format == RGB32 ? 32 : 16;
        GCBITMAP src = bitmap(other.b, 7, 5, bits, format);
        GCBITMAP ref = bitmap(argb.b, 7, 5, 32, PX_ARGB32);

        for (i = 0; i < 7 * 5; i++)
        {
            unsigned long v = (i * 0x9e3779b1UL + 0x01234567UL) & 0xffffffffUL, rgb;

            if (bits == 32)
            {
                /* the spare byte: 0, 0xff and others */
                v = (v & 0x00ffffffUL) | ((unsigned long)(i == 0 ? 0 : i == 1 ? 0xff : (i * 37) & 255) << 24);
                put_source(other.b + i * 4, v, RGB32);
                rgb = v & 0x00ffffffUL;
            } else
            {
                unsigned long r, g, b;

                v &= 0xffffUL;
                put16(other.b + i * 2, (unsigned short)v, (format & REVERSED) != 0);
                b = expand(v & 31, 5);
                if (base == RGB565)
                {
                    r = expand(v >> 11, 5);
                    g = expand((v >> 5) & 63, 6);
                } else
                {
                    r = expand((v >> 10) & 31, 5);
                    g = expand((v >> 5) & 31, 5);
                }
                rgb = (r << 16) | (g << 8) | b;
            }
            put_source(argb.b + i * 4, 0xff000000UL | rgb, PX_ARGB32);
        }
        for (l = 0; l < 2; l++)
            for (w = 0; w < 3; w++)
                for (via = 0; via < 2; via++)
                {
                    GCBITMAP d1 = bitmap(d_argb.b, 13, 9, 16, dests[l]);
                    GCBITMAP d2 = bitmap(d_other.b, 13, 9, 16, dests[l]);

                    set_weight(&alpha_vwk, weights[w]);
                    memcpy(d_argb.b, before.b, sizeof(before.b));
                    memcpy(d_other.b, before.b, sizeof(before.b));
                    if (via)
                    {
                        vr_transfer_bits(&alpha_vwk, &ref, &d1, &sr, &dr, T_BLEND);
                        vr_transfer_bits(&alpha_vwk, &src, &d2, &sr, &dr, T_BLEND | 128);
                    } else
                    {
                        transfer_blend(&alpha_vwk, &ref, &d1, &sr, &dr, T_BLEND);
                        transfer_blend(&alpha_vwk, &src, &d2, &sr, &dr, T_BLEND);
                    }
                    assert(memcmp(d_argb.b, before.b, sizeof(before.b)) != 0);     /* drawn */
                    assert(memcmp(d_argb.b, d_other.b, sizeof(before.b)) == 0);
                    comparisons++;
                }
    }
    x_colours_close(&alpha_vwk);
    assert(raster_allocations() == allocations);
}

/* The screen: vr_transfer_bits with no destination bitmap.  The fake blit
 * of tests/transfer.c draws on a 48 x 36 screen with padded lines, reads it
 * back into memory for T_BLEND (from the shadow when there is one) and
 * checks that each read is written back as the same rectangle.
 */
#define SCREEN_W     48
#define SCREEN_H     36
#define SCREEN_WRAP  (SCREEN_W * 2 + 6)
#define SCREEN_BYTES (SCREEN_WRAP * SCREEN_H)

static union { unsigned long align; unsigned char b[SCREEN_BYTES]; } screen_px, shadow_px, screen_expect;
static long screen_images, screen_read_backs;
static long larger(long a, long b) { return a > b ? a : b; }
static long smaller(long a, long b) { return a < b ? a : b; }
static int screen_pool_missing;         /* no pool block: nothing may be drawn */
static long last_reads, last_visible;   /* of the last screen image */

/* The fake screen in layout l of layouts[]: kind 0 plain, 1 with a shadow,
 * 2 without an address (VRAM not mapped: only the driver's blit reaches it).
 */
static Virtual *blend_screen(int l, int kind)
{
    Virtual *v = raster_screen(SCREEN_W, SCREEN_H, screen_px.b, layouts[l].gbits, layouts[l].rpos,
                               kind == 2);
    Workstation *w = v->real_address;
    int i;

    for (i = 0; i < layouts[l].gbits; i++)
        w->driver->device->scrmap.bitnumber.green[i] = layouts[l].gpos + i;
    w->screen.wrap = SCREEN_WRAP;
    w->screen.shadow.address = kind == 1 ? shadow_px.b : NULL;
    return v;
}

/* Background bg of the vectors in layout l: the 555 colours for 555, the
 * raw 565 values for the others (in the layouts with a gap, some have the
 * gap bit set, which a blended pixel clears).
 */
static unsigned short background(int bg, int l, int x, int y)
{
    return l == 1 ? alpha_bg555[bg][y][x] : alpha_bg565[bg][y][x];
}

/* An alpha_xfer entry drawn in layout l by the test's own arithmetic, the
 * whole 32 x 24 area: the nearest source pixel sx = sr.x1 + (x - dr.x1) *
 * sw / dw, likewise sy, from the whole of dr; alpha 0 and, when the entry
 * is clipped, the pixels outside cr left as they are.
 */
static void reference_area(const AlphaXfer *t, int l, unsigned short out[ALPHA_IMG_H][ALPHA_IMG_W])
{
    long sw = t->sr[2] - t->sr[0] + 1, sh = t->sr[3] - t->sr[1] + 1;
    long dw = t->dr[2] - t->dr[0] + 1, dh = t->dr[3] - t->dr[1] + 1;
    int x, y;

    for (y = 0; y < ALPHA_IMG_H; y++)
        for (x = 0; x < ALPHA_IMG_W; x++)
        {
            unsigned short d = background(t->bg, l, x, y);
            unsigned long argb;
            long sx, sy;

            out[y][x] = d;
            if (x < t->dr[0] || x > t->dr[2] || y < t->dr[1] || y > t->dr[3] ||
                (t->clip && (x < t->cr[0] || x > t->cr[2] || y < t->cr[1] || y > t->cr[3])))
                continue;
            sx = t->sr[0] + (x - t->dr[0]) * sw / dw;
            sy = t->sr[1] + (y - t->dr[1]) * sh / dh;
            argb = alpha_src[t->src].argb[sy * alpha_src[t->src].w + sx];
            if (argb >> 24)
                out[y][x] = reference_pixel(d, argb, alpha_weight[t->weight],
                                            layouts[l].rpos, layouts[l].gpos, layouts[l].gbits);
        }
}

static unsigned long area_crc(unsigned short a[ALPHA_IMG_H][ALPHA_IMG_W])
{
    unsigned long crc = 0xffffffffUL;
    int x, y;

    for (y = 0; y < ALPHA_IMG_H; y++)
        for (x = 0; x < ALPHA_IMG_W; x++)
            crc = crc_byte(crc_byte(crc, (unsigned char)(a[y][x] >> 8)), (unsigned char)a[y][x]);
    return crc ^ 0xffffffffUL;
}

/* An alpha_xfer entry drawn onto the fake screen (blend_screen(l, kind))
 * with its 32 x 24 area at (ox, oy), possibly partly off the screen; a
 * clipped entry through vs_clip, cr moved with the area.  variant: bit 0
 * PX_RGBA32 (else PX_ARGB32), bit 1 bit 7 in the mode, bit 2 through
 * vr_transfer_bits, bit 3 clipping on at the screen's edges (entries that
 * are not clipped).  Checks the visible part of the area against expect
 * and the rest of the screen untouched, the shadow equal to the screen,
 * each read written back, every pixel drawn read once and no other (no
 * pixel blended twice, none left out), one pool block, none when nothing
 * is visible, and no allocation.  Returns the area as the vectors' CRC
 * takes it, when it is all on the screen.
 */
static unsigned long blend_screen_image(const AlphaXfer *t, int l, int kind, int ox, int oy,
                                        int variant, unsigned short expect[ALPHA_IMG_H][ALPHA_IMG_W])
{
    static union { unsigned long align; unsigned char b[SRC_BYTES]; } s, s_saved;
    unsigned long format = variant & 1 ? PX_RGBA32 : PX_ARGB32;
    long mode = variant & 2 ? T_BLEND | 128 : T_BLEND;
    Virtual *v = blend_screen(l, kind);
    RECT16 sr, dr;
    GCBITMAP src = source_image(t, format, s.b, &sr);
    long allocations = raster_allocations(), takes = raster_pool_takes();
    long reads, writes, pixels, reads_after, writes_after, pixels_after;
    long x1, y1, x2, y2;
    unsigned long crc = 0xffffffffUL;
    int x, y, whole = ox >= 0 && oy >= 0 && ox + ALPHA_IMG_W <= SCREEN_W && oy + ALPHA_IMG_H <= SCREEN_H;
    long i;

    memcpy(s_saved.b, s.b, SRC_BYTES);
    for (i = 0; i < SCREEN_BYTES; i++)
        screen_px.b[i] = (unsigned char)(i * 29 + 7);
    memcpy(screen_expect.b, screen_px.b, SCREEN_BYTES);
    for (y = 0; y < ALPHA_IMG_H; y++)
        for (x = 0; x < ALPHA_IMG_W; x++)
            if (ox + x >= 0 && ox + x < SCREEN_W && oy + y >= 0 && oy + y < SCREEN_H)
            {
                long at = (long)(oy + y) * SCREEN_WRAP + (ox + x) * 2;
                put16(screen_px.b + at, background(t->bg, l, x, y), 0);
                put16(screen_expect.b + at,
                      screen_pool_missing ? background(t->bg, l, x, y) : expect[y][x], 0);
            }
    memcpy(shadow_px.b, screen_px.b, SCREEN_BYTES);
    dr.x1 = t->dr[0] + ox; dr.y1 = t->dr[1] + oy; dr.x2 = t->dr[2] + ox; dr.y2 = t->dr[3] + oy;
    x1 = larger(dr.x1, 0); y1 = larger(dr.y1, 0);
    x2 = smaller(dr.x2, SCREEN_W - 1); y2 = smaller(dr.y2, SCREEN_H - 1);
    if (t->clip)
    {
        v->clip.on = 1;
        v->clip.rectangle.x1 = t->cr[0] + ox; v->clip.rectangle.y1 = t->cr[1] + oy;
        v->clip.rectangle.x2 = t->cr[2] + ox; v->clip.rectangle.y2 = t->cr[3] + oy;
        x1 = larger(x1, v->clip.rectangle.x1); y1 = larger(y1, v->clip.rectangle.y1);
        x2 = smaller(x2, v->clip.rectangle.x2); y2 = smaller(y2, v->clip.rectangle.y2);
    } else if (variant & 8)
    {
        v->clip.on = 1;
        v->clip.rectangle.x1 = 0; v->clip.rectangle.y1 = 0;
        v->clip.rectangle.x2 = SCREEN_W - 1; v->clip.rectangle.y2 = SCREEN_H - 1;
    }
    last_visible = x1 <= x2 && y1 <= y2 ? (x2 - x1 + 1) * (y2 - y1 + 1) : 0;
    set_weight(v, t->weight);
    raster_screen_counts(&reads, &writes, &pixels);
    if (variant & 4)
        vr_transfer_bits(v, &src, NULL, &sr, &dr, mode);
    else
        transfer_blend(v, &src, NULL, &sr, &dr, mode);
    raster_screen_counts(&reads_after, &writes_after, &pixels_after);
    x_colours_close(v);
    last_reads = reads_after - reads;
    if (memcmp(screen_px.b, screen_expect.b, SCREEN_BYTES) != 0)
        fprintf(stderr, "screen image %d (layout %d, kind %d, at %d,%d, variant %d) differs\n",
                (int)(t - alpha_xfer), l, kind, ox, oy, variant);
    assert(memcmp(screen_px.b, screen_expect.b, SCREEN_BYTES) == 0);
    if (kind == 1)
        assert(memcmp(shadow_px.b, screen_px.b, SCREEN_BYTES) == 0);
    assert(writes_after - writes == last_reads);
    assert(pixels_after - pixels == (screen_pool_missing ? 0 : last_visible));
    assert(raster_pool_takes() == takes + (!screen_pool_missing && last_visible > 0));
    assert(raster_allocations() == allocations);
    assert(memcmp(s.b, s_saved.b, SRC_BYTES) == 0);
    screen_images++;
    screen_read_backs += last_reads;
    comparisons++;
    if (!whole)
        return 0;
    for (y = 0; y < ALPHA_IMG_H; y++)
        for (x = 0; x < ALPHA_IMG_W; x++)
        {
            const unsigned char *p = screen_px.b + (long)(oy + y) * SCREEN_WRAP + (ox + x) * 2;
            crc = crc_byte(crc_byte(crc, p[0]), p[1]);
        }
    return crc ^ 0xffffffffUL;
}

static void blend_screen_images(void)
{
    static const int offsets[3][2] = { { 7, 5 }, { -9, -6 }, { 30, 20 } };
    static unsigned short expect[ALPHA_IMG_H][ALPHA_IMG_W];
    int t, l, kind, o;

    raster_pool(16384, 0);              /* the whole area in one strip */
    for (t = 0; t < ALPHA_NXFER; t++)
        for (l = 0; l < 4; l++)
        {
            const AlphaXfer *x = &alpha_xfer[t];

            reference_area(x, l, expect);
            /* the test's arithmetic and sampling are the vectors' */
            if (l < 2)
                assert(area_crc(expect) == (l ? x->crc555 : x->crc565));
            for (kind = 0; kind < 3; kind++)
                for (o = 0; o < 3; o++)
                {
                    unsigned long crc = blend_screen_image(x, l, kind, offsets[o][0], offsets[o][1],
                                                           (t + l + kind + o) & 15, expect);
                    if (o == 0 && l < 2)
                        assert(crc == (l ? x->crc555 : x->crc565));
                }
        }
}

/* Small pool blocks: many strips (16 pixels of one row in a block of 32
 * bytes), the same pixels.  No pool block: nothing drawn.
 */
static void blend_screen_strips(void)
{
    static const long capacities[3] = { 32, 64, 100 };
    static unsigned short expect[ALPHA_IMG_H][ALPHA_IMG_W];
    long strips = 0;
    int c, t;

    for (c = 0; c < 3; c++)
    {
        raster_pool(capacities[c], 0);
        for (t = 0; t < ALPHA_NXFER; t++)
        {
            const AlphaXfer *x = &alpha_xfer[t];
            int l = t % 4;
            unsigned long crc;

            reference_area(x, l, expect);
            crc = blend_screen_image(x, l, t % 3, 7, 5, t & 15, expect);
            if (l < 2)
                assert(crc == (l ? x->crc555 : x->crc565));
            /* a strip holds at most capacity / 2 pixels */
            assert(last_reads * (capacities[c] / 2) >= last_visible);
            strips += last_reads;
        }
    }
    assert(strips > 3 * ALPHA_NXFER);
    raster_pool(16384, 1);
    screen_pool_missing = 1;
    for (t = 0; t < ALPHA_NXFER; t++)
    {
        static unsigned short ignored[ALPHA_IMG_H][ALPHA_IMG_W];
        blend_screen_image(&alpha_xfer[t], t % 4, t % 3, 7, 5, t & 15, ignored);
        assert(last_reads == 0);
    }
    screen_pool_missing = 0;
    raster_pool(10240, 0);
}

/* T_BLEND onto the screen that must draw nothing: the screen, its shadow
 * and the source memory unchanged, nothing read or written, no pool block;
 * directly and through vr_transfer_bits.
 */
static void screen_refused(Virtual *v, GCBITMAP *src, RECT16 *sr, RECT16 *dr, long mode)
{
    static union { unsigned long align; unsigned char b[SCREEN_BYTES]; } shadow_saved;
    unsigned char src_saved[sizeof(refusal_src)];
    long takes = raster_pool_takes(), reads, writes, reads_after, writes_after;
    int via;

    memcpy(screen_expect.b, screen_px.b, SCREEN_BYTES);
    memcpy(shadow_saved.b, shadow_px.b, SCREEN_BYTES);
    memcpy(src_saved, refusal_src, sizeof(src_saved));
    for (via = 0; via < 2; via++)
    {
        raster_screen_counts(&reads, &writes, NULL);
        if (via)
            vr_transfer_bits(v, src, NULL, sr, dr, mode);
        else
            transfer_blend(v, src, NULL, sr, dr, mode);
        raster_screen_counts(&reads_after, &writes_after, NULL);
        assert(reads_after == reads && writes_after == writes && raster_pool_takes() == takes);
        assert(memcmp(screen_px.b, screen_expect.b, SCREEN_BYTES) == 0);
        assert(memcmp(shadow_px.b, shadow_saved.b, SCREEN_BYTES) == 0);
        assert(memcmp(refusal_src, src_saved, sizeof(src_saved)) == 0);
        comparisons++;
    }
}

static void blend_screen_refusals(void)
{
    Virtual *v = blend_screen(0, 1);                  /* 565 with a shadow */
    Workstation *w = v->real_address;
    GCBITMAP src, good_src = bitmap(refusal_src, 4, 4, 32, PX_ARGB32);
    RECT16 sr = { 0, 0, 3, 3 }, dr = { 2, 2, 5, 5 }, r;
    long reads, writes, reads_after, writes_after;
    int i;

    for (i = 0; i < SCREEN_BYTES; i++)
        screen_px.b[i] = shadow_px.b[i] = (unsigned char)(i * 13 + 5);
    /* the base case draws: one read, written back */
    memcpy(screen_expect.b, screen_px.b, SCREEN_BYTES);
    raster_screen_counts(&reads, &writes, NULL);
    transfer_blend(v, &good_src, NULL, &sr, &dr, T_BLEND);
    raster_screen_counts(&reads_after, &writes_after, NULL);
    assert(reads_after == reads + 1 && writes_after == writes + 1);
    assert(memcmp(screen_px.b, screen_expect.b, SCREEN_BYTES) != 0);
    assert(memcmp(screen_px.b, shadow_px.b, SCREEN_BYTES) == 0);
    memcpy(screen_px.b, screen_expect.b, SCREEN_BYTES);
    memcpy(shadow_px.b, screen_expect.b, SCREEN_BYTES);

    /* the vectors' refusals onto the screen */
    for (i = 0; i < ALPHA_NREFUSAL; i++)
    {
        const AlphaRefusal *c = &alpha_refusal[i];
        long bits = c->px_format == 0x01020101UL ? 1 : c->px_format == INDEX8 ? 8 : 32;

        if (c->dest != ALPHA_DEST_SCREEN)
            continue;
        src = bitmap(refusal_src, 4, 4, bits, c->px_format);
        if (bits == 1)
            src.width = 2;
        screen_refused(v, &src, &sr, &dr, c->mode);
    }
    /* the other arithmetic modes */
    for (i = 65; i < 128; i++)
    {
        screen_refused(v, &good_src, &sr, &dr, i);
        screen_refused(v, &good_src, &sr, &dr, i | 128);
    }
    /* a source in the screen or in its shadow */
    src = good_src;
    src.addr = screen_px.b + 3 * SCREEN_WRAP;
    screen_refused(v, &src, &sr, &dr, T_BLEND);
    src.addr = shadow_px.b + 7 * SCREEN_WRAP;
    screen_refused(v, &src, &sr, &dr, T_BLEND);
    /* a source rectangle outside the source */
    r = sr; r.x2 = 4;
    screen_refused(v, &good_src, &r, &dr, T_BLEND);
    /* nothing of dr on the screen, or inside the clip rectangle */
    r = dr; r.x1 = -10; r.x2 = -7;
    screen_refused(v, &good_src, &sr, &r, T_BLEND);
    r = dr; r.y1 = SCREEN_H; r.y2 = SCREEN_H + 3;
    screen_refused(v, &good_src, &sr, &r, T_BLEND);
    v->clip.on = 1;
    v->clip.rectangle.x1 = 20; v->clip.rectangle.y1 = 20;
    v->clip.rectangle.x2 = 30; v->clip.rectangle.y2 = 30;
    screen_refused(v, &good_src, &sr, &dr, T_BLEND);
    v->clip.rectangle.x1 = 4; v->clip.rectangle.y1 = 4;
    v->clip.rectangle.x2 = 3; v->clip.rectangle.y2 = 3;
    screen_refused(v, &good_src, &sr, &dr, T_BLEND);
    v->clip.on = 0;
    /* screen descriptions of no use */
    w->screen.mfdb.width = 0;
    screen_refused(v, &good_src, &sr, &dr, T_BLEND);
    w->screen.mfdb.width = SCREEN_W;
    w->screen.mfdb.height = 0;
    screen_refused(v, &good_src, &sr, &dr, T_BLEND);
    w->screen.mfdb.height = SCREEN_H;
    w->screen.wrap = SCREEN_W * 2 - 2;
    screen_refused(v, &good_src, &sr, &dr, T_BLEND);
    w->screen.wrap = SCREEN_WRAP;
    /* still drawing after all that */
    memcpy(screen_expect.b, screen_px.b, SCREEN_BYTES);
    transfer_blend(v, &good_src, NULL, &sr, &dr, T_BLEND);
    assert(memcmp(screen_px.b, screen_expect.b, SCREEN_BYTES) != 0);
}

/* A 2 x 2 opaque image onto the screen of v: whether the screen changed.
 * When it did not, nothing was read or written and no pool block taken.
 */
static int screen_draws(Virtual *v)
{
    union { unsigned long align; unsigned char b[16]; } s;
    GCBITMAP src = bitmap(s.b, 2, 2, 32, PX_ARGB32);
    RECT16 sr = { 0, 0, 1, 1 }, dr = { 3, 2, 4, 3 };
    long takes = raster_pool_takes(), reads, writes, reads_after, writes_after;
    int i, changed;

    for (i = 0; i < 4; i++)
        put_source(s.b + i * 4, 0xffc08040UL + i * 0x101010UL, PX_ARGB32);
    memset(screen_px.b, 0, SCREEN_BYTES);
    memset(screen_expect.b, 0, SCREEN_BYTES);
    raster_screen_counts(&reads, &writes, NULL);
    transfer_blend(v, &src, NULL, &sr, &dr, T_BLEND);
    raster_screen_counts(&reads_after, &writes_after, NULL);
    changed = memcmp(screen_px.b, screen_expect.b, SCREEN_BYTES) != 0;
    if (changed)
        assert(reads_after == reads + 1 && writes_after == writes + 1);
    else
        assert(reads_after == reads && writes_after == writes && raster_pool_takes() == takes);
    return changed;
}

/* transfer_blend_ok(), bit 3 of vq_extnd's work_out[30], and whether the
 * screen blends: the same answer on every kind of screen.
 */
static void blend_ok_layouts(void)
{
    Virtual *v;
    Workstation *w;
    Device *dev;
    int l, kind, i;

    for (l = 0; l < 4; l++)
        for (kind = 0; kind < 3; kind++)
        {
            v = blend_screen(l, kind);
            assert(transfer_blend_ok(v->real_address) == 1 && screen_draws(v));
        }
    /* 8 planes */
    v = blend_screen(0, 0);
    v->real_address->screen.mfdb.bitplanes = 8;
    assert(!transfer_blend_ok(v->real_address) && !screen_draws(v));
    /* 16 bits in a layout the engine does not know: red not in one run,
       blue at the top, six bits of green under red at bit 10, planes */
    v = blend_screen(0, 0);
    v->real_address->driver->device->scrmap.bitnumber.red[3] = 0;
    assert(!transfer_blend_ok(v->real_address) && !screen_draws(v));
    v = blend_screen(0, 0);
    dev = v->real_address->driver->device;
    for (i = 0; i < 5; i++)
    {
        dev->scrmap.bitnumber.red[i] = i;
        dev->scrmap.bitnumber.blue[i] = 11 + i;
    }
    assert(!transfer_blend_ok(v->real_address) && !screen_draws(v));
    v = blend_screen(1, 0);
    dev = v->real_address->driver->device;
    dev->bits.green = 6;
    dev->scrmap.bitnumber.green[5] = 10;
    assert(!transfer_blend_ok(v->real_address) && !screen_draws(v));
    v = blend_screen(0, 0);
    v->real_address->driver->device->format = 0;
    assert(!transfer_blend_ok(v->real_address) && !screen_draws(v));
    /* 24 and 32 bits */
    for (i = 24; i <= 32; i += 8)
    {
        int c;

        v = blend_screen(0, 0);
        w = v->real_address;
        dev = w->driver->device;
        w->screen.mfdb.bitplanes = i;
        dev->bit_depth = i;
        dev->bits.red = dev->bits.green = dev->bits.blue = 8;
        for (c = 0; c < 8; c++)
        {
            dev->scrmap.bitnumber.red[c] = 16 + c;
            dev->scrmap.bitnumber.green[c] = 8 + c;
            dev->scrmap.bitnumber.blue[c] = c;
        }
        assert(!transfer_blend_ok(w) && !screen_draws(v));
    }
    /* no device, no driver */
    v = blend_screen(0, 0);
    v->real_address->driver->device = NULL;
    assert(!transfer_blend_ok(v->real_address) && !screen_draws(v));
    v = blend_screen(0, 0);
    v->real_address->driver = NULL;
    assert(!transfer_blend_ok(v->real_address) && !screen_draws(v));
    comparisons += 21;
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
    blend_opaque_sources();
    blend_screen_images();
    blend_screen_strips();
    blend_screen_refusals();
    blend_ok_layouts();
    modes_0_32_formats();
    weight_accessor();
    assert(raster_allocations() == allocations);
    printf("%ld alpha comparisons passed (vectors 0x%08lx; k table, pixels in 565/555/gap layouts both byte orders, memory images, refusals, routing, opaque sources, modes 0/32, weight)\n",
           comparisons, (unsigned long)ALPHA_VECTORS_SUM);
    printf("%ld alpha screen images passed (565/555/gap layouts, with and without a shadow or an address, clipped, in strips); %ld read-backs, each written back as the same rectangle\n",
           screen_images, screen_read_backs);
}
