/* Independent indexed-raster references and allocation/lifecycle checks. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fvdi.h"
#include "function.h"
#include "itable.h"
#include "ctab.h"
#include "test_support.h"

void raster_pool(long, int);
unsigned long reference_channel(const GCBITMAP *, long, long, int);

static void planar_store(unsigned char *screen, long stride, int planes, int x, int y, int index)
{
    int p;
    for (p = 0; p < planes; p++)
    {
        long offset = y * stride + (x / 16 * planes + p) * 2 + (x % 16 >= 8);
        unsigned char mask = 0x80 >> (x % 8);
        screen[offset] = (screen[offset] & ~mask) | ((index & (1 << p)) ? mask : 0);
    }
}

void indexed_screen_blit(Virtual *v, const short *xy, const MFDB *src)
{
    Workstation *wk = v->real_address;
    const unsigned char *s = (const unsigned char *)src->address;
    int x, p;
    assert(src->standard == 0 && src->height == 1 && src->bitplanes == wk->screen.mfdb.bitplanes);
    assert(xy[0] == 0 && xy[1] == 0 && xy[3] == 0 && xy[5] == xy[7]);
    assert(xy[2] == src->width - 1 && xy[6] - xy[4] == xy[2]);
    assert(xy[4] >= 0 && xy[6] < wk->screen.mfdb.width && xy[5] >= 0 && xy[5] < wk->screen.mfdb.height);
    for (x = 0; x < src->width; x++)
    {
        int index = 0;
        for (p = 0; p < src->bitplanes; p++)
        {
            long offset = (x / 16 * src->bitplanes + p) * 2;
            unsigned short word = s[offset] * 256U + s[offset + 1];
            if (word & (0x8000U >> (x % 16))) index |= 1 << p;
        }
        planar_store((unsigned char *)wk->screen.mfdb.address, wk->screen.wrap,
                     src->bitplanes, xy[4] + x, xy[5], index);
        if (wk->screen.shadow.address)
            planar_store(wk->screen.shadow.address, wk->screen.wrap,
                         src->bitplanes, xy[4] + x, xy[5], index);
    }
}

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void __real_free(void *);
static int fail_malloc, fail_calloc;
static long allocations;
static size_t last_malloc, last_calloc;
static int palette_writes;

void copymem_aligned(const void *s, void *d, long n) { memcpy(d, s, n); }
void CDECL set_palette(Virtual *vwk, DrvPalette *pars)
{
    (void)vwk;
    assert(pars->palette && pars->requested && pars->count > 0);
    palette_writes++;
}

void *__wrap_malloc(size_t size)
{
    void *p;
    last_malloc = size;
    if (fail_malloc) { fail_malloc = 0; return NULL; }
    p = __real_malloc(size);
    if (p) allocations++;
    return p;
}

void *__wrap_calloc(size_t count, size_t size)
{
    void *p;
    last_calloc = count * size;
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    p = __real_calloc(count, size);
    if (p) allocations++;
    return p;
}

void __wrap_free(void *p)
{
    if (p) allocations--;
    __real_free(p);
}

static void palette(COLOR_TAB *p, int count)
{
    int i;
    memset(p, 0, sizeof(*p));
    p->magic = 0x63746162L;
    p->length = offsetof(COLOR_TAB, colors) + count * sizeof(COLOR_ENTRY);
    p->color_space = 1;
    p->no_colors = count;
    for (i = 0; i < count; i++)
    {
        p->colors[i].rgb.red = (i * 4177UL + 711) & 65535;
        p->colors[i].rgb.green = (i * 2017UL + 31337) & 65535;
        p->colors[i].rgb.blue = (i * 7919UL + 5133) & 65535;
    }
}

/* Search cell centres directly, without reading the production lookup. */
static unsigned char nearest(const COLOR_TAB *p, int bits, unsigned long rgb)
{
    long bin = 256 / (1 << bits), best = 0x7fffffffL;
    long r = (((rgb >> 16) & 255) / bin) * bin + bin / 2;
    long g = (((rgb >> 8) & 255) / bin) * bin + bin / 2;
    long b = ((rgb & 255) / bin) * bin + bin / 2;
    unsigned char result = 0;
    int i;
    for (i = 0; i < p->no_colors; i++)
    {
        long dr = r - p->colors[i].rgb.red / 256;
        long dg = g - p->colors[i].rgb.green / 256;
        long db = b - p->colors[i].rgb.blue / 256;
        long distance = dr * dr + dg * dg + db * db;
        if (distance < best) { best = distance; result = i; }
    }
    return result;
}

static GCBITMAP bitmap(unsigned char *pixels, int w, int h, int bytes)
{
    GCBITMAP b;
    memset(&b, 0, sizeof(b));
    b.addr = pixels;
    b.width = w * bytes;
    b.xmax = w;
    b.ymax = h;
    b.bits = bytes * 8;
    b.px_format = bytes == 4 ? 0x03421820UL : 0x01020808UL;
    return b;
}

static unsigned long sample(const GCBITMAP *src, RECT16 sr, int dw, int dh,
                             int dx, int dy, int filtered)
{
    long sw = (long)sr.x2 - sr.x1 + 1, sh = (long)sr.y2 - sr.y1 + 1;
    long x, y, fx = 0, fy = 0, k;
    unsigned long rgb = 0;
    if (filtered && (sw > dw || sh > dh))
    {
        x = ((2 * (int64_t)dx + 1) * sw * 128) / dw - 128;
        y = ((2 * (int64_t)dy + 1) * sh * 128) / dh - 128;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > (sw - 1) * 256) x = (sw - 1) * 256;
        if (y > (sh - 1) * 256) y = (sh - 1) * 256;
        fx = x % 256; fy = y % 256;
        x /= 256; y /= 256;
    } else
    {
        x = (int64_t)dx * sw / dw;
        y = (int64_t)dy * sh / dh;
    }
    x += sr.x1 - src->xmin;
    y += sr.y1 - src->ymin;
    for (k = 0; k < 3; k++)
    {
        long x1 = x + (fx != 0), y1 = y + (fy != 0);
        long a, b, c, d;
        uint64_t sum;
        if (src->bits == 8)
        {
            const COLOR_ENTRY *pal = src->ctab->colors;
            COLOR_RGB ra = pal[src->addr[y * src->width + x]].rgb;
            COLOR_RGB rb = pal[src->addr[y * src->width + x1]].rgb;
            COLOR_RGB rc = pal[src->addr[y1 * src->width + x]].rgb;
            COLOR_RGB rd = pal[src->addr[y1 * src->width + x1]].rgb;
            a = (k == 0 ? ra.red : k == 1 ? ra.green : ra.blue) / 256;
            b = (k == 0 ? rb.red : k == 1 ? rb.green : rb.blue) / 256;
            c = (k == 0 ? rc.red : k == 1 ? rc.green : rc.blue) / 256;
            d = (k == 0 ? rd.red : k == 1 ? rd.green : rd.blue) / 256;
        } else
        {
            a = reference_channel(src, x, y, k); b = reference_channel(src, x1, y, k);
            c = reference_channel(src, x, y1, k); d = reference_channel(src, x1, y1, k);
        }
        sum = (uint64_t)a * (256 - fx) * (256 - fy) + (uint64_t)b * fx * (256 - fy) +
              (uint64_t)c * (256 - fx) * fy + (uint64_t)d * fx * fy;
        rgb = (rgb << 8) | ((sum + 32768) / 65536);
    }
    return rgb;
}

static int exact_index(const COLOR_TAB *source, const COLOR_TAB *dest, int index)
{
    COLOR_RGB rgb = source->colors[index].rgb;
    int i;
    /* Check the same index first, then the destination from the beginning. */
    for (i = -1; i < dest->no_colors; i++)
    {
        int j = i < 0 ? index : i;
        if (j < dest->no_colors && rgb.red == dest->colors[j].rgb.red &&
            rgb.green == dest->colors[j].rgb.green && rgb.blue == dest->colors[j].rgb.blue)
            return j;
    }
    return -1;
}

static void reference(const GCBITMAP *src, RECT16 sr, const GCBITMAP *dst, RECT16 dr,
                      unsigned char *pixels, int bits, int dither)
{
    int w = dr.x2 - dr.x1 + 1, h = dr.y2 - dr.y1 + 1, x, y, k;
    /* Two wide error rows, unlike the production in-place short row. */
    long *a = calloc((w + 2) * 3, sizeof(long));
    long *b = calloc((w + 2) * 3, sizeof(long));
    assert(a && b);
    if (src->bits == 8)
    {
        int exact = 1;
        for (k = 0; k < src->ctab->no_colors; k++)
            if (exact_index(src->ctab, dst->ctab, k) < 0) exact = 0;
        if (exact) dither = 0;
    }
    for (y = 0; y < h; y++)
    {
        long *swap;
        memset(b, 0, (w + 2) * 3 * sizeof(long));
        for (x = 0; x < w; x++)
        {
            unsigned long rgb = sample(src, sr, w, h, x, y, src->bits != 8 || dither), adjusted = 0;
            long channel[3];
            unsigned char index;
            for (k = 0; k < 3; k++)
            {
                long e = dither ? a[(x + 1) * 3 + k] + 8 : 0;
                long correction = e < 0 ? -((-e + 15) / 16) : e / 16;
                long c = ((rgb >> (16 - 8 * k)) & 255) + correction;
                channel[k] = c < 0 ? 0 : c > 255 ? 255 : c;
                adjusted = (adjusted << 8) | channel[k];
            }
            index = nearest(dst->ctab, bits, adjusted);
            if (src->bits == 8 && !dither)
            {
                long sx = (int64_t)x * (sr.x2 - sr.x1 + 1) / w + sr.x1 - src->xmin;
                long sy = (int64_t)y * (sr.y2 - sr.y1 + 1) / h + sr.y1 - src->ymin;
                int exact = exact_index(src->ctab, dst->ctab, src->addr[sy * src->width + sx]);
                if (exact >= 0) index = exact;
            }
            pixels[(y + dr.y1 - dst->ymin) * dst->width + x + dr.x1 - dst->xmin] = index;
            if (!dither) continue;
            channel[0] -= dst->ctab->colors[index].rgb.red / 256;
            channel[1] -= dst->ctab->colors[index].rgb.green / 256;
            channel[2] -= dst->ctab->colors[index].rgb.blue / 256;
            for (k = 0; k < 3; k++)
            {
                a[(x + 2) * 3 + k] += channel[k] * 7;
                b[x * 3 + k] += channel[k] * 3;
                b[(x + 1) * 3 + k] += channel[k] * 5;
                b[(x + 2) * 3 + k] += channel[k];
            }
        }
        swap = a; a = b; b = swap;
    }
    free(a); free(b);
}

static void check_image(Virtual *v, COLOR_TAB *pal, void *id, int bits,
                        int sw, int sh, int dw, int dh, int dither, COLOR_TAB *source_pal)
{
    int bytes = source_pal ? 1 : 4;
    long ss = (sw + 2) * bytes + (source_pal ? 3 : 2), ds = dw + 5;
    size_t sn = ss * (sh + 2), dn = ds * (dh + 2);
    unsigned char *s = malloc(sn + 2), *saved = malloc(sn + 2), *d = malloc(dn + 2), *expected = malloc(dn + 2);
    GCBITMAP src = bitmap(s + (source_pal != NULL), sw + 2, sh + 2, bytes);
    GCBITMAP dst = bitmap(d + 1, dw + 2, dh + 2, 1); /* Odd address/stride are valid. */
    RECT16 sr = { -2, -1, sw - 3, sh - 2 }, dr = { -8, 6, dw - 9, dh + 5 };
    long before;
    size_t i;

    assert(s && saved && d && expected);
    src.width = ss; src.xmin = -3; src.xmax -= 3; src.ymin = -2; src.ymax -= 2;
    dst.width = ds; dst.xmin = -9; dst.xmax -= 9; dst.ymin = 5; dst.ymax += 5;
    dst.ctab = pal; dst.itab = id;
    src.ctab = source_pal;
    for (i = 0; i < sn + 2; i++) s[i] = (i * 91 + i / 9 + 17) % (source_pal ? source_pal->no_colors : 256);
    memcpy(saved, s, sn + 2);
    memset(d, 0xa7, dn + 2); memset(expected, 0xa7, dn + 2);
    reference(&src, sr, &dst, dr, expected + 1, bits, dither);
    last_calloc = 0;
    before = allocations;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 32 | (dither ? 128 : 0)) == 1);
    assert(allocations == before);
    if (source_pal)
    {
        int exact = 1, k;
        for (k = 0; k < source_pal->no_colors; k++)
            if (exact_index(source_pal, pal, k) < 0) exact = 0;
        assert(last_calloc == (dither && !exact ? (size_t)dw * 6 : 0));
    } else if (dither) assert(last_calloc == (size_t)dw * 6);
    assert(memcmp(d, expected, dn + 2) == 0);
    assert(memcmp(s, saved, sn + 2) == 0);
    free(s); free(saved); free(d); free(expected);
}

static int indexed_screens(void)
{
    Virtual v;
    Workstation wk;
    Driver driver;
    Device device;
    Colour colors[256];
    COLOR_TAB sp;
    unsigned char *s = malloc(88UL * 60 * 4 + 2), *screen = malloc(70UL * 48),
                  *shadow = malloc(70UL * 48), *expected = malloc(70UL * 48),
                  *indices = malloc(79UL * 55);
    int planes, kind, dither, shrink, clip, blocks, i, x, y, cases = 0;
    long baseline;
    assert(s && screen && shadow && expected && indices);
    baseline = allocations;
    memset(&v, 0, sizeof(v)); memset(&wk, 0, sizeof(wk));
    memset(&driver, 0, sizeof(driver)); memset(&device, 0, sizeof(device));
    memset(colors, 0, sizeof(colors));
    v.real_address = &wk; wk.driver = &driver; driver.device = &device;
    device.clut = 1; device.format = 0;
    wk.screen.mfdb.address = (short *)screen; wk.screen.shadow.address = shadow;
    wk.screen.mfdb.width = 64; wk.screen.mfdb.height = 48;
    wk.screen.palette.colours = colors;
    palette(&sp, 17);
    for (planes = 1; planes <= 8; planes *= 2)
    {
        wk.screen.mfdb.bitplanes = planes;
        wk.screen.wrap = 8 * planes + 2;
        wk.screen.palette.size = 1L << planes;
        for (i = 0; i < wk.screen.palette.size; i++)
        {
            colors[i].vdi.red = (i * 191) % 1001;
            colors[i].vdi.green = (i * 313 + 250) % 1001;
            colors[i].vdi.blue = (i * 719 + 500) % 1001;
        }
        for (kind = 0; kind < 6; kind++)
        for (dither = 0; dither < 2; dither++)
        for (shrink = 0; shrink < 2; shrink++)
        for (clip = 0; clip < 2; clip++)
        for (blocks = 0; blocks < 2; blocks++)
        {
            int sw = shrink ? 83 : 13, sh = shrink ? 53 : 9;
            int dw = shrink ? 51 : 79, dh = shrink ? 31 : 55;
            int indexed = kind == 1;
            GCBITMAP src = bitmap(s + indexed, sw + 2, sh + 2, indexed ? 1 : kind > 1 ? 2 : 4);
            GCBITMAP dst = bitmap(indices, dw, dh, 1);
            RECT16 sr = { 1, 1, sw, sh }, dr = { -3, -2, dw - 4, dh - 3 };
            long bytes = wk.screen.wrap * 48;
            if (kind > 1) src.px_format = ((kind & 1) ? 0x03420f10UL : 0x03021010UL) |
                                        (kind >= 4 ? 0x00800000UL : 0);
            src.width += indexed ? 3 : 2;
            src.ctab = indexed ? &sp : NULL;
            dst.xmin = -3; dst.xmax -= 3; dst.ymin = -2; dst.ymax -= 2;
            dst.ctab = (COLOR_TAB *)ctab_current(&v);
            assert(dst.ctab);
            for (i = 0; i < src.width * (sh + 2); i++) src.addr[i] = (i * 31 + 17) % (indexed ? 17 : 256);
            memset(screen, 0xa7, bytes); memset(shadow, 0xa7, bytes); memset(expected, 0xa7, bytes);
            reference(&src, sr, &dst, dr, indices, 4, dither);
            v.clip.on = clip;
            v.clip.rectangle.x1 = 7; v.clip.rectangle.y1 = 5;
            v.clip.rectangle.x2 = 49; v.clip.rectangle.y2 = 29;
            for (y = 0; y < 48; y++)
                for (x = 0; x < 64; x++)
                    if (x <= dr.x2 && y <= dr.y2 &&
                        (!clip || (x >= 7 && x <= 49 && y >= 5 && y <= 29)))
                        planar_store(expected, wk.screen.wrap, planes, x, y, indices[(y + 2) * dw + x + 3]);
            raster_pool(blocks ? 128 : 32, 0);
            assert(transfer_index8(&v, &src, NULL, &sr, &dr, dither ? 128 : 0) == 1);
            assert(memcmp(screen, expected, bytes) == 0 && memcmp(shadow, expected, bytes) == 0);
            /* Cached inverse tables survive but row scratch must not. */
            if (dither)
            {
                fail_calloc = 1;
                assert(transfer_index8(&v, &src, NULL, &sr, &dr, 128) == 1);
                assert(!fail_calloc && memcmp(screen, expected, bytes) == 0);
            }
            fail_malloc = 1;
            assert(transfer_index8(&v, &src, NULL, &sr, &dr, dither ? 128 : 0) == 1);
            assert(fail_malloc == ((((blocks ? 128 : 32) - dw) & ~1) >= 2 * planes));
            fail_malloc = 0;
            assert(memcmp(screen, expected, bytes) == 0);
            raster_pool(32, 1);
            assert(transfer_index8(&v, &src, NULL, &sr, &dr, 0) == 1);
            assert(memcmp(screen, expected, bytes) == 0);
            cases++;
        }
        ctab_close(&v);
        assert(allocations == baseline);
    }
    raster_pool(128, 0);
    free(s); free(screen); free(shadow); free(expected); free(indices);
    return cases;
}

static void colour_tables(void)
{
    Virtual v, other;
    Workstation wk;
    Driver driver;
    Device device;
    Colour colors[256], local[256];
    COLOR_TAB defaults, *created;
    const COLOR_TAB *current;
    static const int depths[] = { 1, 2, 4, 8, 15, 16, 24, 32 };
    long baseline = allocations, id;
    int i;
    memset(&v, 0, sizeof(v)); memset(&other, 0, sizeof(other));
    memset(&wk, 0, sizeof(wk)); memset(&driver, 0, sizeof(driver));
    memset(&device, 0, sizeof(device)); memset(colors, 0, sizeof(colors));
    memset(local, 0, sizeof(local));
    v.real_address = other.real_address = &wk; wk.driver = &driver; driver.device = &device;
    wk.screen.palette.colours = colors; wk.screen.palette.size = 4;
    colors[0].vdi.red = 1000; colors[1].vdi.green = 500; colors[2].vdi.blue = 1000;
    fail_malloc = 1;
    assert(!ctab_current(&v) && !fail_malloc);
    current = ctab_current(&v);
    assert(current && current->no_colors == 4 && current->length == ctab_bytes(4));
    assert(current->colors[0].rgb.red == 65535 && current->colors[1].rgb.green == 32768);
    id = current->map_id;
    assert(ctab_current(&v)->map_id == id);
    colors[0].vdi.red = 999;
    assert(ctab_current(&v)->map_id != id);
    id = current->map_id;
    assert(ctab_current(&other)->map_id != id);
    assert(!ctab_delete(&v, current));
    v.palette = local; local[0].vdi.blue = 1000;
    assert(ctab_current(&v)->colors[0].rgb.blue == 65535);
    device.clut = 1;
    assert(ctab_current(&v)->colors[0].rgb.blue == 0); /* CLUTs are shared, not local. */
    wk.screen.palette.size = 256; fail_malloc = 1;
    assert(!ctab_current(&v) && !fail_malloc);
    assert(ctab_current(&v)->no_colors == 256);
    wk.screen.palette.size = 2;
    assert(ctab_current(&v)->no_colors == 2);
    for (i = 0; i < 8; i++)
    {
        long count = depths[i] <= 8 ? 1L << depths[i] : 256;
        assert(ctab_default(&defaults, depths[i]));
        assert(defaults.no_colors == count && defaults.length == ctab_bytes(count));
        assert(defaults.colors[0].rgb.red == 65535 && defaults.colors[0].rgb.green == 65535);
        if (depths[i] <= 8)
            assert(defaults.colors[count - 1].rgb.red == 0 && defaults.colors[count - 1].rgb.green == 0);
        created = ctab_create(&v, 1, depths[i]);
        assert(created && created->map_id != defaults.map_id);
        assert(memcmp(created->colors, defaults.colors, count * sizeof(COLOR_ENTRY)) == 0);
        assert(!ctab_delete(&other, created));
        assert(ctab_delete(&v, created) && !ctab_delete(&v, created));
    }
    /* Promotion of the tagged, negative-only true-colour palette must free
     * its allocation base, preserve negative entries, and be atomic on OOM.
     */
    {
        Colour *negative = malloc(9 * sizeof(Colour));
        RGB rgb = { 1000, 500, 0 };
        memset(negative, 0x57, 9 * sizeof(Colour));
        v.palette = (Colour *)((unsigned long)(negative + 9) | 1);
        device.clut = 0; palette_writes = 0;
        fail_malloc = 1;
        lib_vs_color(&v, 0, &rgb);
        assert(!fail_malloc && !palette_writes && v.palette == (Colour *)((unsigned long)(negative + 9) | 1));
        lib_vs_color(&v, 0, &rgb);
        assert(palette_writes == 1 && !((unsigned long)v.palette & 1));
        for (i = 0; i < (int)(9 * sizeof(Colour)); i++)
            assert(((unsigned char *)(v.palette - 9))[i] == 0x57);
        assert(memcmp(v.palette, colors, wk.screen.palette.size * sizeof(Colour)) == 0);
        lib_vs_color(&v, -1, &rgb);
        assert(palette_writes == 1);
        free(v.palette - 9); v.palette = NULL;
        fail_malloc = 1;
        lib_vs_color(&v, 0, &rgb);
        assert(!v.palette && palette_writes == 1 && !fail_malloc);
    }
    fail_malloc = 1;
    assert(!ctab_create(&v, 1, 8) && !fail_malloc);
    assert(!ctab_create(&v, 2, 8) && !ctab_create(&v, 1, 3));
    assert(ctab_create(&v, 0, 8));
    ctab_close(&v); ctab_close(&other);
    assert(allocations == baseline);
}

static int remap_tests(Virtual *v)
{
    COLOR_TAB sp, dp;
    static const int sizes[] = { 1, 2, 7, 19 };
    int variant, bits, a, b, dither, i, cases = 0;
    void *id;
    unsigned char s[32], d[32], saved[32];
    GCBITMAP src = bitmap(s + 1, 4, 4, 1), dst = bitmap(d + 1, 4, 4, 1);
    RECT16 sr = { 0, 0, 3, 3 }, dr = { 0, 0, 0, 0 };

    for (variant = 0; variant < 7; variant++)
    {
        palette(&dp, variant == 6 ? 256 : 17);
        sp = dp;
        if (variant == 1 || variant == 2)
        {
            for (i = 0; i < 17; i++) sp.colors[i] = dp.colors[16 - i];
            if (variant == 2) sp.no_colors = 7;
        }
        if (variant == 3) /* Changed palette with both exact and approximate matches. */
            for (i = 1; i < 17; i += 2) sp.colors[i].rgb.green ^= 0xffff;
        if (variant == 4) sp.colors[0].rgb.red ^= 1; /* RGB16, not RGB8 equality. */
        if (variant == 5) /* Duplicate indices must survive the identity bypass. */
            for (i = 1; i < 17; i += 2) sp.colors[i] = dp.colors[i] = dp.colors[0];
        if (variant == 5) sp.colors[0].rgb.reserved = 0x1234;
        for (bits = 3; bits <= 5; bits++)
        {
            id = itab_create(v, &dp, bits);
            assert(id);
            for (a = 0; a < 4; a++)
                for (b = 0; b < 4; b++)
                    for (dither = 0; dither < 2; dither++)
                    {
                        check_image(v, &dp, id, bits, sizes[a], sizes[b],
                                    sizes[(a + b) % 4], sizes[(a + 2 * b) % 4], dither, &sp);
                        cases++;
                    }
            assert(itab_delete(v, id));
        }
    }
    palette(&dp, 4); sp = dp;
    id = itab_create(v, &dp, 4);
    src.ctab = &sp; dst.ctab = &dp; dst.itab = id;
    memset(s, 0, sizeof(s)); memset(d, 0xa7, sizeof(d)); memcpy(saved, d, sizeof(d));
    s[16] = 4; /* Invalid, unsampled last source pixel must refuse before writing. */
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 128) == 1);
    assert(memcmp(d, saved, sizeof(d)) == 0);
    s[16] = 0;
    src.ctab = NULL;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 0) == 1);
    assert(d[1] == nearest(&dp, 4, 0xffffffUL));
    memset(d, 0xa7, sizeof(d));
    src.ctab = &sp; sp.length = 0;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 0) == 1);
    sp = dp; dst.itab = NULL;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 0) == 1);
    dst.itab = id; dp.colors[0].rgb.red ^= 1;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 0) == 1);
    dp.colors[0].rgb.red ^= 1;
    assert(memcmp(d, saved, sizeof(d)) == 0);
    /* An exact palette bypasses allocation even when flag 128 was requested. */
    fail_calloc = 1;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 128) == 1);
    assert(fail_calloc == 1 && d[1] == 0);
    fail_calloc = 0;
    /* A source palette edit is seen immediately; no persistent remap cache. */
    sp.colors[0] = dp.colors[3];
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 0) == 1 && d[1] == 3);
    memset(d, 0xa7, sizeof(d));
    sp.colors[0].rgb.red ^= 1;
    fail_calloc = 1;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 128) == 1);
    assert(!fail_calloc && memcmp(d, saved, sizeof(d)) == 0);
    /* Invalid bytes outside the source rectangle (including padding) are ignored. */
    sp = dp; memset(s, 255, sizeof(s)); s[1] = 2;
    sr.x2 = sr.y2 = 0;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 0) == 1 && d[1] == 2);
    assert(itab_delete(v, id));
    /* Keep the existing no-palette raw byte-copy use case, without scaling. */
    src.ctab = dst.ctab = NULL; dst.itab = NULL;
    sr.x2 = dr.x2 = sr.y2 = dr.y2 = 3;
    memset(d, 0xa7, sizeof(d));
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 32) == 1);
    assert(memcmp(s + 1, d + 1, 16) == 0 && d[0] == 0xa7 && d[17] == 0xa7);
    memset(d, 0xa7, sizeof(d)); dr.x2 = 2;
    assert(transfer_index8(v, &src, &dst, &sr, &dr, 0) == 1);
    for (a = 0; a < 4; a++)
        for (b = 0; b < 3; b++) saved[1 + a * 4 + b] = s[1 + a * 4 + b * 4 / 3];
    assert(memcmp(d, saved, sizeof(d)) == 0);
    return cases;
}

void indexed_tests(void)
{
    Workstation wk;
    Virtual v, other;
    COLOR_TAB pal;
    static const int sizes[] = { 1, 2, 7, 19 };
    long baseline = allocations;
    int bits, a, b, c, dither, cases = 0;
    void *id, *old, *second;
    const InverseTable *t;
    unsigned char s[64], d[64], original[64];
    GCBITMAP src = bitmap(s, 4, 4, 4), dst = bitmap(d, 4, 4, 1);
    RECT16 rect = { 0, 0, 3, 3 };

    memset(&wk, 0, sizeof(wk)); memset(&v, 0, sizeof(v)); memset(&other, 0, sizeof(other));
    v.real_address = other.real_address = &wk;
    v.clip.on = 1; /* Screen clipping must not affect memory bitmaps. */
    palette(&pal, 17);
    for (bits = 3; bits <= 5; bits++)
    {
        unsigned long cell, cells = 1UL << (bits * 3), mask = (1UL << bits) - 1;
        id = itab_create(&v, &pal, bits);
        assert(id && allocations == baseline + 1);
        assert(last_malloc == sizeof(InverseTable) + 17 * sizeof(COLOR_ENTRY) + cells);
        t = itab_find(&v, id, &pal);
        assert(t && t->bits == bits && t->count == 17);
        for (cell = 0; cell < cells; cell++)
        {
            unsigned long rgb = ((cell >> (2 * bits)) << (24 - bits)) |
                                (((cell >> bits) & mask) << (16 - bits)) |
                                ((cell & mask) << (8 - bits));
            assert(ITAB_PIXELS(t)[cell] == nearest(&pal, bits, rgb));
        }
        for (a = 0; a < 4; a++)
            for (b = 0; b < 4; b++)
                for (c = 0; c < 4; c++)
                    for (dither = 0; dither < 2; dither++)
                    {
                        check_image(&v, &pal, id, bits, sizes[a], sizes[b], sizes[c],
                                    sizes[(a + b + c) % 4], dither, NULL);
                        cases++;
                    }
        assert(itab_delete(&other, id) == 0);
        assert(itab_find(&other, id, &pal) == NULL);
        assert(itab_delete(&v, id) == 1);
        assert(itab_delete(&v, id) == 0 && allocations == baseline);
    }
    /* A 256-entry table exercises index 255 and the largest map allocation. */
    palette(&pal, 256);
    memset(pal.colors, 0, sizeof(pal.colors));
    pal.colors[255].rgb.red = pal.colors[255].rgb.green = pal.colors[255].rgb.blue = 65535;
    id = itab_create(&v, &pal, 5);
    assert(id);
    t = itab_find(&v, id, &pal);
    assert(ITAB_PIXELS(t)[0] == 0 && ITAB_PIXELS(t)[32767] == 255);
    check_image(&v, &pal, id, 5, 31, 17, 53, 29, 1, NULL);
    itab_delete(&v, id);
    /* One-colour saturation and a wide row stress bounded error accumulation. */
    palette(&pal, 1);
    id = itab_create(&v, &pal, 3);
    check_image(&v, &pal, id, 3, 7, 2, 4096, 7, 1, NULL);
    itab_delete(&v, id);
    palette(&pal, 2);
    memset(pal.colors, 0, 2 * sizeof(COLOR_ENTRY));
    pal.colors[1].rgb.red = pal.colors[1].rgb.green = pal.colors[1].rgb.blue = 65535;
    id = itab_create(&v, &pal, 4);
    dst.ctab = &pal; dst.itab = id;
    memset(s, 128, sizeof(s)); memset(d, 0xa7, sizeof(d)); memcpy(original, d, sizeof(d));
    fail_calloc = 1;
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 128) == 1);
    assert(!fail_calloc && memcmp(d, original, sizeof(d)) == 0);
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 128) == 1);
    for (a = 0, b = 0; a < 16; a++) { assert(d[a] <= 1); b += d[a]; }
    assert(b > 3 && b < 13); /* Not an all-black/all-white non-dither result. */
    memset(d, 0xa7, sizeof(d));
    pal.colors[0].rgb.red = 1; /* Even a low-bit-only palette edit invalidates the snapshot. */
    assert(!itab_find(&v, id, &pal));
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 0) == 1);
    pal.colors[0].rgb.red = 0;
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 33) == 1);
    dst.itab = (void *)0xbadc0de1UL;
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 0) == 1);
    dst.itab = id; dst.ctab = NULL;
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 0) == 1);
    dst.ctab = &pal; dst.width = 3;
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 0) == 1);
    dst.width = 4; rect.x2 = 4;
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 0) == 1);
    rect.x2 = 3; dst.addr = s + 2;
    assert(transfer_index8(&v, &src, &dst, &rect, &rect, 0) == 1);
    dst.addr = d;
    assert(memcmp(d, original, sizeof(d)) == 0);
    old = id;
    assert(itab_delete(&v, id) == 1);
    fail_malloc = 1;
    assert(itab_create(&v, &pal, 4) == NULL && !fail_malloc);
    assert(itab_create(&v, &pal, 2) == NULL);
    assert(itab_create(&v, &pal, 6) == NULL);
    pal.length--;
    assert(itab_create(&v, &pal, 4) == NULL);
    pal.length++; pal.no_colors = 257;
    assert(itab_create(&v, &pal, 4) == NULL);
    pal.no_colors = 2; pal.color_space = 2;
    assert(itab_create(&v, &pal, 4) == NULL);
    pal.color_space = 1;
    id = itab_create(&v, &pal, 3);
    second = itab_create(&other, &pal, 3);
    assert(id && id != old && second && second != id);
    assert(itab_create(&v, &pal, 3));
    itab_close(&v);
    assert(!itab_find(&v, id, &pal) && !itab_delete(&v, old));
    assert(itab_find(&other, second, &pal));
    itab_close(&other);
    cases += remap_tests(&v);
    ctab_close(&v);
    colour_tables();
    cases += indexed_screens();
    assert(allocations == baseline);
    printf("%d indexed image comparisons, all 3/4/5-bit lookup cells, lifecycle and allocation-failure checks passed\n", cases + 2);
}
