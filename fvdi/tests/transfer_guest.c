/* Real VDI trap smoke test and fused-versus-two-pass enlargement benchmark.
 * Build with m68k-atari-mint-gcc -m68020 -O2 transfer_guest.c -lgem.
 * Run after fVDI in AUTO, or with --aes under AES, on a disposable boot.
 * Output goes to BIOS 1. Screen contents are intentionally overwritten.
 */
#include <gem.h>
#include <gemx.h>
#include <mint/osbind.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RASTER_BENCH_ONLY
#define RASTER_BENCH_ONLY 0
#endif

static short aes_active, update_locked, test_handle;

static void finish(void)
{
    if (test_handle)
    {
        if (aes_active)
        {
            v_show_c(test_handle, 1);
            v_clsvwk(test_handle);
        } else
            v_clswk(test_handle);
        test_handle = 0;
    }
    if (update_locked) wind_update(END_UPDATE);
    if (aes_active) appl_exit();
    update_locked = aes_active = 0;
}

static void report(const char *message)
{
    while (*message) Bconout(1, *message++);
}

static void check(int condition, const char *message)
{
    if (!condition)
    {
        report("\r\nFVDI TRANSFER FAIL: ");
        report(message);
        report("\r\n");
        exit(1);
    }
}

static GCBITMAP bitmap(void *p, long w, long h, short bits, unsigned long format)
{
    GCBITMAP b;
    memset(&b, 0, sizeof(b));
    b.magic = 0x6362746dL;
    b.length = sizeof(b);
    b.addr = p;
    b.width = w * (bits / 8);
    b.bits = bits;
    b.px_format = format;
    b.xmax = w;
    b.ymax = h;
    return b;
}

static long clock_ticks(void)
{
    return *(volatile long *)0x4ba;
}

/* Conventional two-pass baseline, using rational stepping too. Scratch is
 * allocated once outside timing. This is not NVDI or an old fVDI path.
 */
static void split_scale(const uint32_t *src, uint32_t *tmp, uint16_t *dst,
                        long sw, long sh, long dw, long dh)
{
    long y, x, sy = 0, ye = 0, ystep = sh / dh, yrem = sh % dh;
    long xstep = sw / dw, xrem = sw % dw;
    uint32_t *out = tmp;

    for (y = 0; y < dh; y++)
    {
        long sx = 0, xe = 0;
        const uint32_t *row = src + sy * sw;
        for (x = 0; x < dw; x++)
        {
            *out++ = row[sx];
            sx += xstep;
            xe += xrem;
            if (xe >= dw) { xe -= dw; sx++; }
        }
        sy += ystep;
        ye += yrem;
        if (ye >= dh) { ye -= dh; sy++; }
    }
    for (x = 0; x < dw * dh; x++)
    {
        uint32_t rgb = tmp[x];
        dst[x] = ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 31);
    }
}

static void benchmark(short handle, long sw, long sh, long dw, long dh, int screen_check)
{
    uint32_t *s = malloc(sw * sh * 4), *tmp = malloc(dw * dh * 4);
    uint16_t *d = malloc(dw * dh * 2), *expected = malloc(dw * dh * 2);
    GCBITMAP src = bitmap(s, sw, sh, 32, PX_PREF32);
    GCBITMAP dst = bitmap(d, dw, dh, 16, PX_MATRIX16);
    short sr[4] = { 0, 0, sw - 1, sh - 1 }, dr[4] = { 0, 0, dw - 1, dh - 1 };
    short xy[8] = { 0, 0, dw - 1, dh - 1, 0, 0, dw - 1, dh - 1 };
    MFDB screen, readback;
    long i, round, begin, fused, split;
    char message[160];
    long iterations = RASTER_BENCH_ONLY ? 8 : 128;

    check(s && tmp && d && expected, "benchmark allocation");
    for (i = 0; i < sw * sh; i++)
    {
        s[i] = i * 937UL;
        /* Use bit-replicated RGB5 green for the cross-VDI comparison.
         * The tested NVDI 5.03 path quantizes green through five bits.
         * The standalone cross-VDI benchmark uses shared green precision;
         * normal fVDI regression/benchmark runs retain full-range input.
         */
        if (RASTER_BENCH_ONLY)
        {
            unsigned long green = (s[i] >> 11) & 31;
            s[i] = (s[i] & ~0x0000ff00UL) | ((green << 3 | green >> 2) << 8);
        }
    }
    split_scale(s, tmp, expected, sw, sh, dw, dh);
    memset(d, 0xa7, dw * dh * 2);
    vr_transfer_bits(handle, &src, &dst, sr, dr, 0);
    if (memcmp(d, expected, dw * dh * 2) != 0)
    {
        short info[57];
        unsigned long format;
        long space = vq_px_format(handle, &format);
        vq_extnd(handle, 1, info);
        sprintf(message, "\r\nBENCH DIAGNOSTIC depth=%d caps=%04x space=%ld format=%08lx src=%08lx dst=%08lx size=%lu\r\n",
                info[4], (unsigned short)info[30], space, format, src.px_format, dst.px_format, (unsigned long)sizeof(src));
        report(message);
        for (i = 0; i < dw * dh && d[i] == expected[i]; i++)
            ;
        for (round = 0; round < 8 && i + round < dw * dh; round++)
        {
            long at = i + round;
            sprintf(message, "pixel %ld actual=%04x expected=%04x source=%08lx\r\n",
                    at, d[at], expected[at], (unsigned long)s[at % (sw * sh)]);
            report(message);
        }
    }
    check(memcmp(d, expected, dw * dh * 2) == 0, "enlargement pixels");
    /* Exercise multiple staging strips through the real screen driver. */
    memset(&screen, 0, sizeof(screen));
    memset(&readback, 0, sizeof(readback));
    readback.fd_addr = d;
    readback.fd_w = dw;
    readback.fd_h = dh;
    readback.fd_wdwidth = dw / 16;
    readback.fd_nplanes = 16;
    check(dw % 16 == 0, "benchmark readback stride");
    if (screen_check)
    {
        vr_transfer_bits(handle, &src, NULL, sr, dr, 0);
        memset(d, 0, dw * dh * 2);
        vro_cpyfm(handle, 3, xy, &screen, &readback);
        check(memcmp(d, expected, dw * dh * 2) == 0, "multi-strip screen pixels");
    }
    for (round = 0; round < 3; round++)
    {
        begin = Supexec(clock_ticks);
        for (i = 0; i < iterations; i++) vr_transfer_bits(handle, &src, &dst, sr, dr, 0);
        fused = Supexec(clock_ticks) - begin;
        begin = Supexec(clock_ticks);
        for (i = 0; i < iterations; i++) split_scale(s, tmp, expected, sw, sh, dw, dh);
        split = Supexec(clock_ticks) - begin;
        check(memcmp(d, expected, dw * dh * 2) == 0, "benchmark pixels");
        sprintf(message, "\r\nTRANSFER BENCH %ldx%ld -> %ldx%ld round=%ld fused=%ld split=%ld ticks (%ld calls)\r\n",
                sw, sh, dw, dh, round, fused, split, iterations);
        report(message);
    }
    if (screen_check)
    {
        begin = Supexec(clock_ticks);
        for (i = 0; i < 128; i++) vr_transfer_bits(handle, &src, NULL, sr, dr, 0);
        fused = Supexec(clock_ticks) - begin;
        begin = Supexec(clock_ticks);
        for (i = 0; i < 128; i++)
        {
            split_scale(s, tmp, d, sw, sh, dw, dh);
            vro_cpyfm(handle, 3, xy, &readback, &screen);
        }
        split = Supexec(clock_ticks) - begin;
        memset(d, 0xa7, dw * dh * 2);
        vro_cpyfm(handle, 3, xy, &screen, &readback);
        check(memcmp(d, expected, dw * dh * 2) == 0, "screen benchmark readback");
        sprintf(message, "\r\nSCREEN BENCH %ldx%ld -> %ldx%ld fused=%ld split+blit=%ld ticks (128 calls)\r\n",
                sw, sh, dw, dh, fused, split);
        report(message);
    }
    free(s); free(tmp); free(d); free(expected);
}

static void indexed_palette(COLOR_TAB256 *pal, int count)
{
    memset(pal, 0, sizeof(*pal));
    pal->magic = COLOR_TAB_MAGIC;
    pal->length = sizeof(*pal);
    pal->color_space = CSPACE_RGB;
    pal->no_colors = count;
}

/* Deliberately uncached reference for the 4-bit inverse-map benchmark. */
static unsigned char palette_search(const COLOR_TAB256 *pal, uint32_t rgb)
{
    long r = ((rgb >> 16) & 240) + 8, g = ((rgb >> 8) & 240) + 8;
    long b = (rgb & 240) + 8, best = 0x7fffffffL;
    int i;
    unsigned char result = 0;
    for (i = 0; i < 256; i++)
    {
        long dr = r - (pal->colors[i].rgb.red >> 8);
        long dg = g - (pal->colors[i].rgb.green >> 8);
        long db = b - (pal->colors[i].rgb.blue >> 8);
        long distance = dr * dr + dg * dg + db * db;
        if (distance < best) { best = distance; result = i; }
    }
    return result;
}

static void indexed_benchmark(short handle)
{
    COLOR_TAB256 pal;
    uint32_t *s = malloc(160L * 100 * 4);
    unsigned char *d = malloc(320L * 200), *expected = malloc(320L * 200);
    GCBITMAP src = bitmap(s, 160, 100, 32, PX_PREF32);
    GCBITMAP dst = bitmap(d, 320, 200, 8, PX_PREF8);
    short sr[4] = { 0, 0, 159, 99 }, dr[4] = { 0, 0, 319, 199 };
    long begin, build, plain, dither, search, i, x, y;
    char message[200];

    check(s && d && expected, "indexed benchmark allocation");
    indexed_palette(&pal, 256);
    for (i = 0; i < 256; i++)
    {
        pal.colors[i].rgb.red = ((i >> 5) * 65535UL) / 7;
        pal.colors[i].rgb.green = (((i >> 2) & 7) * 65535UL) / 7;
        pal.colors[i].rgb.blue = ((i & 3) * 65535UL) / 3;
    }
    for (i = 0; i < 160L * 100; i++) s[i] = i * 937UL;
    dst.ctab = (COLOR_TAB *)&pal;
    begin = Supexec(clock_ticks);
    dst.itab = v_create_itab(handle, dst.ctab, 4);
    build = Supexec(clock_ticks) - begin;
    check(dst.itab != NULL, "benchmark inverse palette");
    begin = Supexec(clock_ticks);
    for (i = 0; i < 16; i++) vr_transfer_bits(handle, &src, &dst, sr, dr, 0);
    plain = Supexec(clock_ticks) - begin;
    begin = Supexec(clock_ticks);
    for (y = 0; y < 200; y++)
        for (x = 0; x < 320; x++)
            expected[y * 320 + x] = palette_search(&pal, s[(y / 2) * 160 + x / 2]);
    search = Supexec(clock_ticks) - begin;
    check(memcmp(d, expected, 320L * 200) == 0, "indexed lookup versus palette search");
    begin = Supexec(clock_ticks);
    for (i = 0; i < 16; i++) vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
    dither = Supexec(clock_ticks) - begin;
    sprintf(message, "\r\nINDEXED BENCH 160x100 -> 320x200 build=%ld (1) plain=%ld (16) dither=%ld (16) search=%ld (1) ticks\r\n",
            build, plain, dither, search);
    report(message);
    /* Equal-palette source: no inverse lookup or diffusion in the pixel loop. */
    src.bits = 8; src.px_format = PX_PREF8; src.width = 160;
    src.ctab = dst.ctab;
    for (i = 0; i < 160L * 100; i++) ((unsigned char *)s)[i] = i & 255;
    begin = Supexec(clock_ticks);
    for (i = 0; i < 128; i++) vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
    plain = Supexec(clock_ticks) - begin;
    for (y = 0; y < 200; y++)
        for (x = 0; x < 320; x++)
            check(d[y * 320 + x] == (((y / 2) * 160 + x / 2) & 255), "equal-palette benchmark pixels");
    sprintf(message, "\r\nREMAP BENCH equal palette 160x100 -> 320x200 =%ld ticks (128 calls, flag 128 bypassed)\r\n", plain);
    report(message);
    check(v_delete_itab(handle, dst.itab) == 1, "benchmark inverse delete");
    free(s); free(d); free(expected);
}

static void colour_table_test(short handle)
{
    COLOR_TAB256 table, saved, defaults;
    COLOR_TAB *created;
    COLOR_ENTRY entry;
    long id;
    int i;
    memset(&table, 0xa7, sizeof(table)); saved = table;
    check(vq_ctab(handle, 47, (COLOR_TAB *)&table) == 0, "short CTAB buffer refused");
    check(memcmp(&table, &saved, sizeof(table)) == 0, "short CTAB buffer untouched");
    check(vq_ctab(handle, sizeof(table), (COLOR_TAB *)&table) == 1, "current CTAB query");
    check(table.magic == 0x63746162L && table.no_colors == 256 && table.length == sizeof(table), "current CTAB header");
    id = vq_ctab_id(handle);
    check(id == table.map_id && id == vq_ctab_id(handle), "current CTAB stable ID");
    check(vq_ctab_entry(handle, 2, &entry) == 1 &&
          memcmp(&entry, &table.colors[2], sizeof(entry)) == 0, "CTAB entry query");
    entry.rgb.red = 12345; entry.rgb.green = 23456; entry.rgb.blue = 34567;
    check(vs_ctab_entry(handle, 2, 1, &entry) == 1, "CTAB entry set");
    check(v_ctab_idx2value(handle, 2) == (unsigned long)(((entry.rgb.red >> 11) << 11) |
          ((entry.rgb.green >> 10) << 5) | (entry.rgb.blue >> 11)), "CTAB RGB565 pixel value");
    check(vq_ctab_id(handle) != id, "CTAB ID changes after palette edit");
    check(vs_ctab_entry(handle, -1, 1, &entry) == 0, "negative CTAB entry refused");
    check(vs_ctab_entry(handle, 2, 2, &entry) == 0, "unsupported CTAB colour space refused");
    check(vs_ctab_entry(handle, 2, 1, &table.colors[2]) == 1, "restore CTAB entry");
    check(vq_dflt_ctab(handle, sizeof(defaults), (COLOR_TAB *)&defaults) == 1, "default CTAB query");
    check(vs_dflt_ctab(handle) == 256, "default CTAB setter count");
    check(vs_ctab(handle, (COLOR_TAB *)&table) == 256, "restore complete CTAB");
    for (i = 1; i <= 8; i *= 2)
    {
        created = v_create_ctab(handle, 1, i);
        check(created && created->no_colors == (1L << i), "created CTAB depth");
        check(created->colors[created->no_colors - 1].rgb.red == 0, "default pixel-ordered black");
        check(v_delete_ctab(handle, created) == 1, "created CTAB deletion");
        check(v_delete_ctab(handle, created) == 0, "double CTAB deletion refused");
    }
    report("\r\nFVDI CTAB GUEST PASS\r\n");
}

static void indexed_source_test(short handle)
{
    COLOR_TAB256 pal, source_pal;
    unsigned char s[4] = { 0, 1, 2, 3 }, d[16], expected[16];
    uint32_t rgb[4];
    GCBITMAP src = bitmap(s, 2, 2, 8, PX_PREF8);
    GCBITMAP dst = bitmap(d, 4, 4, 8, PX_PREF8);
    GCBITMAP direct = bitmap(rgb, 2, 2, 32, PX_PREF32), reference;
    short sr[4] = { 0, 0, 1, 1 }, dr[4] = { 0, 0, 3, 3 };
    int i, x, y;

    indexed_palette(&pal, 4);
    pal.colors[0].rgb.red = pal.colors[1].rgb.green = pal.colors[2].rgb.blue = 65535;
    pal.colors[3].rgb.red = pal.colors[3].rgb.green = pal.colors[3].rgb.blue = 65535;
    source_pal = pal;
    src.ctab = (COLOR_TAB *)&source_pal; dst.ctab = (COLOR_TAB *)&pal;
    dst.itab = v_create_itab(handle, dst.ctab, 4);
    check(dst.itab != NULL, "remap inverse table");
    vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            check(d[y * 4 + x] == (y / 2) * 2 + x / 2, "equal-palette index preservation");
    dr[2] = dr[3] = 0;
    vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
    check(d[0] == 0, "equal-palette reduction bypasses interpolation");
    for (i = 0; i < 4; i++) source_pal.colors[i] = pal.colors[3 - i];
    dr[2] = dr[3] = 3;
    vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            check(d[y * 4 + x] == 3 - (y / 2) * 2 - x / 2, "reordered-palette remap");
    source_pal.colors[0].rgb.red = source_pal.colors[0].rgb.green = source_pal.colors[0].rgb.blue = 32768;
    for (i = 0; i < 4; i++)
        rgb[i] = ((uint32_t)(source_pal.colors[i].rgb.red >> 8) << 16) |
                 ((uint32_t)(source_pal.colors[i].rgb.green >> 8) << 8) |
                 (source_pal.colors[i].rgb.blue >> 8);
    reference = dst; reference.addr = expected;
    for (i = 0; i < 2; i++)
    {
        memset(d, 0xa7, sizeof(d)); memset(expected, 0xa7, sizeof(expected));
        vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
        vr_transfer_bits(handle, &direct, &reference, sr, dr, 128);
        check(memcmp(d, expected, sizeof(d)) == 0, "indexed-source diffusion versus RGB32");
        dr[2] = dr[3] = 0;
    }
    check(v_delete_itab(handle, dst.itab) == 1, "remap inverse deletion");
    src.ctab = dst.ctab = NULL; dst.itab = NULL;
    dr[2] = dr[3] = 1;
    memset(d, 0xa7, sizeof(d));
    vr_transfer_bits(handle, &src, &dst, sr, dr, 0);
    check(d[0] == 0 && d[1] == 1 && d[4] == 2 && d[5] == 3 && d[2] == 0xa7,
          "legacy unpaletted byte copy");
    report("\r\nFVDI REMAP GUEST PASS\r\n");
}

static void indexed_test(short handle)
{
    COLOR_TAB256 pal;
    uint32_t s[4] = { 0x00ff0000UL, 0x0000ff00UL, 0x000000ffUL, 0x00ffffffUL };
    unsigned char d[16];
    GCBITMAP src = bitmap(s, 2, 2, 32, PX_PREF32);
    GCBITMAP dst = bitmap(d, 4, 4, 8, PX_PREF8);
    short sr[4] = { 0, 0, 1, 1 }, dr[4] = { 0, 0, 3, 3 };
    ITAB_REF id, old;
    int x, y, ones = 0;

    indexed_palette(&pal, 4);
    pal.colors[0].rgb.red = pal.colors[1].rgb.green = pal.colors[2].rgb.blue = 65535;
    pal.colors[3].rgb.red = pal.colors[3].rgb.green = pal.colors[3].rgb.blue = 65535;
    id = v_create_itab(handle, (COLOR_TAB *)&pal, 4);
    check(id && id != (void *)0xbadc0de1UL, "inverse palette creation");
    dst.ctab = (COLOR_TAB *)&pal; dst.itab = id;
    memset(d, 0xa7, sizeof(d));
    vr_transfer_bits(handle, &src, &dst, sr, dr, 0);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            check(d[y * 4 + x] == (y / 2) * 2 + x / 2, "indexed enlargement");
    dr[2] = dr[3] = 0;
    vr_transfer_bits(handle, &src, &dst, sr, dr, 32);
    check(d[0] == 3, "indexed interpolated reduction");
    memset(d, 0xa7, sizeof(d));
    pal.colors[0].rgb.red--;
    vr_transfer_bits(handle, &src, &dst, sr, dr, 0);
    check(d[0] == 0xa7, "stale palette refusal");
    check(v_delete_itab(handle, id) == 1, "inverse palette deletion");
    check(v_delete_itab(handle, id) == 0, "double inverse deletion refusal");
    old = id;
    indexed_palette(&pal, 2);
    pal.colors[1].rgb.red = pal.colors[1].rgb.green = pal.colors[1].rgb.blue = 65535;
    id = v_create_itab(handle, (COLOR_TAB *)&pal, 4);
    check(id && id != old, "inverse references are not reused");
    dst.itab = id; dr[2] = dr[3] = 3;
    for (x = 0; x < 4; x++) s[x] = 0x00808080UL;
    vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
    for (x = 0; x < 16; x++) { check(d[x] <= 1, "dither index bounds"); ones += d[x]; }
    check(ones > 3 && ones < 13, "error diffusion enabled");
    check(v_delete_itab(handle, id) == 1, "dither inverse deletion");
    indexed_source_test(handle);
    colour_table_test(handle);
    indexed_benchmark(handle);
    report("\r\nFVDI INDEXED GUEST PASS\r\n");
}

static void direct_source_test(short handle)
{
    unsigned char source[17 * 11 * 2], actual[31 * 19 * 2], expected[31 * 19 * 2];
    uint32_t expanded[17 * 11];
    COLOR_TAB256 pal;
    GCBITMAP src = bitmap(source, 17, 11, 16, PX_MATRIX16);
    GCBITMAP ref = bitmap(expanded, 17, 11, 32, PX_PREF32);
    GCBITMAP dst, want;
    short sr[4] = { 0, 0, 16, 10 }, dr[4];
    int format, target, size, mode, i;
    void *inverse;
    indexed_palette(&pal, 17);
    for (i = 0; i < 17; i++)
    {
        pal.colors[i].rgb.red = i * 3119UL;
        pal.colors[i].rgb.green = i * 7919UL;
        pal.colors[i].rgb.blue = i * 1777UL;
    }
    inverse = v_create_itab(handle, (COLOR_TAB *)&pal, 4);
    check(inverse != NULL, "direct-source inverse table");
    for (format = 0; format < 4; format++)
    {
        src.px_format = (format & 1 ? PX_PREF15 : PX_MATRIX16) | (format & 2 ? PX_REVERSED : 0);
        for (i = 0; i < 17 * 11; i++)
        {
            unsigned long pixel = (i * 937UL + 317) & 65535;
            unsigned long r = pixel >> (format & 1 ? 10 : 11) & 31;
            unsigned long g = pixel >> 5 & (format & 1 ? 31 : 63), b = pixel & 31;
            source[i * 2 + (format & 2 ? 1 : 0)] = pixel >> 8;
            source[i * 2 + (format & 2 ? 0 : 1)] = pixel;
            expanded[i] = (r * 8 + r / 4) * 65536UL +
                          (format & 1 ? g * 8 + g / 4 : g * 4 + g / 16) * 256UL + b * 8 + b / 4;
        }
        for (target = 0; target < 2; target++)
        for (size = 0; size < 3; size++)
        for (mode = 0; mode <= 128; mode += 128)
        {
            int w = size == 0 ? 17 : size == 1 ? 31 : 7;
            int h = size == 0 ? 11 : size == 1 ? 19 : 5;
            dst = bitmap(actual, w, h, target ? 8 : 16, target ? PX_PREF8 : PX_MATRIX16);
            dst.ctab = (COLOR_TAB *)&pal; dst.itab = inverse;
            want = dst; want.addr = expected;
            dr[0] = dr[1] = 0; dr[2] = w - 1; dr[3] = h - 1;
            memset(actual, 0xa7, sizeof(actual)); memset(expected, 0xa7, sizeof(expected));
            vr_transfer_bits(handle, &ref, &want, sr, dr, mode);
            vr_transfer_bits(handle, &src, &dst, sr, dr, mode);
            check(memcmp(actual, expected, sizeof(actual)) == 0, "RGB555/RGB565 source conversion/filter/dither");
        }
    }
    check(v_delete_itab(handle, inverse) == 1, "direct-source inverse deletion");
    report("\r\nFVDI DIRECT SOURCE GUEST PASS\r\n");
}

static unsigned long mfdb_pixel(MFDB *b, int x, int y, int packed)
{
    unsigned short *words = b->fd_addr;
    unsigned long value = 0;
    int p;
    if (packed && !b->fd_stand && b->fd_nplanes == 16)
        return words[y * b->fd_wdwidth * 16 + x];
    for (p = 0; p < b->fd_nplanes; p++)
    {
        long at = b->fd_stand ? (p * b->fd_h + y) * b->fd_wdwidth + x / 16 :
                  (y * b->fd_wdwidth + x / 16) * b->fd_nplanes + p;
        if (words[at] & (0x8000U >> (x & 15))) value |= 1UL << p;
    }
    return value;
}

static void mfdb_put(MFDB *b, int x, int y, int packed, unsigned long value)
{
    unsigned short *words = b->fd_addr;
    int p;
    if (packed && !b->fd_stand && b->fd_nplanes == 16)
    {
        words[y * b->fd_wdwidth * 16 + x] = value;
        return;
    }
    for (p = 0; p < b->fd_nplanes; p++)
    {
        long at = b->fd_stand ? (p * b->fd_h + y) * b->fd_wdwidth + x / 16 :
                  (y * b->fd_wdwidth + x / 16) * b->fd_nplanes + p;
        if (value & (1UL << p)) words[at] |= 0x8000U >> (x & 15);
    }
}

static void planar_benchmark(short handle, int planes)
{
    COLOR_TAB256 palette;
    uint32_t *source;
    unsigned char *indices, *native, *readback;
    GCBITMAP src, dst;
    MFDB screen, buffer, captured;
    short sr[4] = { 0, 0, 159, 99 }, dr[4] = { 0, 0, 319, 199 };
    short xy[8] = { 0, 0, 319, 199, 0, 0, 319, 199 };
    long i, x, y, begin, fused, split;
    char message[160];
    report("\r\nPLANAR BENCH allocating\r\n");
    source = malloc(160UL * 100 * 4); indices = malloc(320UL * 200);
    native = malloc(40UL * planes * 200); readback = malloc(40UL * planes * 200);
    src = bitmap(source, 160, 100, 32, PX_PREF32);
    dst = bitmap(indices, 320, 200, 8, PX_PREF8);
    check(source && indices && native && readback, "planar benchmark allocations");
    report("PLANAR BENCH palette\r\n");
    check(vq_ctab(handle, sizeof(palette), (COLOR_TAB *)&palette) == 1, "planar benchmark palette");
    dst.ctab = (COLOR_TAB *)&palette; dst.itab = v_create_itab(handle, dst.ctab, 4);
    check(dst.itab != NULL, "planar benchmark inverse table");
    report("PLANAR BENCH transfer\r\n");
    memset(&screen, 0, sizeof(screen)); memset(&buffer, 0, sizeof(buffer));
    buffer.fd_addr = native; buffer.fd_w = 320; buffer.fd_h = 200;
    buffer.fd_wdwidth = 20; buffer.fd_nplanes = planes;
    captured = buffer; captured.fd_addr = readback;
    for (i = 0; i < 160L * 100; i++) source[i] = i * 937UL;
    vs_clip(handle, 0, dr);
    vr_transfer_bits(handle, &src, NULL, sr, dr, 128); /* Warm automatic inverse. */
    report("\r\nPLANAR BENCH warm-up complete\r\n");
    begin = Supexec(clock_ticks);
    for (i = 0; i < 1; i++) vr_transfer_bits(handle, &src, NULL, sr, dr, 128);
    fused = Supexec(clock_ticks) - begin;
    vro_cpyfm(handle, 3, xy, &screen, &captured);
    begin = Supexec(clock_ticks);
    for (i = 0; i < 1; i++)
    {
        vr_transfer_bits(handle, &src, &dst, sr, dr, 128);
        memset(native, 0, 40UL * planes * 200);
        for (y = 0; y < 200; y++)
            for (x = 0; x < 320; x++) mfdb_put(&buffer, x, y, 0, indices[y * 320 + x]);
        vro_cpyfm(handle, 3, xy, &buffer, &screen);
    }
    split = Supexec(clock_ticks) - begin;
    check(memcmp(native, readback, 40UL * planes * 200) == 0, "planar end-to-end benchmark pixels");
    sprintf(message, "\r\nPLANAR SCREEN BENCH depth=%d RGB32 160x100 -> 320x200 dither fused=%ld split+planar+blit=%ld ticks (1 call)\r\n",
            planes, fused, split);
    report(message);
    check(v_delete_itab(handle, dst.itab) == 1, "planar benchmark inverse delete");
    free(source); free(indices); free(native); free(readback);
}

/* Compare the bit-15 trap against an independently expanded bitmap passed to
 * the ordinary driver operation. This tests the assembly ABI as well as all
 * Boolean/expansion modes without assuming the driver's pen packing.
 */
static void legacy_test(short handle, int planes, int packed)
{
    MFDB src, scaled, actual, expected, screen;
    short full[8] = { 0, 0, 63, 39, 0, 0, 63, 39 };
    short clip[4] = { 11, 12, 49, 30 }, pens[2] = { 1, 0 };
    long bytes = 4L * 2 * planes * 40;
    int expand, standard, to_screen, mode, shrink, x, y;
    memset(&src, 0, sizeof(src)); memset(&scaled, 0, sizeof(scaled));
    memset(&actual, 0, sizeof(actual)); memset(&screen, 0, sizeof(screen));
    src.fd_w = 32; src.fd_wdwidth = 2; src.fd_h = 17;
    scaled.fd_w = actual.fd_w = 64;
    scaled.fd_wdwidth = actual.fd_wdwidth = 4;
    scaled.fd_h = actual.fd_h = 40;
    actual.fd_nplanes = planes;
    src.fd_addr = malloc(2L * 2 * planes * 17 + 64);
    scaled.fd_addr = malloc(bytes + 64); actual.fd_addr = malloc(bytes + 64);
    expected = actual; expected.fd_addr = malloc(bytes + 64);
    check(src.fd_addr && scaled.fd_addr && actual.fd_addr && expected.fd_addr, "legacy allocations");
    for (expand = 0; expand < 2; expand++)
    for (standard = 0; standard < 2; standard++)
    for (to_screen = 0; to_screen < 2; to_screen++)
    for (shrink = 0; shrink < 2; shrink++)
    for (mode = expand ? 1 : 0; mode <= (expand ? 4 : 15); mode++)
    {
        int dw = shrink ? 11 : 53, dh = shrink ? 9 : 31;
        short xy[8] = { 1, 2, 23, 16, 7, 8, 7 + dw - 1, 8 + dh - 1 };
        short refxy[8] = { 0, 0, dw - 1, dh - 1, 7, 8, 7 + dw - 1, 8 + dh - 1 };
        src.fd_nplanes = scaled.fd_nplanes = expand ? 1 : planes;
        src.fd_stand = standard;
        memset(src.fd_addr, 0, 2L * 2 * planes * 17 + 64);
        memset(scaled.fd_addr, 0, bytes + 64);
        for (y = 0; y < 17; y++)
            for (x = 0; x < 32; x++) mfdb_put(&src, x, y, packed, x * 937UL + y * 317UL);
        for (y = 0; y < dh; y++)
            for (x = 0; x < dw; x++)
                mfdb_put(&scaled, x, y, packed, mfdb_pixel(&src, 1 + x * 23 / dw, 2 + y * 15 / dh, packed));
        memset(actual.fd_addr, 0xa7, bytes + 64); memset(expected.fd_addr, 0xa7, bytes + 64);
        vs_clip(handle, 0, clip);
        if (to_screen)
        {
            vro_cpyfm(handle, 3, full, &actual, &screen);
            refxy[0] = clip[0] - 7; refxy[1] = clip[1] - 8;
            refxy[4] = clip[0]; refxy[5] = clip[1];
            if (refxy[6] > clip[2]) { refxy[2] -= refxy[6] - clip[2]; refxy[6] = clip[2]; }
            if (refxy[7] > clip[3]) { refxy[3] -= refxy[7] - clip[3]; refxy[7] = clip[3]; }
        }
        if (expand) vrt_cpyfm(handle, mode, refxy, &scaled, &expected, pens);
        else vro_cpyfm(handle, mode, refxy, &scaled, &expected);
        /* Memory output must ignore clipping, screen output must retain phase. */
        vs_clip(handle, 1, clip);
        if (expand) vrt_cpyfm(handle, mode | 0x8000U, xy, &src, to_screen ? &screen : &actual, pens);
        else vro_cpyfm(handle, mode | 0x8000U, xy, &src, to_screen ? &screen : &actual);
        vs_clip(handle, 0, clip);
        if (to_screen) vro_cpyfm(handle, 3, full, &screen, &actual);
        for (x = 0; x < 64; x++)
        {
            if (((unsigned char *)actual.fd_addr)[bytes + x] != 0xa7 ||
                ((unsigned char *)expected.fd_addr)[bytes + x] != 0xa7 ||
                ((unsigned char *)src.fd_addr)[2L * 2 * planes * 17 + x] != 0 ||
                ((unsigned char *)scaled.fd_addr)[bytes + x] != 0)
            {
                char message[128];
                sprintf(message, "legacy guard depth=%d expand=%d standard=%d screen=%d shrink=%d mode=%d\r\n",
                        planes, expand, standard, to_screen, shrink, mode);
                report(message);
                check(0, "legacy allocation guard");
            }
        }
        if (memcmp(actual.fd_addr, expected.fd_addr, bytes))
        {
            char message[128];
            sprintf(message, "legacy depth=%d expand=%d standard=%d screen=%d shrink=%d mode=%d\r\n",
                    planes, expand, standard, to_screen, shrink, mode);
            report(message);
            check(0, "legacy scaling pixels");
        }
    }
    free(src.fd_addr); free(scaled.fd_addr); free(actual.fd_addr); free(expected.fd_addr);
    report("\r\nFVDI LEGACY GUEST PASS\r\n");
}

static void planar_screen_test(short handle, int planes)
{
    COLOR_TAB256 palette;
    uint32_t *source = malloc(23UL * 17 * 4);
    unsigned char *expected = malloc(79UL * 55);
    unsigned short *readback = calloc(6UL * planes * 80, 2);
    GCBITMAP src = bitmap(source, 23, 17, 32, PX_PREF32);
    GCBITMAP dst = bitmap(expected, 79, 55, 8, PX_PREF8);
    MFDB screen, memory;
    short sr[4] = { 0, 0, 22, 16 }, dr[4] = { 7, 9, 85, 63 };
    short mr[4] = { 0, 0, 78, 54 }, clip[4] = { 11, 12, 80, 60 };
    short xy[8] = { 0, 0, 95, 79, 0, 0, 95, 79 };
    int x, y, p, pass;
    long length = 48 + (1L << planes) * 8;
    check(source && expected && readback, "planar test allocations");
    if (planes > 1)
    {
        short red[3] = { 1000, 0, 0 };
        COLOR_ENTRY color;
        vs_color(handle, 2, red);
        check(vq_ctab_entry(handle, v_ctab_vdi2idx(handle, 2), &color) == 1 &&
              color.rgb.red == 65535 && color.rgb.green == 0 && color.rgb.blue == 0,
              "planar palette setter and index ordering");
    }
    check(vq_ctab(handle, length, (COLOR_TAB *)&palette) == 1 &&
          palette.no_colors == (1L << planes) && palette.length == length, "planar current palette size");
    check(vs_dflt_ctab(handle) == (1 << planes), "planar default setter count");
    check(vs_ctab(handle, (COLOR_TAB *)&palette) == (1 << planes), "planar restore complete CTAB");
    check(v_ctab_idx2value(handle, 1) == 1 && v_ctab_idx2value(handle, -1) == (1UL << planes) - 1,
          "planar CTAB pixel values and invalid-index black");
    dst.ctab = (COLOR_TAB *)&palette;
    dst.itab = v_create_itab(handle, dst.ctab, 4);
    check(dst.itab != NULL, "planar reference inverse table");
    for (x = 0; x < 23 * 17; x++) source[x] = (x * 937UL) & 0xffffffUL;
    memset(&screen, 0, sizeof(screen)); memset(&memory, 0, sizeof(memory));
    memory.fd_addr = readback; memory.fd_w = 96; memory.fd_h = 80;
    memory.fd_wdwidth = 6; memory.fd_nplanes = planes;
    for (pass = 0; pass < 2; pass++)
    {
        memset(readback, 0, 6UL * planes * 80 * 2);
        vs_clip(handle, 0, clip);
        vro_cpyfm(handle, 3, xy, &memory, &screen);
        memset(expected, 0xa7, 79UL * 55);
        vr_transfer_bits(handle, &src, &dst, sr, mr, 128);
        vs_clip(handle, 1, clip);
        vr_transfer_bits(handle, &src, NULL, sr, dr, 128);
        vs_clip(handle, 0, clip);
        vro_cpyfm(handle, 3, xy, &screen, &memory);
        for (y = 0; y < 80; y++)
            for (x = 0; x < 96; x++)
            {
                int actual = 0, want = 0;
                for (p = 0; p < planes; p++)
                    if (readback[(y * 6 + x / 16) * planes + p] & (0x8000U >> (x % 16)))
                        actual |= 1 << p;
                if (x >= clip[0] && x <= clip[2] && y >= clip[1] && y <= clip[3] &&
                    x <= dr[2] && y <= dr[3])
                    want = expected[(y - dr[1]) * 79 + x - dr[0]];
                check(actual == want, "planar screen clipping/diffusion/readback");
            }
        mr[2] = 10; mr[3] = 8; dr[2] = 17; dr[3] = 17;
    }
    {
        short pixel, pen;
        unsigned char index = 1;
        GCBITMAP marker = bitmap(&index, 1, 1, 8, PX_PREF8);
        short point[4] = { 90, 75, 90, 75 }, origin[4] = { 0, 0, 0, 0 };
        marker.ctab = dst.ctab;
        v_get_pixel(handle, 11, 12, &pixel, &pen);
        check(pixel == expected[3 * 79 + 4], "planar get-pixel ordering");
        vr_transfer_bits(handle, &marker, NULL, origin, point, 0);
        v_get_pixel(handle, 90, 75, &pixel, &pen);
        check(pixel == 1, "planar get-pixel low bit and indexed source");
        check(pen == v_ctab_idx2vdi(handle, 1), "planar get-pixel VDI colour number");
    }
    check(v_delete_itab(handle, dst.itab) == 1, "planar reference table deletion");
    free(source); free(expected); free(readback);
    report("\r\nFVDI PLANAR GUEST PASS\r\n");
}

static void allocation_probe(const char *phase)
{
    void *block;
    report(phase);
    block = (void *)Malloc(65536L);
    check((long)block > 0, "post-raster GEMDOS allocation");
    memset(block, 0x57, 65536L);
    check(Mfree(block) == 0, "post-raster GEMDOS free");
}

int main(int argc, char **argv)
{
    short handle = 0, in[11], out[57], info[272];
    uint32_t source[4] = { 0x00ff0000UL, 0x0000ff00UL, 0x000000ffUL, 0x00ffffffUL };
    uint16_t destination[4] = { 0 }, readback[16 * 4];
    GCBITMAP src = bitmap(source, 2, 2, 32, PX_PREF32);
    GCBITMAP dst = bitmap(destination, 2, 2, 16, PX_MATRIX16);
    short sr[4] = { 0, 0, 1, 1 }, dr[4] = { 0, 0, 1, 1 };
    short xy[8] = { 16, 16, 19, 19, 0, 0, 3, 3 };
    short clip[4] = { 17, 17, 18, 18 };
    MFDB screen, buffer;
    int i;

    report("\r\nFVDI TRANSFER GUEST START\r\n");
    atexit(finish);
    for (i = 0; i < 10; i++) in[i] = 1;
    in[10] = 2;
    if (argc == 2 && strcmp(argv[1], "--aes") == 0)
    {
        short cw, ch, bw, bh;
        check(appl_init() >= 0, "AES application registration");
        aes_active = 1;
        report("TRANSFER AES application registered\r\n");
        handle = graf_handle(&cw, &ch, &bw, &bh);
        v_opnvwk(in, &handle, out);
    } else
    {
        check(argc == 1, "usage: RASTER.PRG [--aes]");
        v_opnwk(in, &handle, out);
    }
    check(handle != 0, "open workstation");
    test_handle = handle;
    if (aes_active)
    {
        check(wind_update(BEG_UPDATE) != 0, "AES screen lock");
        update_locked = 1;
        v_hide_c(handle);
    }
    check(out[0] >= 319 && out[1] >= 199, "screen size");
    report("TRANSFER workstation open\r\n");
    vq_scrninfo(handle, info);
    report("TRANSFER screen queried\r\n");
    if (RASTER_BENCH_ONLY || (argc > 1 && strcmp(argv[1], "--bench") == 0))
    {
        benchmark(handle, 320, 200, 320, 200, 0);
        benchmark(handle, 160, 100, 320, 200, 0);
        benchmark(handle, 197, 113, 320, 200, 0);
        finish();
        report("\r\nRASTER MEMORY BENCH PASS\r\n");
        return 0;
    }
    {
        unsigned long format, expected_format = info[2] == 16 ? PX_MATRIX16 :
            0x01000000UL | ((long)info[2] << 8) | info[2] | (info[2] == 1 ? 0x20000UL : 0);
        check(vq_px_format(handle, &format) == 1 && format == expected_format, "workstation pixel format query");
    }
    if (info[0] == 0 && (info[2] == 1 || info[2] == 2 || info[2] == 4 || info[2] == 8))
    {
        direct_source_test(handle);
        allocation_probe("\r\nHEAP PROBE after direct sources\r\n");
        planar_screen_test(handle, info[2]);
        allocation_probe("\r\nHEAP PROBE after planar\r\n");
        legacy_test(handle, info[2], 0);
        allocation_probe("\r\nHEAP PROBE after legacy\r\n");
        planar_benchmark(handle, info[2]);
        finish();
        report("\r\nFVDI TRANSFER GUEST PASS\r\n");
        return 0;
    }
    check(info[0] == 2 && info[2] == 16 && info[8] == 5 && info[9] == 6, "RGB565 screen");
    direct_source_test(handle);
    legacy_test(handle, 16, 1);
    vr_transfer_bits(handle, &src, &dst, sr, dr, 0);
    check(destination[0] == 0xf800 && destination[1] == 0x07e0 &&
          destination[2] == 0x001f && destination[3] == 0xffff, "native-size conversion");
    dr[2] = dr[3] = 0;
    vr_transfer_bits(handle, &src, &dst, sr, dr, 32);
    check(destination[0] == 0x8410, "interpolated reduction");
    memset(&screen, 0, sizeof(screen));
    memset(&buffer, 0, sizeof(buffer));
    memset(readback, 0, sizeof(readback));
    buffer.fd_addr = readback;
    buffer.fd_w = 16;
    buffer.fd_h = 4;
    buffer.fd_wdwidth = 1;
    buffer.fd_nplanes = 16;
    /* Clear a 4x4 patch, then verify clipping and the driver's readback. */
    xy[0] = xy[1] = 0; xy[2] = xy[3] = 3;
    xy[4] = xy[5] = 16; xy[6] = xy[7] = 19;
    vs_clip(handle, 0, clip);
    vro_cpyfm(handle, 3, xy, &buffer, &screen);
    dr[0] = dr[1] = 16; dr[2] = dr[3] = 19;
    vs_clip(handle, 1, clip);
    vr_transfer_bits(handle, &src, NULL, sr, dr, 0);
    vs_clip(handle, 0, clip);
    xy[0] = xy[1] = 16; xy[2] = xy[3] = 19;
    xy[4] = xy[5] = 0; xy[6] = xy[7] = 3;
    vro_cpyfm(handle, 3, xy, &screen, &buffer);
    for (i = 0; i < 4; i++)
    {
        check(readback[i] == 0 && readback[48 + i] == 0, "vertical clip");
        check(readback[16 * i] == 0 && readback[16 * i + 3] == 0, "horizontal clip");
    }
    check(readback[17] == 0xf800 && readback[18] == 0x07e0 &&
          readback[33] == 0x001f && readback[34] == 0xffff, "screen transfer pixels");
    benchmark(handle, 320, 200, 320, 200, 1);
    benchmark(handle, 160, 100, 320, 200, 1);
    benchmark(handle, 197, 113, 320, 200, 1);
    indexed_test(handle);
    finish();
    report("\r\nFVDI TRANSFER GUEST PASS\r\n");
    return 0;
}
