/*
 * dither_guest.c - vr_transfer_bits with error diffusion (mode 128) and
 * 8-bit indexed memory destinations, on EmuTOS's VDI; from fVDI's
 * transfer_guest.c (indexed_test, indexed_source_test, direct_source_test,
 * planar_screen_test), made to run on every screen EmuTOS has.
 *
 * (a) RGB32 onto 8-bit indices with a palette and its inverse table: an
 *     enlargement, a filtered reduction, a changed palette refused, inverse
 *     tables deleted once and never given again, a mid grey dithered onto
 *     black and white
 * (b) 8-bit sources: a palette equal to the destination's keeps its indices
 *     and is not dithered (nor filtered when reduced), a reordered one is
 *     remapped, diffusion of an indexed source equals that of the same
 *     colours as RGB32; no tables at the same size: the bytes as they are
 * (c) RGB565, RGB555 (both byte orders), RGB24 (both orders) against the
 *     same colours as RGB32: 1:1, enlarged, reduced, with and without mode 128
 * (d) 8-bit bitmaps without a table: the palette is the pens, as source and
 *     as destination; the same as a table of the pens with its inverse table
 * (e) planes (1, 2, 4, 8): mode 128 onto the screen with a clipping
 *     rectangle is the dithering of the whole rectangle into memory with the
 *     screen's table (vq_ctab) and its inverse table, inside the clip, and
 *     nothing outside; reduced too; v_get_pixel agrees; into memory in the
 *     screen's format the same
 * (f) 16 bits: mode 128 onto the screen draws what mode 0 draws
 * (g) refusals: a palette without its inverse table, one of another
 *     workstation, a 1-bit source, mode 33, a destination of 16 bits that
 *     says 8-bit indices, an index beyond the source's table
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI DITHER GUEST PASS" or "FVDI DITHER FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long magic;
    long length;
    long format;
    long reserved;
    unsigned char *addr;
    long width;
    long bits;
    unsigned long px_format;
    long xmin, ymin, xmax, ymax;
    void *ctab;
    void *itab;
    long reserved0;
    long reserved1;
} GCBITMAP;

typedef struct {
    unsigned short reserved, red, green, blue;
} Entry;

typedef struct {
    long magic, length, format, reserved, map_id, color_space, flags, no_colors;
    long reserved1, reserved2, reserved3, reserved4;
    Entry colors[256];
} CTab;

#define CTAB_MAGIC  0x63746162L
#define F_32    0x03421820UL
#define F_24    0x03021818UL
#define F_16    0x03021010UL
#define F_15    0x03420f10UL
#define F_8     0x01020808UL
#define F_1     0x01020101UL
#define F_REV   0x00800000UL

static short contrl[12], intin[16], ptsin[16], intout[64], ptsout[16];
static int failed;
static short screen, planes, ncol;
static unsigned long fmt_screen;
static long checks;

static void say(const char *text)
{
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

static void check(int ok, const char *what)
{
    char line[200];

    checks++;
    if (!ok)
    {
        snprintf(line, sizeof(line), "DITHER FAILED: %s", what);
        say(line);
        if (!failed)
        {
            failed = 1;
            snprintf(line, sizeof(line), "FVDI DITHER FAIL: %s", what);
            say(line);
        }
    }
}

static void call(short h, short opcode, short sub, short nintin, short nptsin, void *out)
{
    short *pb[5];

    pb[0] = contrl;
    pb[1] = intin;
    pb[2] = ptsin;
    pb[3] = out ? (short *)out : intout;
    pb[4] = ptsout;
    contrl[0] = opcode;
    contrl[1] = nptsin;
    contrl[2] = 0;
    contrl[3] = nintin;
    contrl[4] = 0;
    contrl[5] = sub;
    contrl[6] = h;
    __asm__ __volatile__(
        "move.l %0,%%d1\n\t"
        "move.w #115,%%d0\n\t"
        "trap #2"
        :
        : "g"(pb)
        : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

static long get_long(const short *p)
{
    return ((long)p[0] << 16) | (unsigned short)p[1];
}

static void put_long(short *p, long v)
{
    p[0] = (short)(v >> 16);
    p[1] = (short)v;
}

static void transfer(short h, GCBITMAP *src, GCBITMAP *dst, const short *sr, const short *dr, short mode)
{
    int i;

    memset(contrl, 0, sizeof(contrl));
    *(GCBITMAP **)&contrl[7] = src;
    *(GCBITMAP **)&contrl[9] = dst;
    intin[0] = mode;
    for (i = 0; i < 4; i++)
    {
        ptsin[i] = sr[i];
        ptsin[4 + i] = dr[i];
    }
    call(h, 170, 0, 1, 4, NULL);
}

static long create_itab(short h, void *t, short bits)
{
    put_long(intin, (long)t);
    intin[2] = bits;
    call(h, 208, 0, 3, 0, NULL);
    return get_long(intout);
}

static short delete_itab(short h, long id)
{
    put_long(intin, id);
    call(h, 208, 1, 2, 0, NULL);
    return intout[0];
}

static short vq_ctab(short h, long room, CTab *t)
{
    put_long(intin, room);
    call(h, 206, 0, 2, 0, t);
    return contrl[4];
}

static short idx2vdi(short h, short index)
{
    intin[0] = index;
    call(h, 206, 3, 1, 0, NULL);
    return intout[0];
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

static void palette(CTab *p, int count)
{
    memset(p, 0, sizeof(*p));
    p->magic = CTAB_MAGIC;
    p->length = 48 + 8L * count;
    p->color_space = 1;
    p->no_colors = count;
}

static short open_vwk(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, chh, bw, bh, h;

    h = graf_handle(&cw, &chh, &bw, &bh);
    v_opnvwk(work_in, &h, work_out);
    return h;
}

static unsigned short to16(short v)
{
    if (v <= 0)
        return 0;
    if (v >= 1000)
        return 0xffff;
    return (unsigned short)(((unsigned long)v * 65535UL + 500) / 1000);
}

/* ------------------------------------------------------------------ */
/* (a) */

static void test_rgb32(void)
{
    static CTab pal;
    unsigned long s[4] = { 0x00ff0000UL, 0x0000ff00UL, 0x000000ffUL, 0x00ffffffUL };
    unsigned char d[16];
    GCBITMAP src = bitmap(s, 2, 2, 32, F_32);
    GCBITMAP dst = bitmap(d, 4, 4, 8, F_8);
    short sr[4] = { 0, 0, 1, 1 }, dr[4] = { 0, 0, 3, 3 };
    long id, old;
    int x, y, ones = 0;

    palette(&pal, 4);
    pal.colors[0].red = pal.colors[1].green = pal.colors[2].blue = 65535;
    pal.colors[3].red = pal.colors[3].green = pal.colors[3].blue = 65535;
    id = create_itab(screen, &pal, 4);
    check(id != 0, "inverse table");
    dst.ctab = &pal;
    dst.itab = (void *)id;
    memset(d, 0xa7, sizeof(d));
    transfer(screen, &src, &dst, sr, dr, 0);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            check(d[y * 4 + x] == (y / 2) * 2 + x / 2, "indexed enlargement");
    dr[2] = dr[3] = 0;
    transfer(screen, &src, &dst, sr, dr, 32);
    check(d[0] == 3, "indexed interpolated reduction");
    memset(d, 0xa7, sizeof(d));
    pal.colors[0].red--;
    transfer(screen, &src, &dst, sr, dr, 0);
    check(d[0] == 0xa7, "a palette changed since its inverse table was made is refused");
    check(delete_itab(screen, id) == 1, "inverse table deletion");
    check(delete_itab(screen, id) == 0, "a second deletion is refused");
    old = id;
    palette(&pal, 2);
    pal.colors[1].red = pal.colors[1].green = pal.colors[1].blue = 65535;
    id = create_itab(screen, &pal, 4);
    check(id != 0 && id != old, "inverse table numbers are not given again");
    dst.itab = (void *)id;
    dr[2] = dr[3] = 3;
    for (x = 0; x < 4; x++)
        s[x] = 0x00808080UL;
    transfer(screen, &src, &dst, sr, dr, 128);
    for (x = 0; x < 16; x++)
    {
        check(d[x] <= 1, "dithered indices stay in the palette");
        ones += d[x];
    }
    check(ones > 3 && ones < 13, "error diffusion of a mid grey onto black and white");
    memset(d, 0xa7, sizeof(d));
    transfer(screen, &src, &dst, sr, dr, 0);
    for (x = 0, ones = 0; x < 16; x++)
        ones += d[x] == d[0];
    check(ones == 16, "without mode 128 a flat grey stays flat");
    check(delete_itab(screen, id) == 1, "dither inverse table deletion");
}

/* (b) */

static void test_indexed_source(void)
{
    static CTab pal, source_pal;
    unsigned char s[4] = { 0, 1, 2, 3 }, d[16], expected[16];
    unsigned long rgb[4];
    GCBITMAP src = bitmap(s, 2, 2, 8, F_8);
    GCBITMAP dst = bitmap(d, 4, 4, 8, F_8);
    GCBITMAP direct = bitmap(rgb, 2, 2, 32, F_32), reference;
    short sr[4] = { 0, 0, 1, 1 }, dr[4] = { 0, 0, 3, 3 };
    long id;
    int i, x, y;

    palette(&pal, 4);
    pal.colors[0].red = pal.colors[1].green = pal.colors[2].blue = 65535;
    pal.colors[3].red = pal.colors[3].green = pal.colors[3].blue = 65535;
    memcpy(&source_pal, &pal, sizeof(pal));
    src.ctab = &source_pal;
    dst.ctab = &pal;
    id = create_itab(screen, &pal, 4);
    dst.itab = (void *)id;
    check(id != 0, "remap inverse table");
    transfer(screen, &src, &dst, sr, dr, 128);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            check(d[y * 4 + x] == (y / 2) * 2 + x / 2, "an equal palette keeps its indices");
    dr[2] = dr[3] = 0;
    transfer(screen, &src, &dst, sr, dr, 128);
    check(d[0] == 0, "an equal palette is not filtered when reduced");
    for (i = 0; i < 4; i++)
        source_pal.colors[i] = pal.colors[3 - i];
    dr[2] = dr[3] = 3;
    transfer(screen, &src, &dst, sr, dr, 128);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            check(d[y * 4 + x] == 3 - (y / 2) * 2 - x / 2, "a reordered palette is remapped");
    source_pal.colors[0].red = source_pal.colors[0].green = source_pal.colors[0].blue = 32768;
    for (i = 0; i < 4; i++)
        rgb[i] = ((unsigned long)(source_pal.colors[i].red >> 8) << 16) |
                 ((unsigned long)(source_pal.colors[i].green >> 8) << 8) |
                 (source_pal.colors[i].blue >> 8);
    reference = dst;
    reference.addr = expected;
    for (i = 0; i < 2; i++)
    {
        memset(d, 0xa7, sizeof(d));
        memset(expected, 0xa7, sizeof(expected));
        transfer(screen, &src, &dst, sr, dr, 128);
        transfer(screen, &direct, &reference, sr, dr, 128);
        check(memcmp(d, expected, sizeof(d)) == 0, "diffusion of an indexed source is that of its colours");
        dr[2] = dr[3] = 0;
    }
    check(delete_itab(screen, id) == 1, "remap inverse table deletion");
    src.ctab = dst.ctab = NULL;
    dst.itab = NULL;
    dr[2] = dr[3] = 1;
    memset(d, 0xa7, sizeof(d));
    transfer(screen, &src, &dst, sr, dr, 0);
    check(d[0] == 0 && d[1] == 1 && d[4] == 2 && d[5] == 3 && d[2] == 0xa7 && d[6] == 0xa7 && d[8] == 0xa7,
          "no tables at the same size: the bytes as they are");
}

/* (c) */

static void test_direct_sources(void)
{
    static unsigned char source[17 * 11 * 3 + 2], actual[31 * 19], expected[31 * 19];
    static unsigned long expanded[17 * 11];
    static CTab pal;
    GCBITMAP src, ref = bitmap(expanded, 17, 11, 32, F_32), dst, want;
    short sr[4] = { 0, 0, 16, 10 }, dr[4];
    int format, size, mode, i;
    long inverse;
    char text[120];

    palette(&pal, 17);
    for (i = 0; i < 17; i++)
    {
        pal.colors[i].red = i * 3119U;
        pal.colors[i].green = i * 7919U;
        pal.colors[i].blue = i * 1777U;
    }
    inverse = create_itab(screen, &pal, 4);
    check(inverse != 0, "direct-source inverse table");
    for (format = 0; format < 6; format++)
    {
        int bytes = format < 4 ? 2 : 3;

        src = bitmap(source, 17, 11, bytes * 8, format < 4 ? ((format & 1 ? F_15 : F_16) | (format & 2 ? F_REV : 0))
                                                     : (F_24 | (format & 1 ? F_REV : 0)));
        for (i = 0; i < 17 * 11; i++)
        {
            unsigned long pixel = (i * 937UL + 317) & 65535;

            if (format < 4)
            {
                unsigned long r = pixel >> (format & 1 ? 10 : 11) & 31;
                unsigned long g = pixel >> 5 & (format & 1 ? 31 : 63), b = pixel & 31;

                source[i * 2 + (format & 2 ? 1 : 0)] = pixel >> 8;
                source[i * 2 + (format & 2 ? 0 : 1)] = pixel;
                expanded[i] = (r * 8 + r / 4) * 65536UL +
                              (format & 1 ? g * 8 + g / 4 : g * 4 + g / 16) * 256UL + b * 8 + b / 4;
            }
            else
            {
                unsigned long r = (i * 37) & 255, g = (i * 101 + 7) & 255, b = (i * 59 + 200) & 255;

                source[i * 3 + 0] = format & 1 ? b : r;
                source[i * 3 + 1] = g;
                source[i * 3 + 2] = format & 1 ? r : b;
                expanded[i] = (r << 16) | (g << 8) | b;
            }
        }
        for (size = 0; size < 3; size++)
            for (mode = 0; mode <= 128; mode += 128)
            {
                int w = size == 0 ? 17 : size == 1 ? 31 : 7;
                int h = size == 0 ? 11 : size == 1 ? 19 : 5;

                dst = bitmap(actual, w, h, 8, F_8);
                dst.ctab = &pal;
                dst.itab = (void *)inverse;
                want = dst;
                want.addr = expected;
                dr[0] = dr[1] = 0;
                dr[2] = w - 1;
                dr[3] = h - 1;
                memset(actual, 0xa7, sizeof(actual));
                memset(expected, 0xa7, sizeof(expected));
                transfer(screen, &ref, &want, sr, dr, mode);
                transfer(screen, &src, &dst, sr, dr, mode);
                snprintf(text, sizeof(text), "format %08lx onto 8-bit indices, %dx%d, mode %d is its colours as RGB32",
                         src.px_format, w, h, mode);
                check(memcmp(actual, expected, sizeof(actual)) == 0 && expected[0] != 0xa7, text);
            }
    }
    check(delete_itab(screen, inverse) == 1, "direct-source inverse table deletion");
}

/* (d) */

static void test_pens_palette(void)
{
    static CTab pens;
    static unsigned char s[13 * 7], a[29 * 15], b[29 * 15];
    static unsigned long rgb[13 * 7];
    GCBITMAP src8 = bitmap(s, 13, 7, 8, F_8), src32 = bitmap(rgb, 13, 7, 32, F_32);
    GCBITMAP da = bitmap(a, 29, 15, 8, F_8), db = bitmap(b, 29, 15, 8, F_8);
    short sr[4] = { 0, 0, 12, 6 }, dr[4] = { 0, 0, 25, 13 }, rgb3[3];
    long id;
    int i, x, y, ok;

    palette(&pens, ncol);
    for (i = 0; i < ncol; i++)
    {
        vq_color(screen, i, 0, rgb3);
        pens.colors[i].red = to16(rgb3[0]);
        pens.colors[i].green = to16(rgb3[1]);
        pens.colors[i].blue = to16(rgb3[2]);
    }
    for (i = 0; i < 13 * 7; i++)
    {
        s[i] = (unsigned char)((i * 7 + 3) % ncol);
        rgb[i] = ((i * 37UL) & 255) << 16 | ((i * 91UL + 40) & 255) << 8 | ((i * 13UL + 99) & 255);
    }
    /* the pens on both sides: an enlargement keeps the indices, with and without mode 128 */
    for (i = 0; i < 2; i++)
    {
        memset(a, 0xa7, sizeof(a));
        transfer(screen, &src8, &da, sr, dr, i ? 128 : 0);
        ok = 1;
        for (y = 0; y <= dr[3]; y++)
            for (x = 0; x <= dr[2]; x++)
                if (a[y * 29 + x] != s[(y * 7 / 14) * 13 + x * 13 / 26])
                    ok = 0;
        check(ok, i ? "pens onto pens keep their indices (mode 128)" : "pens onto pens keep their indices");
    }
    /* a direct source onto the pens: as onto a table of the pens with its inverse table */
    id = create_itab(screen, &pens, 4);
    check(id != 0, "an inverse table of the pens");
    for (i = 0; i < 2; i++)
    {
        memset(a, 0xa7, sizeof(a));
        memset(b, 0xa7, sizeof(b));
        da.ctab = NULL;
        da.itab = NULL;
        transfer(screen, &src32, &da, sr, dr, i ? 128 : 0);
        db.ctab = &pens;
        db.itab = (void *)id;
        transfer(screen, &src32, &db, sr, dr, i ? 128 : 0);
        check(!memcmp(a, b, sizeof(a)) && a[0] != 0xa7, "a bitmap without a table has the pens as its palette");
        /* and its inverse table may be given with it */
        da.itab = (void *)id;
        memset(a, 0xa7, sizeof(a));
        transfer(screen, &src32, &da, sr, dr, i ? 128 : 0);
        check(!memcmp(a, b, sizeof(a)), "the pens with an inverse table made for them");
    }
    check(delete_itab(screen, id) == 1, "pens inverse table deletion");
}

/* ------------------------------------------------------------------ */
/* the screen */

static unsigned long mfdb_pixel(const unsigned short *words, int wdwidth, int x, int y)
{
    unsigned long v = 0;
    int p;

    if (planes == 16)
        return words[y * wdwidth * 16 + x];
    for (p = 0; p < planes; p++)
        if (words[(y * wdwidth + x / 16) * planes + p] & (0x8000U >> (x & 15)))
            v |= 1UL << p;
    return v;
}

/* (e) */
static void test_planes(void)
{
    static CTab pal;
    static unsigned long source[23 * 17];
    static unsigned char expected[79 * 55];
    static unsigned short readback[6 * 8 * 80], zero[6 * 8 * 80], memplanes[6 * 8 * 80];
    GCBITMAP src = bitmap(source, 23, 17, 32, F_32);
    GCBITMAP dst = bitmap(expected, 79, 55, 8, F_8);
    GCBITMAP mem;
    MFDB scr, memory, clear;
    short sr[4] = { 0, 0, 22, 16 }, dr[4] = { 7, 9, 85, 63 };
    short mr[4] = { 0, 0, 78, 54 }, clip[4] = { 11, 12, 80, 60 };
    short xy[8] = { 0, 0, 95, 79, 0, 0, 95, 79 };
    int x, y, pass, bad;
    long length = 48 + 8L * ncol, id;
    char text[120];

    check(vq_ctab(screen, length, &pal) == length / 2 && pal.no_colors == ncol, "the screen's table");
    id = create_itab(screen, &pal, 4);
    check(id != 0, "an inverse table of the screen's table");
    dst.ctab = &pal;
    dst.itab = (void *)id;
    for (x = 0; x < 23 * 17; x++)
        source[x] = (x * 937UL) & 0xffffffUL;
    memset(&scr, 0, sizeof(scr));
    memset(&memory, 0, sizeof(memory));
    memory.fd_addr = readback;
    memory.fd_w = 96;
    memory.fd_h = 80;
    memory.fd_wdwidth = 6;
    memory.fd_nplanes = planes;
    clear = memory;
    clear.fd_addr = zero;
    for (pass = 0; pass < 2; pass++)
    {
        vs_clip(screen, 0, clip);
        vro_cpyfm(screen, 3, xy, &clear, &scr);         /* pixel value 0 */
        memset(expected, 0xa7, sizeof(expected));
        transfer(screen, &src, &dst, sr, mr, 128);
        vs_clip(screen, 1, clip);
        transfer(screen, &src, NULL, sr, dr, 128);
        vs_clip(screen, 0, clip);
        memset(readback, 0, sizeof(readback));
        vro_cpyfm(screen, 3, xy, &scr, &memory);
        bad = 0;
        for (y = 0; y < 80; y++)
            for (x = 0; x < 96; x++)
            {
                unsigned long want = 0;

                if (x >= clip[0] && x <= clip[2] && y >= clip[1] && y <= clip[3] && x <= dr[2] && y <= dr[3])
                    want = expected[(y - dr[1]) * 79 + x - dr[0]];
                if (mfdb_pixel(readback, 6, x, y) != want)
                    bad++;
            }
        snprintf(text, sizeof(text), "%s: mode 128 onto the screen, clipped, is the dithering of the whole rectangle (%d wrong)",
                 pass ? "reduced" : "enlarged", bad);
        check(bad == 0, text);
        if (pass == 0)
        {
            short pixel, pen;

            v_get_pixel(screen, 11, 12, &pixel, &pen);
            check(pixel == expected[3 * 79 + 4], "v_get_pixel reads the value drawn");
            check(pen == idx2vdi(screen, pixel), "v_get_pixel's pen is v_ctab_idx2vdi's");
        }
        mr[2] = 10;
        mr[3] = 8;
        dr[2] = 17;
        dr[3] = 17;
    }

    /* into memory in the screen's format: the same pixels as the 8-bit reference */
    mr[2] = 78;
    mr[3] = 54;
    memset(expected, 0xa7, sizeof(expected));
    transfer(screen, &src, &dst, sr, mr, 128);
    memset(memplanes, 0, sizeof(memplanes));
    memset(&mem, 0, sizeof(mem));
    mem.magic = 0x6362746dL;
    mem.length = sizeof(mem);
    mem.addr = (unsigned char *)memplanes;
    mem.width = 6 * 2L * planes;
    mem.bits = planes;
    mem.px_format = fmt_screen;
    mem.xmax = 96;
    mem.ymax = 80;
    transfer(screen, &src, &mem, sr, mr, 128);
    bad = 0;
    for (y = 0; y < 80; y++)
        for (x = 0; x < 96; x++)
            if (mfdb_pixel(memplanes, 6, x, y) != (x <= 78 && y <= 54 ? expected[y * 79 + x] : 0))
                bad++;
    check(bad == 0, "mode 128 into memory in the screen's format is the 8-bit reference");

    /* an 8-bit source with the screen's table keeps its values */
    {
        unsigned char index = 1;
        GCBITMAP marker = bitmap(&index, 1, 1, 8, F_8);
        short point[4] = { 90, 75, 90, 75 }, origin[4] = { 0, 0, 0, 0 }, pixel, pen;

        marker.ctab = &pal;
        transfer(screen, &marker, NULL, origin, point, 128);
        v_get_pixel(screen, 90, 75, &pixel, &pen);
        check(pixel == 1, "an indexed source with the screen's table keeps its value (mode 128)");
        check(pen == idx2vdi(screen, 1), "and v_get_pixel names its pen");
        index = (unsigned char)(ncol - 1);
        transfer(screen, &marker, NULL, origin, point, 0);
        v_get_pixel(screen, 90, 75, &pixel, &pen);
        check(pal.colors[pixel].red == pal.colors[ncol - 1].red && pal.colors[pixel].green == pal.colors[ncol - 1].green &&
              pal.colors[pixel].blue == pal.colors[ncol - 1].blue, "mode 0 draws the colour of the value");
    }
    check(delete_itab(screen, id) == 1, "screen inverse table deletion");
}

/* (f) */
static void test_16bit(void)
{
    static unsigned long source[23 * 17];
    static unsigned short a[96 * 80], b[96 * 80];
    GCBITMAP src = bitmap(source, 23, 17, 32, F_32);
    MFDB scr, memory;
    short sr[4] = { 0, 0, 22, 16 }, dr[4] = { 7, 9, 85, 63 };
    short xy[8] = { 0, 0, 95, 79, 0, 0, 95, 79 };
    int i;

    for (i = 0; i < 23 * 17; i++)
        source[i] = (i * 937UL) & 0xffffffUL;
    memset(&scr, 0, sizeof(scr));
    memset(&memory, 0, sizeof(memory));
    memory.fd_w = 96;
    memory.fd_h = 80;
    memory.fd_wdwidth = 6;
    memory.fd_nplanes = 16;
    vs_clip(screen, 0, dr);
    transfer(screen, &src, NULL, sr, dr, 0);
    memory.fd_addr = a;
    vro_cpyfm(screen, 3, xy, &scr, &memory);
    vr_recfl(screen, dr);
    transfer(screen, &src, NULL, sr, dr, 128);
    memory.fd_addr = b;
    vro_cpyfm(screen, 3, xy, &scr, &memory);
    check(!memcmp(a, b, sizeof(a)), "onto 16 bits mode 128 draws what mode 0 draws");
}

/* (g) */
static void test_refusals(void)
{
    static CTab pal;
    static unsigned char d[16], keep[16], s8[4] = { 0, 1, 2, 9 }, s1[2] = { 0xa0, 0x50 };
    unsigned long s[4] = { 0x00ff0000UL, 0x0000ff00UL, 0x000000ffUL, 0x00ffffffUL };
    GCBITMAP src = bitmap(s, 2, 2, 32, F_32), dst = bitmap(d, 4, 4, 8, F_8), d2, src2;
    short sr[4] = { 0, 0, 1, 1 }, dr[4] = { 0, 0, 3, 3 }, h2;
    long id, id2 = 0;

    palette(&pal, 4);
    pal.colors[0].red = pal.colors[1].green = pal.colors[2].blue = 65535;
    pal.colors[3].red = pal.colors[3].green = pal.colors[3].blue = 65535;
    id = create_itab(screen, &pal, 4);
    memset(d, 0xa7, sizeof(d));
    memcpy(keep, d, sizeof(d));

    d2 = dst;
    d2.ctab = &pal;
    transfer(screen, &src, &d2, sr, dr, 0);
    check(!memcmp(d, keep, sizeof(d)), "refused: a palette without its inverse table");
    h2 = open_vwk();
    if (h2 > 0)
    {
        id2 = create_itab(h2, &pal, 4);
        d2.itab = (void *)id2;
        transfer(screen, &src, &d2, sr, dr, 0);
        check(!memcmp(d, keep, sizeof(d)), "refused: an inverse table of another workstation");
        v_clsvwk(h2);
    }
    d2.itab = (void *)id;
    transfer(h2 > 0 ? screen : screen, &src, &d2, sr, dr, 0);
    check(memcmp(d, keep, sizeof(d)), "accepted: the palette with its inverse table");
    memset(d, 0xa7, sizeof(d));
    src2 = bitmap(s1, 16, 1, 1, F_1);
    src2.width = 2;
    {
        short r1[4] = { 0, 0, 3, 0 };

        transfer(screen, &src2, &d2, r1, r1, 0);
        check(!memcmp(d, keep, sizeof(d)), "refused: a 1-bit source onto 8-bit indices");
        transfer(screen, &src2, &d2, r1, r1, 33);
        check(!memcmp(d, keep, sizeof(d)), "refused: mode 33 onto 8-bit indices");
    }
    transfer(screen, &src, &d2, sr, dr, 33);
    check(!memcmp(d, keep, sizeof(d)), "refused: mode 33 of a direct source");
    d2.bits = 16;
    transfer(screen, &src, &d2, sr, dr, 0);
    check(!memcmp(d, keep, sizeof(d)), "refused: 16 bits that say 8-bit indices");
    d2.bits = 8;
    src2 = bitmap(s8, 2, 2, 8, F_8);
    src2.ctab = &pal;
    transfer(screen, &src2, &d2, sr, dr, 128);
    check(!memcmp(d, keep, sizeof(d)), "refused: an index beyond the source's table");
    pal.magic ^= 1;
    src2.ctab = &pal;
    s8[3] = 3;
    transfer(screen, &src2, &d2, sr, dr, 128);
    check(!memcmp(d, keep, sizeof(d)), "refused: a source table without the magic");
    pal.magic ^= 1;
    delete_itab(screen, id);
}

int main(void)
{
    short work_out[57];
    char text[120];

    say("DITHER start");
    check(appl_init() >= 0, "AES application registration");
    screen = open_vwk();
    check(screen > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(screen, 1, work_out);
    planes = work_out[4];
    vq_extnd(screen, 0, work_out);
    ncol = work_out[13] > 256 ? 256 : work_out[13];
    fmt_screen = planes == 16 ? F_16 : planes == 1 ? F_1 : 0x01000000UL | ((unsigned long)planes << 8) | planes;
    snprintf(text, sizeof(text), "DITHER %d planes, %d colours", planes, ncol);
    say(text);
    check(planes == 1 || planes == 2 || planes == 4 || planes == 8 || planes == 16,
          "a screen of 1, 2, 4, 8 or 16 planes");
    if (failed)
        goto out;

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    test_rgb32();
    say("DITHER RGB32 onto indices done");
    test_indexed_source();
    say("DITHER indexed sources done");
    test_direct_sources();
    say("DITHER direct sources done");
    test_pens_palette();
    say("DITHER bitmaps without a table done");
    if (planes == 16)
        test_16bit();
    else
        test_planes();
    say("DITHER screen done");
    test_refusals();
    say("DITHER refusals done");
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);

out:
    if (screen > 0)
        v_clsvwk(screen);
    appl_exit();
    if (!failed)
    {
        snprintf(text, sizeof(text), "DITHER %ld checks", checks);
        say(text);
        say("FVDI DITHER GUEST PASS");
    }
    return failed;
}
