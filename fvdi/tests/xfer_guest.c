/*
 * xfer_guest.c - vr_transfer_bits and the scaled vro_cpyfm and vrt_cpyfm of
 * EmuTOS's VDI (the slice: direct, 8-bit indexed and monochrome sources).
 *
 * (a) vq_px_format, and work_out[30] bits 0 and 1 clear (the new raster
 *     calls are not advertised as complete)
 * (b) every direct source format (15, 16, 16 reversed, 15 reversed, 24, 24
 *     reversed, 32 bits) into memory in the format of the screen, 1:1 and
 *     enlarged 2x and 3x; the expected pixels are computed here
 * (c) an 8-bit indexed source with a table of the caller's and without one
 *     (the pens of the workstation); an index beyond the table refuses
 * (d) a 1-bit source: copy, replace and transparent, in the raster colours
 * (e) the workstation as the destination, with a clipping rectangle: the
 *     pixels outside it stay, the sampling does not move
 * (f) refusals: nothing is drawn, the memory beside the destination and the
 *     screen are as they were
 * (g) vro_cpyfm and vrt_cpyfm with bit 15 of the mode against a reference
 *     (every mode, standard and device forms, to memory and to the screen)
 * (h) into the bitmap of a v_open_bm workstation
 *
 * The screen is locked and the mouse off for the whole of it (the AES
 * paints the desktop a while after a program starts).
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI XFER GUEST PASS" or "FVDI XFER FAIL: <check>".
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

short v_open_bm(short base_handle, GCBITMAP *bitmap, short color_flags,
                short unit_flags, short pixel_width, short pixel_height);
void v_clsbm(short handle);

#define F_32    0x03421820UL
#define F_24    0x03021818UL
#define F_16    0x03021010UL
#define F_15    0x03420f10UL
#define F_8     0x01020808UL
#define F_1     0x01020101UL
#define F_REV   0x00800000UL

static short contrl[12], intin[16], ptsin[16], intout[64], ptsout[16];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
static int failed;
static short screen, planes;
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
        snprintf(line, sizeof(line), "XFER FAILED: %s", what);
        say(line);
        if (!failed)
        {
            failed = 1;
            snprintf(line, sizeof(line), "FVDI XFER FAIL: %s", what);
            say(line);
        }
    }
}

static void vdi_raw(short h, short opcode, short sub, short nintin, short nptsin)
{
    contrl[0] = opcode;
    contrl[1] = nptsin;
    contrl[3] = nintin;
    contrl[5] = sub;
    contrl[6] = h;
    __asm__ __volatile__(
        "move.l %0,%%d1\n\t"
        "move.w #115,%%d0\n\t"
        "trap #2"
        :
        : "g"(pblock)
        : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

/* vr_transfer_bits: sr and dr are x1, y1, x2, y2 */
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
    vdi_raw(h, 170, 0, 1, 4);
}

/* v_color2value: the pixel of a colour of 0-65535 components */
static unsigned long color2value(unsigned short r, unsigned short g, unsigned short b)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short)r;
    intin[4] = (short)g;
    intin[5] = (short)b;
    vdi_raw(screen, 204, 0, 6, 0);
    return ((unsigned long)(unsigned short)intout[0] << 16) | (unsigned short)intout[1];
}

/* vs_fg_color or vs_bg_color of the raster colour (kind 4) */
static void set_raster(short opcode, unsigned long rgb8)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short)(((rgb8 >> 16) & 255) * 257);
    intin[4] = (short)(((rgb8 >> 8) & 255) * 257);
    intin[5] = (short)((rgb8 & 255) * 257);
    vdi_raw(screen, opcode, 4, 6, 0);
}

/* ------------------------------------------------------------------ */
/* pixels of the screen's format in memory */

static unsigned long pack565(unsigned long rgb)
{
    return ((rgb >> 8) & 0xf800UL) | ((rgb >> 5) & 0x07e0UL) | ((rgb >> 3) & 31UL);
}

/* the pixel the screen format has for a colour of 8-bit components */
static unsigned long exp_rgb(unsigned long rgb)
{
    if (planes == 16)
        return pack565(rgb);
    return color2value(((rgb >> 16) & 255) * 257, ((rgb >> 8) & 255) * 257, (rgb & 255) * 257);
}

static void dev_put(unsigned char *base, long pitch, int x, int y, unsigned long v)
{
    unsigned char *line = base + y * pitch;
    int p;

    if (planes == 16)
    {
        line[x * 2] = (unsigned char)(v >> 8);
        line[x * 2 + 1] = (unsigned char)v;
        return;
    }
    for (p = 0; p < planes; p++)
    {
        unsigned char *w = line + ((x >> 4) * planes + p) * 2 + ((x & 15) >> 3);
        int m = 0x80 >> (x & 7);

        if ((v >> p) & 1)
            *w |= m;
        else
            *w &= ~m;
    }
}

static unsigned long dev_get(const unsigned char *base, long pitch, int x, int y)
{
    const unsigned char *line = base + y * pitch;
    unsigned long v = 0;
    int p;

    if (planes == 16)
        return ((unsigned long)line[x * 2] << 8) | line[x * 2 + 1];
    for (p = 0; p < planes; p++)
        if (line[((x >> 4) * planes + p) * 2 + ((x & 15) >> 3)] & (0x80 >> (x & 7)))
            v |= 1UL << p;
    return v;
}

/* bytes of a line of w pixels in the screen's format */
static long line_bytes(int w)
{
    return planes == 16 ? w * 2L : ((w + 15) / 16) * 2L * planes;
}

static void mk_dst(GCBITMAP *b, unsigned char *p, long pitch, int w, int h)
{
    memset(b, 0, sizeof(*b));
    b->magic = 0x6362746dL;
    b->length = sizeof(*b);
    b->addr = p;
    b->width = pitch;
    b->bits = planes;
    b->px_format = fmt_screen;
    b->xmax = w;
    b->ymax = h;
}

/* ------------------------------------------------------------------ */
/* sources */

static unsigned long src_rgb(int x, int y)
{
    unsigned long r = (x * 37 + y * 11 + 5) & 255, g = (x * 13 + y * 53 + 90) & 255, b = (x * 7 + y * 29 + 170) & 255;

    return (r << 16) | (g << 8) | b;
}

#define SW 13
#define SH 7

/* the formats of the direct sources */
static const unsigned long direct_fmt[7] = {
    F_32, F_24, F_24 | F_REV, F_16, F_16 | F_REV, F_15, F_15 | F_REV
};

static int fmt_bytes(unsigned long fmt)
{
    unsigned long b = fmt & ~F_REV;

    return b == F_32 ? 4 : b == F_24 ? 3 : 2;
}

/* write a pixel into a source in a format, and say what colour it then is (8-bit components) */
static unsigned long put_direct(unsigned char *p, unsigned long fmt, unsigned long rgb)
{
    unsigned long base = fmt & ~F_REV;
    int rev = (fmt & F_REV) != 0;
    unsigned long r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255, v;

    if (base == F_32)
    {
        p[0] = 0x5a;                /* the spare byte is not read */
        p[1] = r;
        p[2] = g;
        p[3] = b;
        return rgb;
    }
    if (base == F_24)
    {
        p[rev ? 2 : 0] = r;
        p[1] = g;
        p[rev ? 0 : 2] = b;
        return rgb;
    }
    if (base == F_16)
    {
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        r = v >> 11;
        g = (v >> 5) & 63;
        g = (g << 2) | (g >> 4);
    }
    else
    {
        v = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        r = (v >> 10) & 31;
        g = (v >> 5) & 31;
        g = (g << 3) | (g >> 2);
    }
    b = v & 31;
    p[rev ? 1 : 0] = v >> 8;
    p[rev ? 0 : 1] = v;
    return (((r << 3) | (r >> 2)) << 16) | (g << 8) | ((b << 3) | (b >> 2));
}

#define MAXB 16384
static unsigned char sbuf[MAXB] __attribute__((aligned(2))), dbuf[MAXB] __attribute__((aligned(2))),
                     wbuf[MAXB] __attribute__((aligned(2)));
static unsigned long srgb[SH][SW];     /* the colours as the source has them */

/* a direct source of SW x SH in a format, with lines of pitch bytes */
static void build_direct(unsigned long fmt, long pitch, GCBITMAP *src)
{
    int x, y, n = fmt_bytes(fmt);

    memset(sbuf, 0xa5, sizeof(sbuf));
    for (y = 0; y < SH; y++)
        for (x = 0; x < SW; x++)
            srgb[y][x] = put_direct(sbuf + 8 + y * pitch + x * n, fmt, src_rgb(x, y));
    memset(src, 0, sizeof(*src));
    src->magic = 0x6362746dL;
    src->length = sizeof(*src);
    src->addr = sbuf + 8;
    src->width = pitch;
    src->bits = n * 8;
    src->px_format = fmt;
    src->xmax = SW;
    src->ymax = SH;
}

/*
 * Draw by vr_transfer_bits into a destination in memory of dw + 6 by dh + 6
 * pixels (the transfer takes the middle) and compare it with the same buffer
 * the expected pixels were put in.  'pix' gives the pixel that goes to a
 * place of the destination rectangle, or -1 for none (not drawn).
 */
typedef long (*PixFn)(int dx, int dy);

static void run_to_memory(const char *what, GCBITMAP *src, const short *sr, int dw, int dh,
                          short mode, PixFn pix, int refused)
{
    GCBITMAP dst;
    long pitch = line_bytes(dw + 6) + 2;
    long size = pitch * (dh + 6);
    short dr[4];
    int x, y;
    char text[160];

    dr[0] = 3;
    dr[1] = 3;
    dr[2] = 3 + dw - 1;
    dr[3] = 3 + dh - 1;
    memset(dbuf, 0xa5, sizeof(dbuf));
    mk_dst(&dst, dbuf + 16, pitch, dw + 6, dh + 6);
    memcpy(wbuf, dbuf, sizeof(dbuf));
    if (!refused)
        for (y = 0; y < dh; y++)
            for (x = 0; x < dw; x++)
            {
                long v = pix(x, y);

                if (v >= 0)
                    dev_put(wbuf + 16, pitch, 3 + x, 3 + y, (unsigned long)v);
            }
    transfer(screen, src, &dst, sr, dr, mode);
    snprintf(text, sizeof(text), "%s", what);
    check(memcmp(dbuf, wbuf, 16 + size + 64) == 0, text);
    if (memcmp(dbuf, wbuf, 16 + size + 64) != 0)
    {
        int shown = 0;

        for (y = 0; y < dh + 6 && shown < 3; y++)
            for (x = 0; x < dw + 6 && shown < 3; x++)
            {
                unsigned long g = dev_get(dbuf + 16, pitch, x, y), w = dev_get(wbuf + 16, pitch, x, y);

                if (g != w)
                {
                    snprintf(text, sizeof(text), "  at %d,%d got %lx want %lx", x, y, g, w);
                    say(text);
                    shown++;
                }
            }
    }
}

/* the pixel of the current direct source at a destination position, for a scale */
static int cur_scale;
static unsigned long cur_rgb[SH][SW];

static long pix_direct(int dx, int dy)
{
    return (long)exp_rgb(cur_rgb[dy / cur_scale][dx / cur_scale]);
}

/* (b) */
static void test_direct(void)
{
    int f, s, x, y;
    char text[120];

    for (f = 0; f < 7; f++)
        for (s = 1; s <= 3; s++)
        {
            GCBITMAP src;
            short sr[4] = { 0, 0, SW - 1, SH - 1 };
            long pitch = SW * fmt_bytes(direct_fmt[f]) + 2 * (f & 1);

            build_direct(direct_fmt[f], pitch, &src);
            for (y = 0; y < SH; y++)
                for (x = 0; x < SW; x++)
                    cur_rgb[y][x] = srgb[y][x];
            cur_scale = s;
            snprintf(text, sizeof(text), "format %08lx into memory at %dx", direct_fmt[f], s);
            run_to_memory(text, &src, sr, SW * s, SH * s, 0, pix_direct, 0);
            /* replace is the same as copy for colours */
            if (s == 2)
                run_to_memory("replace is copy for a direct source", &src, sr, SW * s, SH * s, 32, pix_direct, 0);
            /* bit 7 dithers onto planes (DITHER.PRG tests that); onto 16 bits it does nothing */
            if (s == 1 && planes == 16)
                run_to_memory("dither (bit 7) does nothing onto 16 bits", &src, sr, SW * s, SH * s, 128, pix_direct, 0);
        }
    /* a part of the source: the middle, with an origin that is not 0,0 */
    {
        GCBITMAP src;
        short sr[4] = { 3, 2, 9, 5 };

        build_direct(F_32, SW * 4, &src);
        src.xmin = -5;
        src.xmax = SW - 5;
        src.ymin = 10;
        src.ymax = 10 + SH;
        sr[0] -= 5;
        sr[2] -= 5;
        sr[1] += 10;
        sr[3] += 10;
        for (y = 0; y < SH; y++)
            for (x = 0; x < SW; x++)
                cur_rgb[y][x] = srgb[y][x];
        {
            /* the destination is 7 x 4 enlarged to 14 x 8: the part of the source, 2x */
            static unsigned long part[SH][SW];
            for (y = 0; y < 4; y++)
                for (x = 0; x < 7; x++)
                    part[y][x] = srgb[2 + y][3 + x];
            memcpy(cur_rgb, part, sizeof(cur_rgb));
            cur_scale = 2;
            run_to_memory("a part of a source with an origin", &src, sr, 14, 8, 0, pix_direct, 0);
        }
    }
}

/* (c) the indexed source */
static CTab ctab;
static unsigned char idx[SH][SW];

static long pix_index_table(int dx, int dy)
{
    Entry *e = &ctab.colors[idx[dy / cur_scale][dx / cur_scale]];

    return (long)exp_rgb(((unsigned long)(e->red >> 8) << 16) | ((unsigned long)(e->green >> 8) << 8) | (e->blue >> 8));
}

static short pen_rgb[256][3];
static int numcolors;

static long pix_index_pens(int dx, int dy)
{
    int i = idx[dy / cur_scale][dx / cur_scale];
    unsigned long c[3];
    int k;

    for (k = 0; k < 3; k++)
        c[k] = ((unsigned long)pen_rgb[i][k] * 65535UL + 500) / 1000;
    return (long)exp_rgb(((c[0] >> 8) << 16) | ((c[1] >> 8) << 8) | (c[2] >> 8));
}

static void test_indexed(void)
{
    GCBITMAP src;
    short sr[4] = { 0, 0, SW - 1, SH - 1 };
    int x, y, i, s;
    long pitch = SW + 3;

    memset(&ctab, 0, sizeof(ctab));
    ctab.magic = 0x63746162L;
    ctab.length = sizeof(ctab);
    ctab.no_colors = 11;
    for (i = 0; i < 11; i++)
    {
        if (planes == 16)
        {
            ctab.colors[i].red = (unsigned short)(i * 5843 + 1000);
            ctab.colors[i].green = (unsigned short)(i * 3331 + 20000);
            ctab.colors[i].blue = (unsigned short)(65535 - i * 6007);
        }
        else
        {
            short *c = pen_rgb[(i * 3) % numcolors];

            /* a pen's own colour, in whole 8-bit steps: asked for as 16-bit or as 8-bit it is the same */
            ctab.colors[i].red = (unsigned short)((((unsigned long)c[0] * 65535UL + 500) / 1000 >> 8) * 257);
            ctab.colors[i].green = (unsigned short)((((unsigned long)c[1] * 65535UL + 500) / 1000 >> 8) * 257);
            ctab.colors[i].blue = (unsigned short)((((unsigned long)c[2] * 65535UL + 500) / 1000 >> 8) * 257);
        }
    }
    memset(sbuf, 0xa5, sizeof(sbuf));
    for (y = 0; y < SH; y++)
        for (x = 0; x < SW; x++)
        {
            idx[y][x] = (x * 3 + y * 5) % 11;
            sbuf[8 + y * pitch + x] = idx[y][x];
        }
    memset(&src, 0, sizeof(src));
    src.magic = 0x6362746dL;
    src.length = sizeof(src);
    src.addr = sbuf + 8;
    src.width = pitch;
    src.bits = 8;
    src.px_format = F_8;
    src.xmax = SW;
    src.ymax = SH;

    src.ctab = &ctab;
    for (s = 1; s <= 3; s++)
    {
        cur_scale = s;
        run_to_memory("8-bit source with the caller's table", &src, sr, SW * s, SH * s, 0, pix_index_table, 0);
    }
    cur_scale = 2;
    run_to_memory("8-bit source, replace", &src, sr, SW * 2, SH * 2, 32, pix_index_table, 0);

    /* an index beyond the table: the call is refused */
    sbuf[8 + 3 * pitch + 4] = 11;
    cur_scale = 1;
    run_to_memory("8-bit source with an index beyond its table draws nothing", &src, sr, SW, SH, 0, pix_index_table, 1);
    sbuf[8 + 3 * pitch + 4] = idx[3][4];
    /* a table with no entries is not a table: the pens of the workstation */
    ctab.no_colors = 0;
    for (y = 0; y < SH; y++)
        for (x = 0; x < SW; x++)
            idx[y][x] = (x * 3 + y * 5) % numcolors;
    for (y = 0; y < SH; y++)
        for (x = 0; x < SW; x++)
            sbuf[8 + y * pitch + x] = idx[y][x];
    cur_scale = 2;
    run_to_memory("8-bit source with an empty table uses the pens", &src, sr, SW * 2, SH * 2, 0, pix_index_pens, 0);
    src.ctab = NULL;
    run_to_memory("8-bit source without a table uses the pens", &src, sr, SW * 2, SH * 2, 0, pix_index_pens, 0);
    cur_scale = 1;
    run_to_memory("8-bit source without a table, 1:1", &src, sr, SW, SH, 0, pix_index_pens, 0);
    /* an index beyond the pens */
    if (numcolors < 256)
    {
        src.ctab = NULL;
        sbuf[8 + 2 * pitch + 2] = (unsigned char)numcolors;
        run_to_memory("8-bit source with an index beyond the pens draws nothing", &src, sr, SW, SH, 0, pix_index_pens, 1);
    }
}

/* (d) the 1-bit source */
static unsigned long raster_fg = 0xe03010, raster_bg = 0x2060d0;
static unsigned char bits1[SH][SW];
static int mode1;

static long pix_mono(int dx, int dy)
{
    int b = bits1[dy / cur_scale][dx / cur_scale];

    if (!b && mode1 == 33)
        return -1;
    return (long)exp_rgb(b ? raster_fg : raster_bg);
}

static void test_mono(void)
{
    GCBITMAP src;
    short sr[4] = { 0, 0, SW - 1, SH - 1 };
    int x, y, s, shift;
    long pitch = 3;
    static const short modes[3] = { 0, 32, 33 };
    int m;
    char text[100];

    set_raster(200, raster_fg);
    set_raster(201, raster_bg);
    for (shift = 0; shift < 8; shift += 5)
    {
        memset(sbuf, 0xa5, sizeof(sbuf));
        for (y = 0; y < SH; y++)
        {
            unsigned char *row = sbuf + 8 + y * pitch;

            row[0] = row[1] = row[2] = 0;
            for (x = 0; x < SW; x++)
            {
                int b = ((x * 5 + y * 3) % 7) < 3;

                bits1[y][x] = b;
                if (b)
                    row[(x + shift) >> 3] |= 0x80 >> ((x + shift) & 7);
            }
        }
        memset(&src, 0, sizeof(src));
        src.magic = 0x6362746dL;
        src.length = sizeof(src);
        src.addr = sbuf + 8;
        src.width = pitch;
        src.bits = 1;
        src.px_format = F_1;
        src.xmin = -shift;                  /* the first pixel (x = 0) is at bit 'shift' */
        src.xmax = -shift + 24;
        src.ymax = SH;
        for (m = 0; m < 3; m++)
            for (s = 1; s <= 2; s++)
            {
                mode1 = modes[m];
                cur_scale = s;
                snprintf(text, sizeof(text), "1-bit source mode %d at %dx, bit offset %d", modes[m], s, shift);
                run_to_memory(text, &src, sr, SW * s, SH * s, modes[m], pix_mono, 0);
            }
    }
}

/* ------------------------------------------------------------------ */
/* (e) the workstation as the destination */

#define AX 16
#define AY 16
#define AW 96
#define AH 64
static unsigned char areaA[16384] __attribute__((aligned(2))), areaB[16384] __attribute__((aligned(2)));

static void grab(unsigned char *buf, long *pitch, short x, short y, int w, int h)
{
    MFDB scr, mem;
    short xy[8];

    memset(&scr, 0, sizeof(scr));
    memset(&mem, 0, sizeof(mem));
    mem.fd_addr = buf;
    mem.fd_w = w;
    mem.fd_h = h;
    mem.fd_wdwidth = (w + 15) / 16;
    mem.fd_nplanes = planes;
    *pitch = line_bytes(mem.fd_wdwidth * 16);
    xy[0] = x;
    xy[1] = y;
    xy[2] = x + w - 1;
    xy[3] = y + h - 1;
    xy[4] = 0;
    xy[5] = 0;
    xy[6] = w - 1;
    xy[7] = h - 1;
    vro_cpyfm(screen, 3, xy, &scr, &mem);
}

static unsigned long draw_src[SH][SW];

static void test_screen(short h, const char *label)
{
    GCBITMAP src;
    short sr[4] = { 0, 0, SW - 1, SH - 1 };
    short dr[4], clip[4];
    long pitchA, pitchB;
    int s, x, y, n, bad;
    char text[120];

    for (n = 0; n < 2; n++)
    {
        build_direct(n ? F_16 : F_32, SW * fmt_bytes(n ? F_16 : F_32), &src);
        for (y = 0; y < SH; y++)
            for (x = 0; x < SW; x++)
                draw_src[y][x] = srgb[y][x];
        for (s = 2; s <= 3; s++)
        {
            int dw = SW * s, dh = SH * s;

            /* the destination rectangle, cut by the clip on every side but the bottom right corner */
            dr[0] = AX + 5;
            dr[1] = AY + 4;
            dr[2] = dr[0] + dw - 1;
            dr[3] = dr[1] + dh - 1;
            clip[0] = dr[0] + 7;
            clip[1] = dr[1] + 3;
            clip[2] = dr[2] - 4;
            clip[3] = dr[3] - 2;
            say("XFER screen grab");
            grab(areaA, &pitchA, AX, AY, AW, AH);
            vs_clip(h, 1, clip);
            say("XFER screen transfer");
            transfer(h, &src, NULL, sr, dr, 0);
            vs_clip(h, 0, clip);
            say("XFER screen grab again");
            grab(areaB, &pitchB, AX, AY, AW, AH);
            say("XFER screen compare");
            bad = 0;
            for (y = 0; y < AH; y++)
                for (x = 0; x < AW; x++)
                {
                    int sx = AX + x, sy = AY + y;
                    unsigned long want = dev_get(areaA, pitchA, x, y);

                    if (sx >= clip[0] && sx <= clip[2] && sy >= clip[1] && sy <= clip[3] &&
                        sx >= dr[0] && sx <= dr[2] && sy >= dr[1] && sy <= dr[3])
                        want = exp_rgb(draw_src[(sy - dr[1]) / s][(sx - dr[0]) / s]);
                    if (dev_get(areaB, pitchB, x, y) != want)
                        bad++;
                }
            snprintf(text, sizeof(text), "%s: clipped to the screen, source %s, %dx (%d pixels differ)",
                     label, n ? "16 bits" : "32 bits", s, bad);
            check(bad == 0, text);
        }
    }
    /* without clipping the whole rectangle is drawn; clipping on with the window of the screen only */
    build_direct(F_32, SW * 4, &src);
    for (y = 0; y < SH; y++)
        for (x = 0; x < SW; x++)
            draw_src[y][x] = srgb[y][x];
    dr[0] = AX + 20;
    dr[1] = AY + 10;
    dr[2] = dr[0] + SW * 2 - 1;
    dr[3] = dr[1] + SH * 2 - 1;
    clip[0] = 0;
    clip[1] = 0;
    clip[2] = 4000;
    clip[3] = 4000;
    grab(areaA, &pitchA, AX, AY, AW, AH);
    vs_clip(h, 0, clip);
    transfer(h, &src, NULL, sr, dr, 0);
    grab(areaB, &pitchB, AX, AY, AW, AH);
    bad = 0;
    for (y = 0; y < AH; y++)
        for (x = 0; x < AW; x++)
        {
            int sx = AX + x, sy = AY + y;
            unsigned long want = dev_get(areaA, pitchA, x, y);

            if (sx >= dr[0] && sx <= dr[2] && sy >= dr[1] && sy <= dr[3])
                want = exp_rgb(draw_src[(sy - dr[1]) / 2][(sx - dr[0]) / 2]);
            if (dev_get(areaB, pitchB, x, y) != want)
                bad++;
        }
    snprintf(text, sizeof(text), "%s: no clipping (%d pixels differ)", label, bad);
    check(bad == 0, text);
}

/* a 1-bit source onto the screen, transparent: only the 1 bits arrive */
static void test_screen_mono(short h, const char *label)
{
    GCBITMAP src;
    short sr[4] = { 0, 0, SW - 1, SH - 1 }, dr[4], clip[4];
    long pitchA, pitchB;
    int x, y, bad, tr;
    char text[120];

    memset(sbuf, 0xa5, sizeof(sbuf));
    for (y = 0; y < SH; y++)
    {
        unsigned char *row = sbuf + 8 + y * 2;

        row[0] = row[1] = 0;
        for (x = 0; x < SW; x++)
        {
            bits1[y][x] = ((x + 2 * y) % 5) < 2;
            if (bits1[y][x])
                row[x >> 3] |= 0x80 >> (x & 7);
        }
    }
    memset(&src, 0, sizeof(src));
    src.magic = 0x6362746dL;
    src.length = sizeof(src);
    src.addr = sbuf + 8;
    src.width = 2;
    src.bits = 1;
    src.px_format = F_1;
    src.xmax = SW;
    src.ymax = SH;
    set_raster(200, raster_fg);
    set_raster(201, raster_bg);
    for (tr = 0; tr < 2; tr++)
    {
        dr[0] = AX + 3;
        dr[1] = AY + 2;
        dr[2] = dr[0] + SW * 3 - 1;
        dr[3] = dr[1] + SH * 3 - 1;
        clip[0] = dr[0] + 4;
        clip[1] = dr[1] + 4;
        clip[2] = dr[2] - 5;
        clip[3] = dr[3] - 3;
        grab(areaA, &pitchA, AX, AY, AW, AH);
        vs_clip(h, 1, clip);
        transfer(h, &src, NULL, sr, dr, tr ? 33 : 0);
        vs_clip(h, 0, clip);
        grab(areaB, &pitchB, AX, AY, AW, AH);
        bad = 0;
        for (y = 0; y < AH; y++)
            for (x = 0; x < AW; x++)
            {
                int sx = AX + x, sy = AY + y;
                unsigned long want = dev_get(areaA, pitchA, x, y);

                if (sx >= clip[0] && sx <= clip[2] && sy >= clip[1] && sy <= clip[3])
                {
                    int b = bits1[(sy - dr[1]) / 3][(sx - dr[0]) / 3];

                    if (b)
                        want = exp_rgb(raster_fg);
                    else if (!tr)
                        want = exp_rgb(raster_bg);
                }
                if (dev_get(areaB, pitchB, x, y) != want)
                    bad++;
            }
        snprintf(text, sizeof(text), "%s: 1-bit source onto the screen, %s (%d pixels differ)", label,
                 tr ? "transparent" : "copy", bad);
        check(bad == 0, text);
    }
}

/* ------------------------------------------------------------------ */
/* (f) refusals */

/*
 * one call that must draw nothing into memory: the destination with its
 * guard bytes and the source stay as they were
 */
static void refuse(const char *what, GCBITMAP *src, GCBITMAP *dst, const short *sr, const short *dr, short mode)
{
    static unsigned char ssave[MAXB];
    char text[120];
    int same;

    memcpy(ssave, sbuf, sizeof(sbuf));
    transfer(screen, src, dst, sr, dr, mode);
    same = memcmp(dbuf, wbuf, sizeof(dbuf)) == 0 && memcmp(ssave, sbuf, sizeof(sbuf)) == 0;
    snprintf(text, sizeof(text), "refused: %s", what);
    check(same, text);
}

static void test_refusals(short h)
{
    GCBITMAP src, src2, dst, dst2;
    short sr[4] = { 0, 0, SW - 1, SH - 1 }, dr[4] = { 3, 3, 3 + SW - 1, 3 + SH - 1 }, r2[4];
    long pitch = line_bytes(SW + 6) + 2, pitchA, pitchB;
    int bad, x, y;

    build_direct(F_32, SW * 4, &src);
    memset(dbuf, 0xa5, sizeof(dbuf));
    mk_dst(&dst, dbuf + 16, pitch, SW + 6, SH + 6);
    memcpy(wbuf, dbuf, sizeof(dbuf));

    refuse("no source", NULL, &dst, sr, dr, 0);
    dst2 = dst;
    dst2.px_format = F_32;
    refuse("a destination of another format", &src, &dst2, sr, dr, 0);
    dst2 = dst;
    dst2.px_format = fmt_screen ^ 0x01000000UL;
    refuse("a destination of a format that is not the screen's", &src, &dst2, sr, dr, 0);
    src2 = src;
    src2.bits = 16;
    refuse("bits that are not the source's", &src2, &dst, sr, dr, 0);
    dst2 = dst;
    dst2.bits = planes == 16 ? 8 : 16;
    refuse("bits that are not the destination's", &src, &dst2, sr, dr, 0);
    {
        short sr2[4] = { 0, 0, SW, SH - 1 };

        refuse("a source rectangle outside the bitmap", &src, &dst, sr2, dr, 0);
    }
    memcpy(r2, dr, sizeof(r2));
    r2[2] = SW + 6;
    refuse("a destination rectangle outside the bitmap", &src, &dst, sr, r2, 0);
    memcpy(r2, dr, sizeof(r2));
    r2[1] = -1;
    refuse("a destination rectangle above the bitmap", &src, &dst, sr, r2, 0);
    src2 = src;
    src2.width = SW * 4 - 2;
    refuse("a source line too short", &src2, &dst, sr, dr, 0);
    src2 = src;
    src2.addr = sbuf + 8 + 1;
    refuse("a 32-bit source at an odd address", &src2, &dst, sr, dr, 0);
    dst2 = dst;
    dst2.width = line_bytes(SW + 6) - 2;
    refuse("a destination line too short", &src, &dst2, sr, dr, 0);
    refuse("a mode that does not exist", &src, &dst, sr, dr, 7);
    /* 65, not 64: 64 is T_BLEND, which blends direct sources onto 16 bits where it is advertised */
    refuse("a mode that does not exist (65)", &src, &dst, sr, dr, 65);
    refuse("transparent for a source that is not 1-bit", &src, &dst, sr, dr, 33);
    src2 = src;
    src2.px_format = 0x03421821UL;
    refuse("a source format that is not one", &src2, &dst, sr, dr, 0);
    src2 = src;
    src2.px_format = F_32 | F_REV;
    refuse("a 32-bit source that is reversed", &src2, &dst, sr, dr, 0);
    memcpy(r2, dr, sizeof(r2));
    r2[0] = 20;
    r2[2] = 10;
    refuse("an upside-down destination rectangle", &src, &dst, sr, r2, 0);
    {
        short sr2[4] = { 9, 0, 3, SH - 1 };

        refuse("an upside-down source rectangle", &src, &dst, sr2, dr, 0);
    }
    /* overlapping memory: the destination inside the source, and the source inside the destination */
    dst2 = dst;
    dst2.addr = sbuf + 8 + 16;
    refuse("a destination inside the source", &src, &dst2, sr, dr, 0);
    src2 = src;
    src2.addr = dbuf + 16 + 4;
    refuse("a source inside the destination", &src2, &dst, sr, dr, 0);

    /* the screen is as it was */
    grab(areaA, &pitchA, AX, AY, AW, AH);
    dr[0] = AX + 4;
    dr[1] = AY + 4;
    dr[2] = AX + 4 + SW - 1;
    dr[3] = AY + 4 + SH - 1;
    transfer(screen, NULL, NULL, sr, dr, 0);
    src2 = src;
    src2.px_format = 0x03421821UL;
    transfer(screen, &src2, NULL, sr, dr, 0);
    src2 = src;
    src2.bits = 8;
    transfer(screen, &src2, NULL, sr, dr, 0);
    transfer(screen, &src, NULL, sr, dr, 9);
    transfer(screen, &src, NULL, sr, dr, 33);
    src2 = src;
    src2.width = 4;
    transfer(screen, &src2, NULL, sr, dr, 0);
    {
        short r3[4] = { AX + 40, AY + 4, AX + 30, AY + 10 };

        transfer(screen, &src, NULL, sr, r3, 0);
    }
    /* memory that is the screen: the workstation is the screen, not a bitmap in memory */
    mk_dst(&dst2, (unsigned char *)Logbase(), line_bytes(1024), 1024, 128);
    {
        short r4[4] = { AX + 4, AY + 4, AX + 4 + SW - 1, AY + 4 + SH - 1 };

        transfer(screen, &src, &dst2, sr, r4, 0);
    }
    grab(areaB, &pitchB, AX, AY, AW, AH);
    bad = 0;
    for (y = 0; y < AH; y++)
        for (x = 0; x < AW; x++)
            if (dev_get(areaA, pitchA, x, y) != dev_get(areaB, pitchB, x, y))
                bad++;
    check(bad == 0, "refused calls (a destination that is the screen's own memory too) leave the screen as it was");
    (void)h;
}

/* ------------------------------------------------------------------ */
/* (g) bit 15 of the mode in vro_cpyfm and vrt_cpyfm */

static unsigned long mfdb_pixel(MFDB *b, int x, int y)
{
    unsigned short *words = b->fd_addr;
    unsigned long value = 0;
    int p;

    if (planes == 16 && !b->fd_stand && b->fd_nplanes == 16)
        return words[y * b->fd_wdwidth * 16 + x];
    for (p = 0; p < b->fd_nplanes; p++)
    {
        long at = b->fd_stand ? ((long)p * b->fd_h + y) * b->fd_wdwidth + x / 16 :
                  ((long)y * b->fd_wdwidth + x / 16) * b->fd_nplanes + p;

        if (words[at] & (0x8000U >> (x & 15)))
            value |= 1UL << ((b->fd_stand && planes == 16) ? 15 - p : p);   /* as vr_trnfm: plane 0 is bit 15 */
    }
    return value;
}

static void mfdb_put(MFDB *b, int x, int y, unsigned long value)
{
    unsigned short *words = b->fd_addr;
    int p;

    if (planes == 16 && !b->fd_stand && b->fd_nplanes == 16)
    {
        words[y * b->fd_wdwidth * 16 + x] = value;
        return;
    }
    for (p = 0; p < b->fd_nplanes; p++)
    {
        long at = b->fd_stand ? ((long)p * b->fd_h + y) * b->fd_wdwidth + x / 16 :
                  ((long)y * b->fd_wdwidth + x / 16) * b->fd_nplanes + p;

        if (value & (1UL << ((b->fd_stand && planes == 16) ? 15 - p : p)))
            words[at] |= 0x8000U >> (x & 15);
    }
}

/* a 16-plane standard form through a scaled copy is what vr_trnfm and an unscaled copy make of it */
static void std16_test(short h)
{
    MFDB std, dev, a, b;
    static unsigned short sw_[16 * 8], dv[16 * 8], ra[16 * 8 * 4], rb[16 * 8 * 4];
    short xy[8] = { 0, 0, 15, 7, 0, 0, 15, 7 }, xy2[8] = { 0, 0, 15, 7, 0, 0, 31, 15 };
    int i, mode;

    if (planes != 16)
        return;
    for (i = 0; i < 16 * 8; i++)
        sw_[i] = (unsigned short)(i * 40503u + 12345u);
    memset(&std, 0, sizeof(std));
    std.fd_addr = sw_;
    std.fd_w = 16;
    std.fd_h = 8;
    std.fd_wdwidth = 1;
    std.fd_stand = 1;
    std.fd_nplanes = 16;
    dev = std;
    dev.fd_addr = dv;
    dev.fd_stand = 0;
    vr_trnfm(h, &std, &dev);
    for (mode = 0; mode < 2; mode++)
    {
        short *p = mode ? xy2 : xy;

        memset(ra, 0x11, sizeof(ra));
        memset(rb, 0x11, sizeof(rb));
        a = dev;
        a.fd_addr = ra;
        a.fd_w = 32;
        a.fd_h = 16;
        a.fd_wdwidth = 2;
        b = a;
        b.fd_addr = rb;
        vro_cpyfm(h, 3 | 0x8000, p, &std, &a);
        vro_cpyfm(h, 3 | 0x8000, p, &dev, &b);
        check(memcmp(ra, rb, sizeof(ra)) == 0, mode ? "a 16-plane standard form scaled 2x is what vr_trnfm makes of it"
                                                    : "a 16-plane standard form copied 1:1 is what vr_trnfm makes of it");
    }
    memset(ra, 0x11, sizeof(ra));
    a = dev;
    a.fd_addr = ra;
    a.fd_w = 32;
    a.fd_h = 16;
    a.fd_wdwidth = 2;
    vro_cpyfm(h, 3, xy, &dev, &a);
    {
        int x, y, bad = 0;

        for (y = 0; y < 8; y++)
            for (x = 0; x < 16; x++)
                if (ra[y * 32 + x] != dv[y * 16 + x])
                    bad++;
        check(bad == 0, "the unscaled copy of the transformed form is the transformed form");
    }
}

static void legacy_test(short h)
{
    MFDB src, scaled, actual, expected, scr;
    short full[8] = { 0, 0, 63, 39, 0, 0, 63, 39 };
    short clip[4] = { 11, 12, 49, 30 }, pens[2] = { 1, 0 };
    long bytes = 4L * 2 * planes * 40, sbytes = 2L * 2 * planes * 17 + 64;
    int expand, standard, to_screen, mode, shrink, x, y;
    unsigned short *sa, *sc, *ac, *ex;

    memset(&src, 0, sizeof(src));
    memset(&scaled, 0, sizeof(scaled));
    memset(&actual, 0, sizeof(actual));
    memset(&scr, 0, sizeof(scr));
    src.fd_w = 32;
    src.fd_wdwidth = 2;
    src.fd_h = 17;
    scaled.fd_w = actual.fd_w = 64;
    scaled.fd_wdwidth = actual.fd_wdwidth = 4;
    scaled.fd_h = actual.fd_h = 40;
    actual.fd_nplanes = planes;
    sa = malloc(sbytes);
    sc = malloc(bytes + 64);
    ac = malloc(bytes + 64);
    ex = malloc(bytes + 64);
    check(sa && sc && ac && ex, "legacy allocations");
    src.fd_addr = sa;
    scaled.fd_addr = sc;
    actual.fd_addr = ac;
    expected = actual;
    expected.fd_addr = ex;
    for (expand = 0; expand < 2; expand++)
        for (standard = 0; standard < 2; standard++)
            for (to_screen = 0; to_screen < 2; to_screen++)
                for (shrink = 0; shrink < 2; shrink++)
                    for (mode = expand ? 1 : 0; mode <= (expand ? 4 : 15); mode++)
                    {
                        int dw = shrink ? 11 : 53, dh = shrink ? 9 : 31;
                        short xy[8] = { 1, 2, 23, 16, 7, 8, 7, 8 };
                        short refxy[8] = { 0, 0, 0, 0, 7, 8, 7, 8 };
                        int bad;

                        xy[6] = 7 + dw - 1;
                        xy[7] = 8 + dh - 1;
                        refxy[2] = dw - 1;
                        refxy[3] = dh - 1;
                        refxy[6] = 7 + dw - 1;
                        refxy[7] = 8 + dh - 1;
                        src.fd_nplanes = scaled.fd_nplanes = expand ? 1 : planes;
                        src.fd_stand = standard;
                        memset(sa, 0, sbytes);
                        memset(sc, 0, bytes + 64);
                        for (y = 0; y < 17; y++)
                            for (x = 0; x < 32; x++)
                                mfdb_put(&src, x, y, x * 937UL + y * 317UL);
                        for (y = 0; y < dh; y++)
                            for (x = 0; x < dw; x++)
                                mfdb_put(&scaled, x, y, mfdb_pixel(&src, 1 + x * 23 / dw, 2 + y * 15 / dh));
                        memset(ac, 0xa7, bytes + 64);
                        memset(ex, 0xa7, bytes + 64);
                        vs_clip(h, 0, clip);
                        if (to_screen)
                        {
                            vro_cpyfm(h, 3, full, &actual, &scr);
                            refxy[0] = clip[0] - 7;
                            refxy[1] = clip[1] - 8;
                            refxy[4] = clip[0];
                            refxy[5] = clip[1];
                            if (refxy[6] > clip[2])
                            {
                                refxy[2] -= refxy[6] - clip[2];
                                refxy[6] = clip[2];
                            }
                            if (refxy[7] > clip[3])
                            {
                                refxy[3] -= refxy[7] - clip[3];
                                refxy[7] = clip[3];
                            }
                        }
                        if (expand)
                            vrt_cpyfm(h, mode, refxy, &scaled, &expected, pens);
                        else
                            vro_cpyfm(h, mode, refxy, &scaled, &expected);
                        /* memory output ignores clipping, screen output keeps the phase */
                        vs_clip(h, 1, clip);
                        if (expand)
                            vrt_cpyfm(h, mode | 0x8000, xy, &src, to_screen ? &scr : &actual, pens);
                        else
                            vro_cpyfm(h, mode | 0x8000, xy, &src, to_screen ? &scr : &actual);
                        vs_clip(h, 0, clip);
                        if (to_screen)
                            vro_cpyfm(h, 3, full, &scr, &actual);
                        bad = 0;
                        for (x = 0; x < 64; x++)
                            if (((unsigned char *)ac)[bytes + x] != 0xa7 ||
                                ((unsigned char *)ex)[bytes + x] != 0xa7 ||
                                ((unsigned char *)sa)[2L * 2 * planes * 17 + x] != 0 ||
                                ((unsigned char *)sc)[bytes + x] != 0)
                                bad = 1;
                        if (bad || memcmp(ac, ex, bytes))
                        {
                            char message[160];

                            snprintf(message, sizeof(message),
                                     "bit 15: depth=%d expand=%d standard=%d screen=%d shrink=%d mode=%d %s",
                                     planes, expand, standard, to_screen, shrink, mode, bad ? "(guard)" : "(pixels)");
                            check(0, message);
                        }
                        else
                            checks++;
                    }
    free(sa);
    free(sc);
    free(ac);
    free(ex);
}

/* refusals of bit 15: a rectangle outside, a source of the wrong depth, a screen source, upside down */
static void legacy_refusals(short h)
{
    MFDB src, dst;
    short xy[8] = { 0, 0, 15, 7, 0, 0, 31, 15 };
    long sb = line_bytes(16) * 8, db = line_bytes(32) * 16;
    unsigned char *sa = malloc(sb + 64), *da = malloc(db + 64), *ref = malloc(db + 64);
    int round;

    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.fd_addr = sa;
    src.fd_w = 16;
    src.fd_h = 8;
    src.fd_wdwidth = 1;
    src.fd_nplanes = planes;
    dst.fd_addr = da;
    dst.fd_w = 32;
    dst.fd_h = 16;
    dst.fd_wdwidth = 2;
    dst.fd_nplanes = planes;
    memset(sa, 0x3c, sb + 64);
    memset(da, 0xa5, db + 64);
    memset(ref, 0xa5, db + 64);
    for (round = 0; round < 5; round++)
    {
        short pts[8];

        memcpy(pts, xy, sizeof(pts));
        switch (round)
        {
        case 0:
            pts[2] = 16;                    /* the source rectangle is outside */
            break;
        case 1:
            pts[6] = 32;                    /* the destination rectangle is outside */
            break;
        case 2:
            src.fd_nplanes = planes == 16 ? 8 : 16;
            break;
        case 3:
            src.fd_addr = NULL;             /* a screen source */
            break;
        default:
            pts[0] = 9;
            pts[2] = 3;                     /* upside down */
            break;
        }
        vro_cpyfm(h, 3 | 0x8000, pts, &src, &dst);
        src.fd_nplanes = planes;
        src.fd_addr = sa;
        check(memcmp(da, ref, db + 64) == 0, "a refused scaled copy draws nothing");
    }
    free(sa);
    free(da);
    free(ref);
}

/* ------------------------------------------------------------------ */
/* (h) into the bitmap of a bitmap workstation */

static void test_bitmap(short base)
{
    GCBITMAP bm, src;
    short hb, sr[4] = { 0, 0, SW - 1, SH - 1 }, dr[4] = { 19, 7, 19 + SW * 2 - 1, 7 + SH * 2 - 1 };
    short clip[4] = { 22, 9, 40, 18 };
    long pitch, size;
    unsigned char *before;
    int x, y, bad, n;
    char text[120];

    memset(&bm, 0, sizeof(bm));
    bm.xmax = 64;
    bm.ymax = 32;
    hb = v_open_bm(base, &bm, 0, 0, 0, 0);
    check(hb > 0, "v_open_bm");
    if (hb <= 0 || !bm.addr)
        return;
    pitch = bm.width;
    size = pitch * 32;
    before = malloc(size);
    check(before != NULL, "snapshot memory");
    for (n = 0; n < 2; n++)
    {
        build_direct(n ? F_16 : F_32, SW * fmt_bytes(n ? F_16 : F_32), &src);
        for (y = 0; y < SH; y++)
            for (x = 0; x < SW; x++)
                draw_src[y][x] = srgb[y][x];
        memcpy(before, bm.addr, size);
        if (n)
            vs_clip(hb, 1, clip);
        transfer(hb, &src, NULL, sr, dr, 0);
        bad = 0;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 64; x++)
            {
                unsigned long want = dev_get(before, pitch, x, y);

                if (x >= dr[0] && x <= dr[2] && y >= dr[1] && y <= dr[3] &&
                    (!n || (x >= clip[0] && x <= clip[2] && y >= clip[1] && y <= clip[3])))
                    want = exp_rgb(draw_src[(y - dr[1]) / 2][(x - dr[0]) / 2]);
                if (dev_get(bm.addr, pitch, x, y) != want)
                    bad++;
            }
        snprintf(text, sizeof(text), "into a bitmap workstation%s (%d pixels differ)", n ? " with a clip" : "", bad);
        check(bad == 0, text);
        if (n)
            vs_clip(hb, 0, clip);
    }
    /* a scaled vro_cpyfm into the bitmap, clipping off: the bitmap's edge is the limit */
    {
        MFDB sm, dm;
        short xy[8] = { 0, 0, 15, 7, 50, 20, 79, 35 };
        unsigned char *msrc = malloc(line_bytes(16) * 8 + 8);

        memset(&sm, 0, sizeof(sm));
        memset(&dm, 0, sizeof(dm));
        memset(msrc, 0x69, line_bytes(16) * 8 + 8);
        sm.fd_addr = msrc;
        sm.fd_w = 16;
        sm.fd_h = 8;
        sm.fd_wdwidth = 1;
        sm.fd_nplanes = planes;
        memcpy(before, bm.addr, size);
        vro_cpyfm(hb, 3 | 0x8000, xy, &sm, &dm);
        bad = 0;
        for (y = 0; y < 32; y++)
            for (x = 0; x < 64; x++)
            {
                unsigned long want = dev_get(before, pitch, x, y);

                if (x >= 50 && y >= 20)
                    want = mfdb_pixel(&sm, (x - 50) * 16 / 30, (y - 20) * 8 / 16);
                if (dev_get(bm.addr, pitch, x, y) != want)
                    bad++;
            }
        snprintf(text, sizeof(text), "a scaled vro_cpyfm into a bitmap workstation, cut by its edge (%d pixels differ)", bad);
        check(bad == 0, text);
        free(msrc);
    }
    free(before);
    v_clsbm(hb);
}

/* ------------------------------------------------------------------ */

static short open_vwk(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, chh, bw, bh, h;

    h = graf_handle(&cw, &chh, &bw, &bh);
    v_opnvwk(work_in, &h, work_out);
    return h;
}

int main(void)
{
    short work_out[57];
    unsigned long expected, got;
    int i;
    char text[120];

    say("XFER start");
    check(appl_init() >= 0, "AES application registration");
    screen = open_vwk();
    check(screen > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(screen, 1, work_out);
    planes = work_out[4];
    vq_extnd(screen, 0, work_out);
    numcolors = work_out[13];
    if (numcolors > 256)
        numcolors = 256;
    snprintf(text, sizeof(text), "XFER %d planes, %d colours", planes, numcolors);
    say(text);
    check(planes == 1 || planes == 2 || planes == 4 || planes == 8 || planes == 16, "a screen of 1, 2, 4, 8 or 16 planes");
    if (failed)
        goto out;
    for (i = 0; i < numcolors; i++)
        vq_color(screen, i, 0, pen_rgb[i]);

    /* (a) */
    expected = planes == 16 ? F_16 : planes == 1 ? F_1 : 0x01000000UL | ((unsigned long)planes << 8) | planes;
    fmt_screen = expected;
    vdi_raw(screen, 204, 3, 0, 0);
    got = ((unsigned long)(unsigned short)intout[2] << 16) | (unsigned short)intout[3];
    check(intout[0] == 0 && intout[1] == 1 && got == expected, "vq_px_format says the format of the screen");
    vq_extnd(screen, 1, work_out);
    check((work_out[30] & 3) == 0, "work_out[30] bits 0 and 1 stay clear: the raster calls are not advertised as complete");

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    test_direct();
    say("XFER direct sources done");
    test_indexed();
    say("XFER indexed sources done");
    test_mono();
    say("XFER 1-bit sources done");
    test_screen(screen, "screen");
    test_screen_mono(screen, "screen");
    say("XFER screen destination done");
    test_refusals(screen);
    say("XFER refusals done");
    legacy_test(screen);
    std16_test(screen);
    legacy_refusals(screen);
    say("XFER bit 15 done");
    test_bitmap(screen);
    say("XFER bitmap done");
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);

out:
    if (screen > 0)
        v_clsvwk(screen);
    appl_exit();
    if (!failed)
    {
        snprintf(text, sizeof(text), "XFER %ld checks", checks);
        say(text);
        say("FVDI XFER GUEST PASS");
    }
    return failed;
}
