/* Independent MFDB scaling, Boolean-operation and expansion references. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fvdi.h"
#include "function.h"
#include "test_support.h"

int legacy_test_active;
void raster_pool(long, int);

static uint32_t get(const MFDB *b, int packed, int x, int y)
{
    const unsigned char *data = (const unsigned char *)b->address;
    uint32_t value = 0;
    int p, n = b->bitplanes;
    if (packed && n > 1 && !b->standard)
    {
        long offset = ((long)y * b->wdwidth * 16 + x) * (n / 8);
        for (p = 0; p < n / 8; p++) value = value * 256 + data[offset + p];
    } else
        for (p = 0; p < n; p++)
        {
            long word = b->standard ? ((long)p * b->height + y) * b->wdwidth + x / 16 :
                        ((long)y * b->wdwidth + x / 16) * n + p;
            if (data[word * 2 + (x % 16) / 8] & (128 >> (x % 8))) value |= UINT32_C(1) << p;
        }
    return value;
}

static void put(MFDB *b, int packed, int x, int y, uint32_t value)
{
    unsigned char *data = (unsigned char *)b->address;
    int p, n = b->bitplanes;
    if (packed && n > 1 && !b->standard)
    {
        long offset = ((long)y * b->wdwidth * 16 + x) * (n / 8);
        for (p = n / 8; p--; ) { data[offset + p] = value; value /= 256; }
    } else
        for (p = 0; p < n; p++)
        {
            long word = b->standard ? ((long)p * b->height + y) * b->wdwidth + x / 16 :
                        ((long)y * b->wdwidth + x / 16) * n + p;
            long byte = word * 2 + (x % 16) / 8;
            unsigned char mask = 128 >> (x % 8);
            data[byte] = (data[byte] & ~mask) | ((value & (UINT32_C(1) << p)) ? mask : 0);
        }
}

static uint32_t combine(int op, uint32_t s, uint32_t d, int expand, int n)
{
    uint32_t mask = n == 32 ? UINT32_MAX : (UINT32_C(1) << n) - 1;
    uint32_t fg = UINT32_C(0xaaaaaaaa) & mask, bg = UINT32_C(0x55555555) & mask;
    if (expand)
    {
        if (op == 1) return s ? fg : bg;
        if (op == 2) return s ? fg : d;
        if (op == 3) return s ? (d ^ mask) : d;
        return s ? d : bg;
    }
    return (((op & 8) ? (~s & ~d) : 0) | ((op & 4) ? (~s & d) : 0) |
            ((op & 2) ? (s & ~d) : 0) | ((op & 1) ? (s & d) : 0)) & mask;
}

void legacy_test_blit(void *func, Virtual *v, long mode, void *points, void *source, void *dest, void *pens)
{
    const short *xy = points;
    const MFDB *src = source;
    MFDB *dst = dest ? dest : &v->real_address->screen.mfdb;
    int x, packed = v->real_address->driver->device->format == 2;
    int expand = func == (void *)lib_vrt_cpyfm;
    assert(expand || func == (void *)lib_vro_cpyfm);
    assert(src->height == 1 && src->standard == 0 && xy[0] == 0 && xy[1] == 0 && xy[3] == 0);
    assert(xy[2] + 1 == src->width && xy[6] - xy[4] == xy[2] && xy[5] == xy[7]);
    if (expand) assert(pens && ((short *)pens)[0] == 2 && ((short *)pens)[1] == 3);
    for (x = 0; x < src->width; x++)
        put(dst, packed, x + xy[4], xy[5], combine(mode, get(src, packed, x, 0),
            get(dst, packed, x + xy[4], xy[5]), expand, dst->bitplanes));
}

static void legacy_refusals(void)
{
    unsigned short s[32], d[128], saved[128];
    Virtual v;
    Workstation wk;
    Driver driver;
    Device device;
    int test;
    memset(&v, 0, sizeof(v)); memset(&wk, 0, sizeof(wk));
    memset(&driver, 0, sizeof(driver)); memset(&device, 0, sizeof(device));
    memset(s, 0x57, sizeof(s)); memset(d, 0xa7, sizeof(d)); memcpy(saved, d, sizeof(d));
    v.real_address = &wk; wk.driver = &driver; driver.device = &device;
    for (test = 0; test < 16; test++)
    {
        MFDB src, dst;
        const MFDB *sp = &src, *dp = &dst;
        short xy[8] = { 0, 0, 15, 3, 0, 0, 31, 7 }, operation = 0x8003U;
        const short *points = xy;
        unsigned char args[2 + 4 * sizeof(void *)];
        memset(&src, 0, sizeof(src)); memset(&dst, 0, sizeof(dst));
        src.address = (short *)s; src.width = 16; src.height = 4; src.wdwidth = 1; src.bitplanes = 4;
        dst.address = (short *)d; dst.width = 32; dst.height = 8; dst.wdwidth = 2; dst.bitplanes = 4;
        wk.screen.mfdb = dst; wk.screen.mfdb.address = NULL; wk.screen.wrap = 16;
        raster_pool(128, test == 12);
        switch (test)
        {
        case 0: operation = 0x8010U; break;
        case 1: sp = NULL; break;
        case 2: src.address = NULL; break;
        case 3: src.width = 0; break;
        case 4: src.wdwidth = 0; break;
        case 5: src.standard = 2; break;
        case 6: dst.standard = 1; break;
        case 7: xy[0] = -1; break;
        case 8: xy[2] = src.width; break;
        case 9: xy[6] = dst.width; break;
        case 10: dst.address = src.address; break;
        case 11: dp = NULL; wk.screen.mfdb.address = src.address; break;
        case 13: src.bitplanes = 2; break;
        case 14: points = NULL; break;
        case 15:
            dp = NULL; wk.screen.mfdb.address = (short *)d;
            wk.screen.mfdb.height = 32767; wk.screen.wrap = -1;
            break;
        }
        memcpy(args, &operation, 2); memcpy(args + 2, &points, sizeof(points));
        memcpy(args + 2 + sizeof(points), &sp, sizeof(sp));
        memcpy(args + 2 + sizeof(points) + sizeof(sp), &dp, sizeof(dp));
        legacy_vro_scale(&v, args);
        assert(memcmp(d, saved, sizeof(d)) == 0);
    }
    raster_pool(128, 0);
}

void legacy_tests(void)
{
    static const int depths[] = { 1, 2, 4, 8, 8, 16, 24, 32 };
    unsigned char *s = malloc(48UL * 37 * 4), *saved = malloc(48UL * 37 * 4),
                  *d = malloc(64UL * 48 * 4), *expected = malloc(64UL * 48 * 4);
    Virtual v;
    Workstation wk;
    Driver driver;
    Device device;
    int f, expand, mode, standard, screen, shrink, blocks, x, y, cases = 0;
    assert(s && saved && d && expected);
    memset(&v, 0, sizeof(v)); memset(&wk, 0, sizeof(wk));
    memset(&driver, 0, sizeof(driver)); memset(&device, 0, sizeof(device));
    v.real_address = &wk; wk.driver = &driver; driver.device = &device;
    legacy_test_active = 1;
    for (f = 0; f < 8; f++)
    for (expand = 0; expand < 2; expand++)
    for (mode = expand ? 1 : 0; mode <= (expand ? 4 : 15); mode++)
    for (standard = 0; standard < 2; standard++)
    for (screen = 0; screen < 2; screen++)
    for (shrink = 0; shrink < 2; shrink++)
    for (blocks = 0; blocks < 2; blocks++)
    {
        MFDB src, dst, ref;
        short xy[8] = { 2, 1, 42, 35, 1, 2, 21, 19 }, pens[2] = { 2, 3 };
        unsigned char arguments[2 + 4 * sizeof(void *)];
        const short *points = xy, *colors = pens;
        const MFDB *sp = &src, *dp;
        short operation = 0x8000U | mode;
        int n = depths[f], packed = f >= 4;
        long sn, dn, i;
        memset(&src, 0, sizeof(src)); memset(&dst, 0, sizeof(dst));
        src.address = (short *)s; src.width = 43; src.height = 37; src.wdwidth = 3;
        src.bitplanes = expand ? 1 : n; src.standard = standard;
        dst.address = (short *)d; dst.width = 64; dst.height = 48; dst.wdwidth = 4; dst.bitplanes = n;
        wk.screen.mfdb = dst; wk.screen.wrap = 8 * n;
        if (!screen) wk.screen.mfdb.address = NULL;
        dp = screen ? NULL : &dst;
        device.format = packed ? 2 : 0;
        v.clip.on = 1; v.clip.rectangle.x1 = 5; v.clip.rectangle.y1 = 4;
        v.clip.rectangle.x2 = 43; v.clip.rectangle.y2 = 29;
        if (!shrink) { xy[2] = 22; xy[3] = 17; xy[6] = 59; xy[7] = 42; }
        if (screen) { xy[4] -= 4; xy[6] -= 4; xy[5] -= 4; xy[7] -= 4; }
        sn = 6L * src.bitplanes * 37; dn = 8L * n * 48;
        for (i = 0; i < sn; i++) s[i] = (i * 37 + i / 7 + 11) & 255;
        memcpy(saved, s, sn); memset(d, 0xa7, dn); memset(expected, 0xa7, dn);
        ref = dst; ref.address = (short *)expected;
        for (y = xy[5]; y <= xy[7]; y++)
            for (x = xy[4]; x <= xy[6]; x++)
            {
                int sx, sy;
                if (screen && (x < 5 || x > 43 || y < 4 || y > 29)) continue;
                sx = xy[0] + (int64_t)(x - xy[4]) * (xy[2] - xy[0] + 1) / (xy[6] - xy[4] + 1);
                sy = xy[1] + (int64_t)(y - xy[5]) * (xy[3] - xy[1] + 1) / (xy[7] - xy[5] + 1);
                put(&ref, packed, x, y, combine(mode, get(&src, packed, sx, sy), get(&ref, packed, x, y), expand, n));
            }
        memcpy(arguments, &operation, 2); memcpy(arguments + 2, &points, sizeof(points));
        memcpy(arguments + 2 + sizeof(points), &sp, sizeof(sp));
        memcpy(arguments + 2 + sizeof(points) + sizeof(sp), &dp, sizeof(dp));
        memcpy(arguments + 2 + sizeof(points) + sizeof(sp) + sizeof(dp), &colors, sizeof(colors));
        raster_pool(blocks ? 128 : 32, 0);
        if (!expand && n > 16 && !blocks)
            memset(expected, 0xa7, dn); /* Block too short for one aligned MFDB row. */
        if (expand) legacy_vrt_scale(&v, arguments); else legacy_vro_scale(&v, arguments);
        if (memcmp(s, saved, sn) || memcmp(d, expected, dn))
        {
            fprintf(stderr, "Legacy mismatch f=%d expand=%d mode=%d standard=%d screen=%d shrink=%d blocks=%d\n",
                    f, expand, mode, standard, screen, shrink, blocks);
            abort();
        }
        cases++;
    }
    legacy_test_active = 0;
    raster_pool(128, 0);
    legacy_refusals();
    free(s); free(saved); free(d); free(expected);
    printf("%d legacy MFDB scaling/Boolean/expansion comparisons passed\n", cases);
}
