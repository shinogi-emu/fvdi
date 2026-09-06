/* Host reference checks for the RGB32 -> RGB16 transfer path.
 * Run with make -C fvdi/tests check. No Atari OS/emulator is required.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fvdi.h"
#include "function.h"
#include "test_support.h"
#ifdef __m68k__
#include <mint/osbind.h>
#endif

void indexed_tests(void);
void indexed_screen_blit(Virtual *, const short *, const MFDB *);
void legacy_tests(void);
void legacy_test_blit(void *, Virtual *, long, void *, void *, void *, void *);
extern int legacy_test_active;

static void reference_progress(const char *message)
{
#ifdef __m68k__
    while (*message) Bconout(1, *message++);
#else
    (void)message;
#endif
}

#ifdef __m68k__
void reference_fail(const char *condition, const char *file, long line)
{
    char message[256];
    snprintf(message, sizeof(message), "\r\nFVDI TRANSFER FAIL: %s:%ld: %s\r\n", file, line, condition);
    reference_progress(message);
    exit(1);
}
#endif

static union {
    unsigned long alignment;
    unsigned char bytes[16416];
} pool;
static long pool_capacity = 10240;
static int pool_busy, pool_unavailable, blits;
static Workstation wk;
static Virtual vwk;
static Driver driver;
static Device device;

void raster_pool(long capacity, int unavailable)
{
    assert(!pool_busy);
    pool_capacity = capacity;
    pool_unavailable = unavailable;
}

char *allocate_block(long size)
{
    assert(!pool_busy);
    if (pool_unavailable || size > pool_capacity)
        return NULL;
    memset(pool.bytes, 0xa5, sizeof(pool.bytes));
    *(long *)(pool.bytes + 16) = pool_capacity;
    pool_busy = 1;
    return (char *)pool.bytes + 16;
}

void free_block(void *addr)
{
    long i;
    assert(pool_busy && addr == pool.bytes + 16);
    for (i = 0; i < 16; i++)
        assert(pool.bytes[i] == 0xa5);
    for (i = pool_capacity + 16; i < (long)sizeof(pool.bytes); i++)
        assert(pool.bytes[i] == 0xa5);
    pool_busy = 0;
}

void lib_vro_cpyfm(Virtual *v, short mode, short *xy, MFDB *src, MFDB *dst)
{
    (void)v; (void)mode; (void)xy; (void)src; (void)dst;
    abort(); /* Only the assembly ABI adapter below may be used. */
}

void lib_vrt_cpyfm(Virtual *v, short mode, short *xy, MFDB *src, MFDB *dst, short *pens)
{
    (void)v; (void)mode; (void)xy; (void)src; (void)dst; (void)pens;
    abort();
}

void lib_vdi_spppp(void *func, Virtual *v, long mode, void *points,
                  void *source, void *dest, void *unused)
{
    short *xy = points;
    MFDB *s = source;
    long x, y;

    if (legacy_test_active)
    {
        legacy_test_blit(func, v, mode, points, source, dest, unused);
        blits++;
        return;
    }
    if (s->bitplanes <= 8)
    {
        assert(func == lib_vro_cpyfm && mode == 3 && dest == NULL && unused == NULL);
        indexed_screen_blit(v, xy, s);
        blits++;
        return;
    }
    assert(func == lib_vro_cpyfm && v == &vwk && mode == 3);
    assert(dest == NULL && unused == NULL && s->standard == 0 && s->bitplanes == 16);
    assert(xy[0] == 0 && xy[1] == 0);
    assert(xy[2] == s->width - 1 && xy[3] == s->height - 1);
    assert(xy[6] - xy[4] == xy[2] && xy[7] - xy[5] == xy[3]);
    assert(xy[4] >= 0 && xy[5] >= 0);
    assert(xy[6] < wk.screen.mfdb.width && xy[7] < wk.screen.mfdb.height);
    if (v->clip.on)
    {
        assert(xy[4] >= v->clip.rectangle.x1 && xy[5] >= v->clip.rectangle.y1);
        assert(xy[6] <= v->clip.rectangle.x2 && xy[7] <= v->clip.rectangle.y2);
    }
    for (y = 0; y < s->height; y++)
        for (x = 0; x < s->width * 2; x++)
        {
            long target = (xy[5] + y) * wk.screen.wrap + xy[4] * 2 + x;
            unsigned char value = ((unsigned char *)s->address)[y * s->wdwidth * 32 + x];
            ((unsigned char *)wk.screen.mfdb.address)[target] = value;
            if (wk.screen.shadow.address)
                ((unsigned char *)wk.screen.shadow.address)[target] = value;
        }
    blits++;
}

static void screen_setup(int w, int h, unsigned char *pixels, int green_bits, int red_start)
{
    int i, green_start = red_start - green_bits;

    memset(&wk, 0, sizeof(wk));
    memset(&vwk, 0, sizeof(vwk));
    memset(&device, 0, sizeof(device));
    vwk.real_address = &wk;
    wk.driver = &driver;
    driver.device = &device;
    wk.screen.mfdb.address = (short *)pixels;
    wk.screen.mfdb.width = w;
    wk.screen.mfdb.height = h;
    wk.screen.mfdb.bitplanes = 16;
    wk.screen.wrap = w * 2;
    device.format = 2;
    device.bit_depth = 16;
    device.bits.red = device.bits.blue = 5;
    device.bits.green = green_bits;
    for (i = 0; i < 5; i++)
    {
        device.scrmap.bitnumber.red[i] = red_start + i;
        device.scrmap.bitnumber.blue[i] = i;
    }
    for (i = 0; i < green_bits; i++)
        device.scrmap.bitnumber.green[i] = green_start + i;
}

static GCBITMAP bitmap(unsigned char *p, int w, int h, int bpp, unsigned long format)
{
    GCBITMAP bm;
    memset(&bm, 0, sizeof(bm));
    bm.addr = p;
    bm.width = w * (bpp / 8);
    bm.bits = bpp;
    bm.px_format = format;
    bm.xmax = w;
    bm.ymax = h;
    return bm;
}

/* Independent reference: direct 64-bit coordinate formula and four weighted
 * samples, deliberately without the production rational-stepping recurrence.
 */
unsigned long reference_channel(const GCBITMAP *src, long x, long y, int channel)
{
    const unsigned char *p = src->addr + y * src->width + x * (src->bits / 8);
    unsigned long value, bits, shift;
    if (src->bits == 32) return p[channel + 1];
    value = src->px_format & 0x00800000UL ? p[1] * 256UL + p[0] : p[0] * 256UL + p[1];
    bits = channel == 1 && (src->px_format & ~0x00800000UL) == 0x03021010UL ? 6 : 5;
    shift = channel == 2 ? 0 : channel == 1 ? 5 :
            (src->px_format & ~0x00800000UL) == 0x03021010UL ? 11 : 10;
    value = value >> shift & ((1UL << bits) - 1);
    /* Integer replication expressed independently of the production shifts. */
    return value * (1UL << (8 - bits)) + value / (1UL << (2 * bits - 8));
}

static unsigned short reference(const GCBITMAP *src, RECT16 sr, RECT16 dr,
                                 long dx, long dy, int rshift, int gshift, int gbits)
{
    long sw = (long)sr.x2 - sr.x1 + 1, sh = (long)sr.y2 - sr.y1 + 1;
    long dw = (long)dr.x2 - dr.x1 + 1, dh = (long)dr.y2 - dr.y1 + 1;
    long x, y, x2, y2, fx = 0, fy = 0;
    unsigned long colour[3];
    int k;

    dx -= dr.x1;
    dy -= dr.y1;
    if (src->bits == 8)
    {
        COLOR_RGB rgb;
        x = (int64_t)dx * sw / dw + sr.x1 - src->xmin;
        y = (int64_t)dy * sh / dh + sr.y1 - src->ymin;
        rgb = src->ctab->colors[src->addr[y * src->width + x]].rgb;
        return ((rgb.red >> 11) << rshift) |
               ((rgb.green >> (16 - gbits)) << gshift) | (rgb.blue >> 11);
    }
    if (sw > dw || sh > dh)
    {
        x = ((2 * (int64_t)dx + 1) * sw * 128) / dw - 128;
        y = ((2 * (int64_t)dy + 1) * sh * 128) / dh - 128;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > (sw - 1) * 256) x = (sw - 1) * 256;
        if (y > (sh - 1) * 256) y = (sh - 1) * 256;
        fx = x % 256;
        fy = y % 256;
        x /= 256;
        y /= 256;
    } else
    {
        x = dx * sw / dw;
        y = dy * sh / dh;
    }
    x += sr.x1 - src->xmin;
    y += sr.y1 - src->ymin;
    x2 = x + (fx != 0);
    y2 = y + (fy != 0);
    for (k = 0; k < 3; k++)
    {
        uint64_t sum = (uint64_t)reference_channel(src, x, y, k) * (256 - fx) * (256 - fy) +
                       (uint64_t)reference_channel(src, x2, y, k) * fx * (256 - fy) +
                       (uint64_t)reference_channel(src, x, y2, k) * (256 - fx) * fy +
                       (uint64_t)reference_channel(src, x2, y2, k) * fx * fy;
        colour[k] = (sum + 32768) / 65536;
    }
    return ((colour[0] >> 3) << rshift) |
           ((colour[1] >> (8 - gbits)) << gshift) | (colour[2] >> 3);
}

static void fill_source(unsigned char *p, size_t bytes)
{
    size_t i;
    for (i = 0; i < bytes; i++)
        p[i] = (i * 73 + i / 7 + 39) & 255;
}

static void check_case(int sw, int sh, int dw, int dh, int screen, int variant, int clip, int kind)
{
    int indexed = kind == 1, bytes = indexed ? 1 : kind > 1 ? 2 : 4;
    unsigned long format = indexed ? 0x01020808UL : kind == 0 ? 0x03421820UL :
                           kind & 1 ? 0x03420f10UL : 0x03021010UL;
    long stride = (sw + 4) * bytes + (indexed ? 3 : 2);
    size_t source_bytes = stride * (sh + 4) + indexed;
    int target_w = dw + 6, target_h = dh + 6;
    size_t target_bytes = target_w * target_h * 2;
    unsigned char *s = malloc(source_bytes), *saved = malloc(source_bytes);
    unsigned char *d = malloc(target_bytes), *expected = malloc(target_bytes);
    unsigned char *shadow = malloc(target_bytes);
    GCBITMAP src = bitmap(s + indexed, sw + 4, sh + 4, bytes * 8,
                         format | (kind >= 4 ? 0x00800000UL : 0));
    COLOR_TAB palette;
    GCBITMAP dst = bitmap(d, target_w, target_h, 16, 0x03021010UL);
    RECT16 sr = { 2, 2, sw + 1, sh + 1 };
    RECT16 dr = { 3, 3, dw + 2, dh + 2 };
    long x, y;
    int r = variant == 2 ? 10 : 11, gb = variant == 0 ? 6 : 5;
    int g = r - gb, reverse = !screen && variant == 3;

    assert(s && saved && d && expected && shadow);
    memset(&palette, 0, sizeof(palette));
    palette.magic = 0x63746162L; palette.length = sizeof(palette);
    palette.color_space = 1; palette.no_colors = 256;
    for (x = 0; x < 256; x++)
    {
        palette.colors[x].rgb.red = (x * 3119UL) & 65535;
        palette.colors[x].rgb.green = (x * 7919UL + 511) & 65535;
        palette.colors[x].rgb.blue = (x * 1777UL + 31337) & 65535;
    }
    if (indexed) src.ctab = &palette;
    src.width = stride;
    /* Nonzero (and negative) bitmap origins are independent of byte stride. */
    src.xmin -= 7; src.xmax -= 7; sr.x1 -= 7; sr.x2 -= 7;
    src.ymin += 11; src.ymax += 11; sr.y1 += 11; sr.y2 += 11;
    if (!screen)
    {
        r = variant & 1 ? 10 : 11;
        gb = variant & 1 ? 5 : 6;
        g = 5;
        dst.px_format = variant & 1 ? 0x03420f10UL : 0x03021010UL;
        if (reverse) dst.px_format |= 0x00800000UL;
        dst.xmin -= 9; dst.xmax -= 9; dr.x1 -= 9; dr.x2 -= 9;
    }
    if (screen && clip == 2)
    {
        dr.x1 -= 6; dr.x2 -= 6;
        dr.y1 -= 5; dr.y2 -= 5;
    }
    screen_setup(target_w, target_h, screen ? d : NULL, gb, r);
    wk.screen.shadow.address = screen ? shadow : NULL;
    fill_source(s, source_bytes);
    memcpy(saved, s, source_bytes);
    memset(d, 0xcd, target_bytes);
    memset(shadow, 0xcd, target_bytes);
    memset(expected, 0xcd, target_bytes);
    if (clip == 1)
    {
        vwk.clip.on = 1;
        vwk.clip.rectangle.x1 = dw > 2 ? 4 : 3;
        vwk.clip.rectangle.y1 = dh > 2 ? 4 : 3;
        vwk.clip.rectangle.x2 = dw > 2 ? dw + 1 : dw + 2;
        vwk.clip.rectangle.y2 = dh > 2 ? dh + 1 : dh + 2;
    }
    for (y = dr.y1; y <= dr.y2; y++)
        for (x = dr.x1; x <= dr.x2; x++)
        {
            unsigned short pixel;
            long offset = (y * target_w + x - (screen ? 0 : dst.xmin)) * 2;
            if (screen && (x < 0 || y < 0 || x >= target_w || y >= target_h))
                continue;
            if (screen && clip == 1 &&
                (x < vwk.clip.rectangle.x1 || x > vwk.clip.rectangle.x2 ||
                 y < vwk.clip.rectangle.y1 || y > vwk.clip.rectangle.y2))
                continue;
            pixel = reference(&src, sr, dr, x, y, r, g, gb);
            expected[offset] = reverse ? pixel : pixel >> 8;
            expected[offset + 1] = reverse ? pixel >> 8 : pixel;
        }
    assert(transfer_rgb16(&vwk, &src, screen ? NULL : &dst, &sr, &dr, variant & 1 ? 160 : 0) == 1);
    assert(!pool_busy);
    assert(memcmp(s, saved, source_bytes) == 0);
    if (memcmp(d, expected, target_bytes) != 0)
    {
        fprintf(stderr, "Mismatch %dx%d -> %dx%d screen=%d variant=%d clip=%d\n",
                sw, sh, dw, dh, screen, variant, clip);
        abort();
    }
    if (screen) assert(memcmp(shadow, expected, target_bytes) == 0);
    free(s); free(saved); free(d); free(expected); free(shadow);
}

static void extreme_coordinates(void)
{
    unsigned char *s = malloc(65536UL * 4), *d = malloc(65536UL * 2);
    GCBITMAP src = bitmap(s, 65536L, 1, 32, 0x03421820UL);
    GCBITMAP dst = bitmap(d, 1, 1, 16, 0x03021010UL);
    RECT16 sr = { -32768, 0, 32767, 0 }, dr = { 0, 0, 0, 0 };
    unsigned short pixel;
    long x;

    assert(s && d);
    src.xmin = -32768L; src.xmax = 32768L;
    fill_source(s, 65536UL * 4);
    screen_setup(1, 1, NULL, 6, 11);
    pixel = reference(&src, sr, dr, 0, 0, 11, 5, 6);
    assert(transfer_rgb16(&vwk, &src, &dst, &sr, &dr, 0) == 1);
    assert(d[0] == (pixel >> 8) && d[1] == (pixel & 255));
    /* Reverse ratio, maximum RECT16 width, with a negative origin. */
    src.xmin = 0; src.xmax = 1; src.width = 4;
    dst.xmin = -32768L; dst.xmax = 32768L; dst.width = 65536UL * 2;
    sr.x1 = sr.x2 = 0;
    dr.x1 = -32768; dr.x2 = 32767;
    pixel = reference(&src, sr, dr, -32768, 0, 11, 5, 6);
    assert(transfer_rgb16(&vwk, &src, &dst, &sr, &dr, 0) == 1);
    for (x = 0; x < 65536L; x++)
        assert(d[x * 2] == (pixel >> 8) && d[x * 2 + 1] == (pixel & 255));
    free(s); free(d);
}

static void rejected(void)
{
    unsigned char s[128] = { 0 }, d[128];
    GCBITMAP src = bitmap(s, 4, 4, 32, 0x03421820UL);
    GCBITMAP dst = bitmap(d, 4, 4, 16, 0x03021010UL);
    RECT16 sr = { 0, 0, 3, 3 }, dr = sr;
    int i;

    screen_setup(4, 4, d, 6, 11);
    memset(d, 0x7b, sizeof(d));
    assert(transfer_rgb16(&vwk, &src, NULL, &sr, &dr, 33) == 1);
    src.width = 14;
    assert(transfer_rgb16(&vwk, &src, NULL, &sr, &dr, 0) == 1);
    src.width = 16;
    sr.x2 = 4;
    assert(transfer_rgb16(&vwk, &src, NULL, &sr, &dr, 0) == 1);
    sr.x2 = 3;
    device.scrmap.bitnumber.red[3] = 0;
    assert(transfer_rgb16(&vwk, &src, NULL, &sr, &dr, 0) == 1);
    device.scrmap.bitnumber.red[3] = 14;
    pool_unavailable = 1;
    assert(transfer_rgb16(&vwk, &src, NULL, &sr, &dr, 0) == 1);
    pool_unavailable = 0;
    assert(transfer_rgb16(&vwk, &src, &dst, &sr, &dr, 0) == 1); /* explicit screen alias */
    dst.addr = s + 2;
    assert(transfer_rgb16(&vwk, &src, &dst, &sr, &dr, 0) == 1); /* overlap */
    dst.addr = d;
    wk.screen.mfdb.address = NULL;
    dst.width = 0x7fffffffL;
    assert(transfer_rgb16(&vwk, &src, &dst, &sr, &dr, 0) == 1);
    dst.width = 8;
    dst.px_format = 123;
    assert(transfer_rgb16(&vwk, &src, &dst, &sr, &dr, 0) == 1);
    src.px_format = 123;
    assert(transfer_rgb16(&vwk, &src, &dst, &sr, &dr, 0) == 0);
    for (i = 0; i < (int)sizeof(d); i++) assert(d[i] == 0x7b);
}

int main(void)
{
    static const int sizes[] = { 1, 2, 3, 7, 16, 19, 33 };
    unsigned long cases = 0;
    int a, b, c, screen, variant, indexed;

    reference_progress("\r\nFVDI REFERENCE START\r\n");
    for (indexed = 0; indexed < 6; indexed++)
    {
        for (screen = 0; screen < 2; screen++)
            for (variant = 0; variant < 4; variant++)
                for (a = 0; a < 7; a++)
                    for (b = 0; b < 7; b++)
                        for (c = 0; c < 7; c++)
                        {
                            check_case(sizes[a], sizes[b], sizes[c], sizes[(a + b + c) % 7],
                                       screen, variant, (a + b) & 1, indexed);
                            cases++;
                        }
        reference_progress("FVDI REFERENCE source format passed\r\n");
    }
    /* Exercise both horizontal chunking and multiple strips per image. */
    for (indexed = 0; indexed < 6; indexed++)
    {
        pool_capacity = 32;
        check_case(37, 29, 83, 53, 1, 0, 1, indexed);
        check_case(83, 53, 37, 29, 1, 1, 1, indexed);
        pool_capacity = 128;
        check_case(37, 53, 83, 29, 1, 2, 1, indexed);
        check_case(37, 53, 83, 29, 1, 0, 2, indexed);
    }
    extreme_coordinates();
    rejected();
    reference_progress("FVDI REFERENCE indexed checks\r\n");
    indexed_tests();
    reference_progress("FVDI REFERENCE legacy checks\r\n");
    legacy_tests();
    printf("%lu reference comparisons passed; bounded screen staging (%d blits) and refusal checks passed\n",
           cases + 8, blits);
#ifdef __m68k__
    {
        const char *message = "\r\nFVDI TRANSFER REFERENCE PASS\r\n";
        while (*message) Bconout(1, *message++);
    }
#endif
    return 0;
}
