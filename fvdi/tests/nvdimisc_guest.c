/*
 * nvdimisc_guest.c - v_setrgb, vr_clip_rects_by_dst/_by_src and their
 * 32-bit forms, v_bez_qual, and v_color2value / v_value2color /
 * v_color2nearest.
 *
 * The colour conversions are checked against the screen itself: a pixel
 * drawn into an off-screen bitmap must hold the value v_color2value gives
 * for its colour.  Needs a 16-bit (or deeper) screen for that part.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI NVDIMISC GUEST PASS" or "FVDI NVDIMISC FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

typedef struct { unsigned short reserved, red, green, blue; } RGB16;
typedef struct {
    long magic, length, format, reserved;
    unsigned char *addr;
    long width, bits;
    unsigned long px_format;
    long xmin, ymin, xmax, ymax;
    void *ctab, *itab;
    long reserved0, reserved1;
} GCBITMAP;

void v_setrgb(short handle, short type, short r, short g, short b);
void v_bez(short handle, short count, short *xyarr, char *bezarr,
           short *extent, short *totpts, short *totmoves);
short v_bez_qual(short handle, short percent, short *actual);
unsigned long v_color2value(short handle, long color_space, RGB16 *color);
long v_value2color(short handle, unsigned long value, RGB16 *color);
long v_color2nearest(short handle, long color_space, RGB16 *color, RGB16 *nearest);
long vqf_fg_color(short handle, RGB16 *color);
short vsf_fg_color(short handle, long color_space, RGB16 *color);
short v_open_bm(short base_handle, GCBITMAP *bitmap, short color_flags,
                short unit_flags, short pixel_width, short pixel_height);
void v_clsbm(short handle);

static short contrl[12], intin[16], ptsin[32], intout[16], ptsout[32];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
static int failed;
static short handle;

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

    snprintf(line, sizeof(line), "NVDIMISC %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI NVDIMISC FAIL: %s", what);
        say(line);
    }
}

static void vdi_raw(short opcode, short sub, short nptsin)
{
    contrl[0] = opcode;
    contrl[1] = nptsin;
    contrl[3] = 0;
    contrl[5] = sub;
    contrl[6] = handle;
    __asm__ __volatile__(
        "move.l %0,%%d1\n\t"
        "move.w #115,%%d0\n\t"
        "trap #2"
        :
        : "g"(pblock)
        : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

/* 16-bit form: clip, src, dst in; clipped src and dst out; result */
static short clip16(short sub, const short *clip, const short *src, const short *dst, short *out_src, short *out_dst)
{
    memcpy(ptsin, clip, 8);
    memcpy(ptsin + 4, src, 8);
    memcpy(ptsin + 8, dst, 8);
    intout[0] = 0x7777;
    vdi_raw(171, sub, 6);
    memcpy(out_src, ptsout, 8);
    memcpy(out_dst, ptsout + 4, 8);
    return intout[0];
}

static short clip32(short sub, const long *clip, const long *src, const long *dst, long *out_src, long *out_dst)
{
    memcpy(ptsin, clip, 16);
    memcpy(ptsin + 8, src, 16);
    memcpy(ptsin + 16, dst, 16);
    intout[0] = 0x7777;
    vdi_raw(171, sub, 12);
    memcpy(out_src, ptsout, 16);
    memcpy(out_dst, ptsout + 8, 16);
    return intout[0];
}

static int same4(const short *a, short a0, short a1, short a2, short a3)
{
    return a[0] == a0 && a[1] == a1 && a[2] == a2 && a[3] == a3;
}

static int same4l(const long *a, long a0, long a1, long a2, long a3)
{
    return a[0] == a0 && a[1] == a1 && a[2] == a2 && a[3] == a3;
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, out_s[4], out_d[4], q0, q50, q100, actual, bm_h;
    long out_sl[4], out_dl[4], space;
    RGB16 c, n, n2;
    unsigned long v, v_red;
    GCBITMAP bm;
    char line[200];

    say("NVDIMISC start");
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;

    /* v_setrgb: fill colour from tenths of a percent */
    v_setrgb(handle, 1, 1000, 0, 500);
    vqf_fg_color(handle, &c);
    snprintf(line, sizeof(line), "NVDIMISC fill after v_setrgb: %04x %04x %04x", c.red, c.green, c.blue);
    say(line);
    vq_extnd(handle, 1, work_out);
    if (work_out[4] >= 16)
        check(c.red == 0xffff && c.green == 0 && c.blue >= 0x7f00 && c.blue <= 0x8100,
              "v_setrgb sets the fill colour");
    else
    {
        /* A screen with a colour table gives the nearest pen it has */
        n.reserved = 0;
        n.red = 0xffff;
        n.green = 0;
        n.blue = 0x8000;
        v_color2nearest(handle, 1, &n, &n2);
        snprintf(line, sizeof(line), "NVDIMISC nearest pen %04x %04x %04x", n2.red, n2.green, n2.blue);
        say(line);
        check(c.red == n2.red && c.green == n2.green && c.blue == n2.blue,
              "v_setrgb sets the fill colour to the nearest pen");
    }

    /* vr_clip_rects, 16 bits */
    {
        short clip[4] = { 10, 10, 50, 50 }, r100[4] = { 0, 0, 99, 99 }, r50[4] = { 0, 0, 49, 49 };
        short clipb[4] = { 10, 10, 59, 59 }, far[4] = { 200, 200, 300, 300 };

        check(clip16(0, clip, r100, r100, out_s, out_d) == 1 &&
              same4(out_d, 10, 10, 50, 50) && same4(out_s, 10, 10, 50, 50),
              "by_dst, same size: both clipped alike");
        check(clip16(0, clipb, r50, r100, out_s, out_d) == 1 &&
              same4(out_d, 10, 10, 59, 59) && same4(out_s, 5, 5, 29, 29),
              "by_dst, half-size source: source clipped in proportion");
        snprintf(line, sizeof(line), "NVDIMISC scaled by_dst src %d,%d-%d,%d", out_s[0], out_s[1], out_s[2], out_s[3]);
        say(line);
        check(clip16(1, clipb, r100, r50, out_s, out_d) == 1 &&
              same4(out_s, 10, 10, 59, 59) && same4(out_d, 5, 5, 29, 29),
              "by_src, half-size destination: destination clipped in proportion");
        check(clip16(0, far, r100, r100, out_s, out_d) == 0, "by_dst: nothing left is 0");
    }

    /* vr_clip_rects, 32 bits */
    {
        long clip[4] = { 100000L, 0, 150000L, 10 }, src[4] = { 0, 0, 199999L, 99 }, dst[4] = { 0, 0, 199999L, 99 };

        check(clip32(2, clip, src, dst, out_sl, out_dl) == 1 &&
              same4l(out_dl, 100000L, 0, 150000L, 10) && same4l(out_sl, 100000L, 0, 150000L, 10),
              "32-bit by_dst beyond 16 bits");
        check(clip32(3, clip, src, dst, out_sl, out_dl) == 1 &&
              same4l(out_sl, 100000L, 0, 150000L, 10) && same4l(out_dl, 100000L, 0, 150000L, 10),
              "32-bit by_src beyond 16 bits");
    }

    vq_extnd(handle, 1, work_out);
    check((work_out[30] & 4) != 0, "vq_extnd says vr_clip_rects exists");

    /* v_bez_qual */
    q100 = v_bez_qual(handle, 100, &actual);
    q50 = v_bez_qual(handle, 50, &actual);
    q0 = v_bez_qual(handle, 0, &actual);
    snprintf(line, sizeof(line), "NVDIMISC v_bez_qual 100 -> %d, 50 -> %d, 0 -> %d", q100, q50, q0);
    say(line);
    check(q100 == 100 && q0 == 0 && q50 > 0 && q50 < 100, "v_bez_qual maps 0..100 onto what fVDI can do");
    {
        short pts[8] = { 100, 100, 150, 50, 200, 150, 250, 100 };
        char flags[4] = { 1, 0, 0, 0 };
        short extent[4], totpts, totmoves;

        v_bez(handle, 4, pts, flags, extent, &totpts, &totmoves);
        snprintf(line, sizeof(line), "NVDIMISC coarse bezier: %d points", totpts);
        say(line);
        v_bez_qual(handle, 100, &actual);
        v_bez(handle, 4, pts, flags, extent, &totpts, &totmoves);
        check(1, "Beziers drawn at the coarsest and the finest quality");
    }

    /* Colour conversions */
    vq_extnd(handle, 1, work_out);
    if (work_out[4] >= 16)
    {
        c.reserved = 0; c.red = 0xffff; c.green = 0; c.blue = 0;
        v_red = v_color2value(handle, 1, &c);
        space = v_value2color(handle, v_red, &n);
        snprintf(line, sizeof(line), "NVDIMISC red -> value %08lx -> %ld %04x %04x %04x",
                 v_red, space, n.red, n.green, n.blue);
        say(line);
        check(v_red != 0, "v_color2value of red is not zero");
        check(space == 1 && n.red == 0xffff && n.green == 0 && n.blue == 0, "v_value2color of it is red again");

        c.red = 0x1234; c.green = 0x5678; c.blue = 0x9abc;
        v_color2nearest(handle, 1, &c, &n);
        v_color2nearest(handle, 1, &n, &n2);
        v = v_color2value(handle, 1, &c);
        snprintf(line, sizeof(line), "NVDIMISC nearest %04x %04x %04x -> %04x %04x %04x, value %08lx",
                 c.red, c.green, c.blue, n.red, n.green, n.blue, v);
        say(line);
        check(!memcmp(&n, &n2, sizeof(n)), "the nearest colour is its own nearest");
        check(v_color2value(handle, 1, &n) == v, "the nearest colour has the same pixel value");
        /* within one step of a 5-bit component (0x0842) */
        check(n.red - c.red <= 0x842 && c.red - n.red <= 0x842 &&
              n.green - c.green <= 0x842 && c.green - n.green <= 0x842 &&
              n.blue - c.blue <= 0x842 && c.blue - n.blue <= 0x842, "and is within a step of it");

        /* Against the screen: draw red into a bitmap, read the pixel */
        memset(&bm, 0, sizeof(bm));
        bm.xmax = 32;
        bm.ymax = 8;
        bm_h = v_open_bm(handle, &bm, 1, 0, 0, 0);
        check(bm_h > 0, "a bitmap to draw in");
        if (bm_h > 0)
        {
            short xy[4] = { 0, 0, 7, 7 };

            c.red = 0xffff; c.green = 0; c.blue = 0;
            vsf_fg_color(bm_h, 1, &c);
            vsf_interior(bm_h, 1);
            vsf_perimeter(bm_h, 0);
            vswr_mode(bm_h, 1);
            v_bar(bm_h, xy);
            v = *(unsigned short *)(bm.addr + 4 * bm.width + 4 * 2);
            snprintf(line, sizeof(line), "NVDIMISC red drawn as %04lx, v_color2value says %04lx", v, v_red);
            say(line);
            check(v == v_red, "v_color2value matches what drawing red puts in memory");
            check(v_value2color(handle, v, &n) == 1 && n.red == 0xffff && n.green == 0 && n.blue == 0,
                  "v_value2color reads the drawn pixel as red");
            v_clsbm(bm_h);
        }
    } else
        say("NVDIMISC screen under 16 bits - colour conversion checks skipped");

out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI NVDIMISC GUEST PASS");
    return failed;
}
