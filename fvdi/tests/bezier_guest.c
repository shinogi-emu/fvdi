/*
 * bezier_guest.c - v_bez, v_bez_fill, v_bez_on/off and v_bez_qual on
 * EmuTOS's VDI, which makes its curves as fVDI does (engine/bezier.c).
 *
 * The points expected are made here by fVDI's own subdivision (bez_depth
 * and do_bez4, copied below), and checked independently against the cubic
 * itself: each point lies within 1.5 pixels of the curve, the ends are the
 * end points.  The pixels drawn are compared with v_pline / v_fillarea of
 * the expected points, read back from the screen.
 *
 * (a) work_out[28] bit 1; v_bez_on answers 7, v_bez_off nothing;
 *     v_bez_qual 100, 50, 0 -> 100, 44, 0 (fVDI's depth scale)
 * (b) one curve: the point count, the extent, no moves, the points on the
 *     curve; the pixels of v_pline of them: solid, dashed, wide, XOR
 * (c) curves, a line and a move: two polylines, intout[1] = 1
 * (d) v_bez_fill: one curve as v_fillarea of its points, perimeter off and
 *     on; two curves apart (a move) as two fills; a ring (a move inside)
 *     leaves its hole
 * (e) the flags are read only as far as contrl[3] reaches; a curve without
 *     its four points is a polyline; intin empty: v_pline as ever;
 *     v_pline without sub-opcode 13 draws Beziers only after v_bez_on
 * (f) clipping: nothing outside the rectangle, inside as the expected points
 *     made for that clip; the coarsest quality; too many curves are made
 *     coarser, never more points than work_out[14]
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI BEZIER GUEST PASS" or "FVDI BEZIER FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static short contrl[12], intin[300], ptsin[2400], intout[64], ptsout[16];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
static int failed;
static short screen, planes, max_vertices;
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
        snprintf(line, sizeof(line), "BEZIER FAILED: %s", what);
        say(line);
        if (!failed)
        {
            failed = 1;
            snprintf(line, sizeof(line), "FVDI BEZIER FAIL: %s", what);
            say(line);
        }
    }
}

static void vdi_raw(short h, short opcode, short sub, short nintin, short nptsin)
{
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
        : "g"(pblock)
        : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

/* v_bez (opcode 6) or v_bez_fill (9) with sub-opcode 13: the flags a byte a point, PC order */
static void bez_call(short opcode, short sub, const short *xy, const unsigned char *flags, int n, int nintin)
{
    int i;

    memset(intin, 0, sizeof(intin));
    for (i = 0; i < n; i++)
        intin[i >> 1] |= (i & 1) ? flags[i] << 8 : flags[i];
    memcpy(ptsin, xy, n * 4);
    vdi_raw(screen, opcode, sub, nintin < 0 ? (n + 1) / 2 : nintin, n);
}

/* v_bez_qual: escape 5, sub-function 99, intin[0] 32, intin[2] the percentage */
static short bez_qual(short percent)
{
    intin[0] = 32;
    intin[1] = 1;
    intin[2] = percent;
    vdi_raw(screen, 5, 99, 3, 0);
    return intout[0];
}

/* ------------------------------------------------------------------ */
/* fVDI's subdivision (engine/bezier.c), the reference */

struct coords { long x, y; };
struct node { struct coords anchor, in, out; short onscreen, level; };

#define HI(v) ((short)((v) >> 16))
#define MINMAX(min, max, v3, v4) { short t_; if (min > max) { t_ = min; min = max; max = t_; } \
    if (v3 > v4) { t_ = v3; v3 = v4; v4 = t_; } if (v4 > max) max = v4; if (v3 < min) min = v3; }

static short ref_depth(const short *pts, short scale)
{
    short base, xdiff, ydiff, t, depth;
    long size;

    base = pts[0];
    xdiff = base - pts[2]; if (xdiff < 0) xdiff = -xdiff;
    t = base - pts[4]; if (t < 0) t = -t; if (t > xdiff) xdiff = t;
    t = base - pts[6]; if (t < 0) t = -t; if (t > xdiff) xdiff = t;
    base = pts[1];
    ydiff = base - pts[3]; if (ydiff < 0) ydiff = -ydiff;
    t = base - pts[5]; if (t < 0) t = -t; if (t > ydiff) ydiff = t;
    t = base - pts[7]; if (t < 0) t = -t; if (t > ydiff) ydiff = t;
    size = (long)xdiff * ydiff;
    if (!(size >>= 4 * 2 + scale - 2))
        return 2;
    for (depth = 3; depth < 7; depth++)
        if (!(size >>= 4))
            return depth;
    return 7;
}

static int ref_bez4(const short *xyin, short depth, short *xyout, const short *clip)
{
    int count = 0;
    struct coords mid;
    struct node bez[8], *c = bez;

    c->onscreen = 0;
    c->level = depth - 1;
    c->anchor.x = ((((long)xyin[6]) << 16) >> 1) + 0x4000;
    c->anchor.y = ((((long)xyin[7]) << 16) >> 1) + 0x4000;
    c->in.x = ((((long)xyin[4]) << 16) >> 1) + 0x4000;
    c->in.y = ((((long)xyin[5]) << 16) >> 1) + 0x4000;
    c->out.x = ((((long)xyin[2]) << 16) >> 1) + 0x4000;
    c->out.y = ((((long)xyin[3]) << 16) >> 1) + 0x4000;
    (c + 1)->anchor.x = ((((long)xyin[0]) << 16) >> 1) + 0x4000;
    (c + 1)->anchor.y = ((((long)xyin[1]) << 16) >> 1) + 0x4000;
    for (;;)
    {
        if (!c->onscreen)
        {
            short xmin = HI(c->anchor.x), xmax = HI(c->in.x), v3 = HI(c->out.x), v4 = HI((c + 1)->anchor.x);
            short ymin, ymax;

            MINMAX(xmin, xmax, v3, v4);
            ymin = HI(c->anchor.y); ymax = HI(c->in.y); v3 = HI(c->out.y); v4 = HI((c + 1)->anchor.y);
            MINMAX(ymin, ymax, v3, v4);
            xmin = xmin * 2 - 16; ymin = ymin * 2 - 16;
            xmax = xmax * 2 + 1 + 16; ymax = ymax * 2 + 1 + 16;
            if (xmax < clip[0] || xmin > clip[2] || ymax < clip[1] || ymin > clip[3])
                c->level = 0;
            else if (xmin >= clip[0] && xmax <= clip[2] && ymin >= clip[1] && ymax <= clip[3])
                c->onscreen = 1;
        }
        if (c->level <= 0)
        {
            *xyout++ = (short)(((((c + 1)->anchor.x + c->in.x) >> 1) + c->out.x) >> 16);
            *xyout++ = (short)(((((c + 1)->anchor.y + c->in.y) >> 1) + c->out.y) >> 16);
            *xyout++ = (short)((((c->anchor.x + c->out.x) >> 1) + c->in.x) >> 16);
            *xyout++ = (short)((((c->anchor.y + c->out.y) >> 1) + c->in.y) >> 16);
            count += 3;
            if (c == bez)
                break;
            *xyout++ = (short)((c->anchor.x << 1) >> 16);
            *xyout++ = (short)((c->anchor.y << 1) >> 16);
            c--;
        }
        else
        {
            mid.x = (c->in.x + c->out.x) >> 1;
            mid.y = (c->in.y + c->out.y) >> 1;
            (c + 2)->anchor = (c + 1)->anchor;
            (c + 1)->out.x = (c->out.x + (c + 1)->anchor.x) >> 1;
            (c + 1)->out.y = (c->out.y + (c + 1)->anchor.y) >> 1;
            (c + 1)->in.x = (mid.x + (c + 1)->out.x) >> 1;
            (c + 1)->in.y = (mid.y + (c + 1)->out.y) >> 1;
            c->in.x = (c->in.x + c->anchor.x) >> 1;
            c->in.y = (c->in.y + c->anchor.y) >> 1;
            c->out.x = (c->in.x + mid.x) >> 1;
            c->out.y = (c->in.y + mid.y) >> 1;
            (c + 1)->anchor.x = ((c + 1)->in.x + c->out.x) >> 1;
            (c + 1)->anchor.y = ((c + 1)->in.y + c->out.y) >> 1;
            c->level--;
            (c + 1)->level = c->level;
            (c + 1)->onscreen = c->onscreen;
            c++;
        }
    }
    return count;
}

/* one curve: its points, P0 to P3, into out; returns how many */
static int ref_curve(const short *p, short scale, const short *clip, short *out)
{
    int n;

    out[0] = p[0];
    out[1] = p[1];
    n = ref_bez4(p, ref_depth(p, scale), out + 2, clip) - 1;
    out[2 + 2 * n] = p[6];
    out[3 + 2 * n] = p[7];
    return n + 2;
}

/* the distance from (x, y) to the cubic, in 1/16 pixels, from 2048 samples */
static long curve_distance(const short *p, short x, short y)
{
    long long best = 0x7fffffffffffLL;
    long k, n = 2048;

    for (k = 0; k <= n; k++)
    {
        long long u = n - k, t = k, d = (long long)n * n * n;
        long long bx = u * u * u * p[0] + 3 * u * u * t * p[2] + 3 * u * t * t * p[4] + t * t * t * p[6];
        long long by = u * u * u * p[1] + 3 * u * u * t * p[3] + 3 * u * t * t * p[5] + t * t * t * p[7];
        long long dx = x * d - bx, dy = y * d - by, e;

        dx = dx * 16 / d;
        dy = dy * 16 / d;
        e = dx * dx + dy * dy;
        if (e < best)
            best = e;
    }
    return (long)best;      /* squared, in 1/256 pixel units */
}

/* ------------------------------------------------------------------ */
/* the screen */

#define AX 8
#define AY 8
#define AW 288
#define AH 176

static unsigned short grabA[AW * AH], grabB[AW * AH];

static void clear_area(void)
{
    short r[4] = { AX, AY, AX + AW - 1, AY + AH - 1 };

    vsf_interior(screen, 1);
    vsf_color(screen, 0);
    vswr_mode(screen, 1);
    vsf_perimeter(screen, 0);
    vs_clip(screen, 0, r);
    vr_recfl(screen, r);
}

static void grab(unsigned short *buf)
{
    MFDB scr, mem;
    short xy[8] = { AX, AY, AX + AW - 1, AY + AH - 1, 0, 0, AW - 1, AH - 1 };

    memset(&scr, 0, sizeof(scr));
    memset(&mem, 0, sizeof(mem));
    mem.fd_addr = buf;
    mem.fd_w = AW;
    mem.fd_h = AH;
    mem.fd_wdwidth = AW / 16;
    mem.fd_nplanes = planes;
    memset(buf, 0, (long)AW * AH * planes / 8);
    vro_cpyfm(screen, 3, xy, &scr, &mem);
}

static unsigned long grab_pixel(const unsigned short *g, int x, int y)
{
    unsigned long v = 0;
    int p;

    if (planes == 16)
        return g[y * AW + x];
    for (p = 0; p < planes; p++)
        if (g[(y * (AW / 16) + x / 16) * planes + p] & (0x8000U >> (x & 15)))
            v |= 1UL << p;
    return v;
}

/* the grabs equal; if not, where they differ is reported */
static int same_grabs(void)
{
    long bad = 0;
    int x, y, fx = -1, fy = -1;
    char text[120];

    if (!memcmp(grabA, grabB, (long)AW * AH * planes / 8))
        return 1;
    for (y = 0; y < AH; y++)
        for (x = 0; x < AW; x++)
            if (grab_pixel(grabA, x, y) != grab_pixel(grabB, x, y))
            {
                if (!bad++)
                {
                    fx = x;
                    fy = y;
                }
            }
    snprintf(text, sizeof(text), "BEZIER %ld pixels differ, the first at %d,%d: %lx expected, %lx drawn",
             bad, fx + AX, fy + AY, grab_pixel(grabA, fx, fy), grab_pixel(grabB, fx, fy));
    say(text);
    return 0;
}

/* set the attributes used by both sides of a comparison */
static void attributes(short type, short width, short mode)
{
    vsl_type(screen, type);
    vsl_width(screen, width);
    vsl_color(screen, 1);
    vsl_ends(screen, 0, 0);
    vswr_mode(screen, mode);
    vsf_interior(screen, 2);
    vsf_style(screen, 4);
    vsf_color(screen, 1);
}

static short refpts[2400];

/* ------------------------------------------------------------------ */

static void test_switches(void)
{
    short work_out[57];

    vq_extnd(screen, 1, work_out);
    check((work_out[28] & 2) != 0, "work_out[28] bit 1: Beziers");
    max_vertices = work_out[14];
    vdi_raw(screen, 11, 13, 0, 1);
    check(contrl[4] == 1 && intout[0] == 7, "v_bez_on answers the greatest depth, 7");
    vdi_raw(screen, 11, 13, 0, 0);
    check(contrl[4] == 0, "v_bez_off answers nothing");
    check(bez_qual(100) == 100 && contrl[4] == 1, "v_bez_qual 100 -> 100");
    check(bez_qual(50) == 44, "v_bez_qual 50 -> 44 (depth scale 5)");
    check(bez_qual(0) == 0, "v_bez_qual 0 -> 0");
    bez_qual(100);
}

static const short full_clip_init[4] = { 0, 0, 0, 0 };
static short full_clip[4];

static void test_curve(void)
{
    static const short p[8] = { AX + 10, AY + 150, AX + 60, AY + 5, AX + 200, AY + 170, AX + 270, AY + 20 };
    static const unsigned char flags[4] = { 1, 0, 0, 0 };
    short xmin, ymin, xmax, ymax;
    int n, i, k, ok;
    char text[120];
    static const short styles[4][3] = { { 1, 1, 1 }, { 2, 1, 1 }, { 1, 5, 1 }, { 1, 1, 3 } };

    n = ref_curve(p, 0, full_clip, refpts);
    attributes(1, 1, 1);
    bez_call(6, 13, p, flags, 4, -1);
    snprintf(text, sizeof(text), "one curve: %d points (expected %d), extent %d,%d - %d,%d",
             intout[0], n, ptsout[0], ptsout[1], ptsout[2], ptsout[3]);
    say(text);
    check(contrl[2] == 2 && contrl[4] == 6, "v_bez answers 2 points and 6 words");
    check(intout[0] == n && intout[1] == 0, "the points of fVDI's subdivision, no moves");
    xmin = xmax = refpts[0];
    ymin = ymax = refpts[1];
    for (i = 1; i < n; i++)
    {
        if (refpts[2 * i] < xmin) xmin = refpts[2 * i];
        if (refpts[2 * i] > xmax) xmax = refpts[2 * i];
        if (refpts[2 * i + 1] < ymin) ymin = refpts[2 * i + 1];
        if (refpts[2 * i + 1] > ymax) ymax = refpts[2 * i + 1];
    }
    check(ptsout[0] == xmin && ptsout[1] == ymin && ptsout[2] == xmax && ptsout[3] == ymax, "the extent");
    check(n >= 12 && refpts[0] == p[0] && refpts[1] == p[1] && refpts[2 * n - 2] == p[6] && refpts[2 * n - 1] == p[7],
          "the curve runs from its first point to its last");
    ok = 1;
    for (i = 0; i < n; i++)
        if (curve_distance(p, refpts[2 * i], refpts[2 * i + 1]) > 24 * 24)      /* 1.5 pixels */
            ok = 0;
    check(ok, "every point lies within 1.5 pixels of the cubic");

    for (k = 0; k < 4; k++)
    {
        clear_area();
        attributes(styles[k][0], styles[k][1], styles[k][2]);
        v_pline(screen, n, refpts);
        grab(grabA);
        clear_area();
        attributes(styles[k][0], styles[k][1], styles[k][2]);
        bez_call(6, 13, p, flags, 4, -1);
        grab(grabB);
        snprintf(text, sizeof(text), "the pixels of v_pline of the points (type %d, width %d, mode %d)",
                 styles[k][0], styles[k][1], styles[k][2]);
        check(same_grabs(), text);
    }
}

static void test_moves(void)
{
    static const short p[18] = {
        AX + 10, AY + 20, AX + 40, AY + 100, AX + 80, AY + 0, AX + 120, AY + 60,    /* curve */
        AX + 130, AY + 150,                                                         /* line to */
        AX + 150, AY + 160, AX + 200, AY + 20, AX + 240, AY + 170, AX + 280, AY + 30 /* move, curve */
    };
    static const unsigned char flags[9] = { 1, 0, 0, 0, 0, 3, 0, 0, 0 };
    int n1, n2;

    n1 = ref_curve(p, 0, full_clip, refpts);
    refpts[2 * n1] = p[8];
    refpts[2 * n1 + 1] = p[9];
    n1++;
    n2 = ref_curve(p + 10, 0, full_clip, refpts + 2 * n1);
    clear_area();
    attributes(1, 1, 1);
    v_pline(screen, n1, refpts);
    v_pline(screen, n2, refpts + 2 * n1);
    grab(grabA);
    clear_area();
    attributes(1, 1, 1);
    bez_call(6, 13, p, flags, 9, -1);
    check(intout[0] == n1 + n2 && intout[1] == 1, "curve, line, move, curve: the points and one move");
    grab(grabB);
    check(same_grabs(), "two polylines, apart at the move");
}

static void test_fill(void)
{
    static const short p[8] = { AX + 20, AY + 150, AX + 40, AY + 0, AX + 250, AY + 10, AX + 260, AY + 160 };
    static const unsigned char flags[4] = { 1, 0, 0, 0 };
    /*
     * Two curves side by side, the second the first moved right: the same
     * rows.  (Filled at once, the contours share one range of rows, as the
     * outlines of one polygon do; a polygon's top row is not filled, so
     * only contours with the same top compare with separate v_fillarea.)
     */
    static const short two[16] = {
        AX + 10, AY + 80, AX + 30, AY + 0, AX + 110, AY + 0, AX + 130, AY + 80,
        AX + 150, AY + 80, AX + 170, AY + 0, AX + 250, AY + 0, AX + 270, AY + 80 };
    static const unsigned char twoflags[8] = { 1, 0, 0, 0, 3, 0, 0, 0 };
    static const short ring[10] = { AX + 40, AY + 20, AX + 240, AY + 20, AX + 240, AY + 160, AX + 40, AY + 160,
                                    AX + 100, AY + 70 };
    static unsigned char ringflags[10];
    short ringpts[18];
    int n, n2, per;
    unsigned short pixel, bg, fg;
    char text[120];

    for (per = 0; per < 2; per++)
    {
        n = ref_curve(p, 0, full_clip, refpts);
        clear_area();
        attributes(1, 1, 1);
        vsf_perimeter(screen, per);
        v_fillarea(screen, n, refpts);
        grab(grabA);
        clear_area();
        attributes(1, 1, 1);
        vsf_perimeter(screen, per);
        bez_call(9, 13, p, flags, 4, -1);
        check(intout[0] == n + 1, "v_bez_fill: the points and the first again at the end");
        grab(grabB);
        check(same_grabs(), per ? "v_bez_fill is v_fillarea of the points (perimeter)" :
                                  "v_bez_fill is v_fillarea of the points");
    }

    n = ref_curve(two, 0, full_clip, refpts);
    n2 = ref_curve(two + 8, 0, full_clip, refpts + 2 * n);
    clear_area();
    attributes(1, 1, 1);
    vsf_perimeter(screen, 0);
    v_fillarea(screen, n, refpts);
    v_fillarea(screen, n2, refpts + 2 * n);
    grab(grabA);
    clear_area();
    attributes(1, 1, 1);
    vsf_perimeter(screen, 0);
    bez_call(9, 13, two, twoflags, 8, -1);
    check(intout[0] == n + n2 + 2 && intout[1] == 1, "two curves apart: each closed, one move");
    grab(grabB);
    check(same_grabs(), "two curves apart are filled as two areas");

    /* a ring: the square, then (a move) a smaller one inside it; even-odd leaves a hole */
    memcpy(ringpts, ring, 8 * 2);
    ringpts[8] = AX + 100; ringpts[9] = AY + 70;
    ringpts[10] = AX + 180; ringpts[11] = AY + 70;
    ringpts[12] = AX + 180; ringpts[13] = AY + 110;
    ringpts[14] = AX + 100; ringpts[15] = AY + 110;
    memset(ringflags, 0, sizeof(ringflags));
    ringflags[4] = 2;
    clear_area();
    attributes(1, 1, 1);
    vsf_interior(screen, 1);
    vsf_perimeter(screen, 0);
    bez_call(9, 13, ringpts, ringflags, 8, -1);
    check(intout[0] == 10 && intout[1] == 1, "the ring: 8 points, each square closed, one move");
    grab(grabB);
    bg = grab_pixel(grabB, 140, 90);        /* the hole */
    fg = grab_pixel(grabB, 60, 90);         /* the ring */
    pixel = grab_pixel(grabB, 20, 90);      /* outside */
    snprintf(text, sizeof(text), "BEZIER ring: hole %x, ring %x, outside %x", bg, fg, pixel);
    say(text);
    check(bg == pixel && fg != pixel, "the ring is filled and its hole is not");
}

static void test_limits(void)
{
    static const short p[16] = {
        AX + 10, AY + 20, AX + 40, AY + 100, AX + 80, AY + 0, AX + 120, AY + 60,
        AX + 130, AY + 70, AX + 160, AY + 170, AX + 200, AY + 20, AX + 280, AY + 90 };
    static const unsigned char flags[8] = { 1, 0, 0, 0, 1, 0, 0, 0 };
    static const short three[6] = { AX + 10, AY + 10, AX + 100, AY + 150, AX + 200, AY + 30 };
    int n1, n2;

    n1 = ref_curve(p, 0, full_clip, refpts);
    n2 = ref_curve(p + 8, 0, full_clip, refpts + 2 * n1);
    attributes(1, 1, 1);
    bez_call(6, 13, p, flags, 8, 4);
    check(intout[0] == n1 + n2, "two curves: the flags of both read");
    bez_call(6, 13, p, flags, 8, 2);
    check(intout[0] == n1 + 4, "contrl[3] = 2: the second curve's flag is not read, its points are a polyline");

    /* three points with a curve's flag: a polyline */
    clear_area();
    attributes(1, 1, 1);
    v_pline(screen, 3, (short *)three);
    grab(grabA);
    clear_area();
    attributes(1, 1, 1);
    bez_call(6, 13, three, flags, 3, -1);
    check(intout[0] == 3 && intout[1] == 0, "a curve short of its four points: three points");
    grab(grabB);
    check(same_grabs(), "and drawn as a polyline");

    /* no intin: v_pline as ever; without sub-opcode 13, only after v_bez_on */
    clear_area();
    attributes(1, 1, 1);
    bez_call(6, 13, p, flags, 4, 0);
    check(contrl[4] == 0, "sub-opcode 13 without intin is v_pline");
    grab(grabB);
    clear_area();
    attributes(1, 1, 1);
    v_pline(screen, 4, (short *)p);
    grab(grabA);
    check(same_grabs(), "and draws as v_pline");
    bez_call(6, 0, p, flags, 4, -1);
    check(contrl[4] == 0, "v_pline with intin but without sub-opcode 13 is v_pline");
    vdi_raw(screen, 11, 13, 0, 1);
    bez_call(6, 0, p, flags, 4, -1);
    check(contrl[4] == 6 && intout[0] == n1, "after v_bez_on, v_pline with intin is v_bez");
    vdi_raw(screen, 11, 13, 0, 0);
    bez_call(6, 0, p, flags, 4, -1);
    check(contrl[4] == 0, "after v_bez_off it is v_pline again");

    /* contrl[3] of 16384 words or more: still only the flags of the points are read */
    bez_call(6, 13, p, flags, 4, 20000);
    check(contrl[4] == 6 && intout[0] == n1, "contrl[3] = 20000: the curve, its flags read");
}

static void test_clip_quality(void)
{
    static const short p[8] = { AX + 10, AY + 150, AX + 60, AY + 5, AX + 200, AY + 170, AX + 270, AY + 20 };
    static const unsigned char flags[4] = { 1, 0, 0, 0 };
    short clip[4] = { AX + 50, AY + 40, AX + 180, AY + 130 };
    unsigned short *a, *b;
    int n, x, y, ok, i, count;
    static short many[2400];
    static unsigned char manyflags[600];
    char text[120];

    /* the reference made for this clip */
    n = ref_curve(p, 0, clip, refpts);
    clear_area();
    attributes(1, 1, 1);
    vs_clip(screen, 1, clip);
    v_pline(screen, n, refpts);
    vs_clip(screen, 0, clip);
    grab(grabA);
    clear_area();
    attributes(1, 1, 1);
    vs_clip(screen, 1, clip);
    bez_call(6, 13, p, flags, 4, -1);
    vs_clip(screen, 0, clip);
    check(intout[0] == n, "clipped: the points made for the clip");
    grab(grabB);
    check(same_grabs(), "clipped: the pixels of v_pline of them");
    /* and outside the clip nothing at all: compare with an empty area */
    memcpy(grabA, grabB, sizeof(grabA));
    clear_area();
    grab(grabB);
    a = grabA;
    b = grabB;
    ok = 1;
    if (planes == 16)
    {
        for (y = 0; y < AH; y++)
            for (x = 0; x < AW; x++)
                if ((x + AX < clip[0] || x + AX > clip[2] || y + AY < clip[1] || y + AY > clip[3]) &&
                    a[y * AW + x] != b[y * AW + x])
                    ok = 0;
    }
    else
    {
        for (y = 0; y < AH; y++)
            for (x = 0; x < AW; x++)
            {
                int p2, same = 1;

                for (p2 = 0; p2 < planes; p2++)
                {
                    long at = (y * (AW / 16) + x / 16) * planes + p2;

                    if ((a[at] ^ b[at]) & (0x8000U >> (x & 15)))
                        same = 0;
                }
                if (!same && (x + AX < clip[0] || x + AX > clip[2] || y + AY < clip[1] || y + AY > clip[3]))
                    ok = 0;
            }
    }
    check(ok, "clipped: nothing drawn outside the clipping rectangle");

    /* the coarsest quality */
    bez_qual(0);
    n = ref_curve(p, 9, full_clip, refpts);
    clear_area();
    attributes(1, 1, 1);
    v_pline(screen, n, refpts);
    grab(grabA);
    clear_area();
    attributes(1, 1, 1);
    bez_call(6, 13, p, flags, 4, -1);
    check(intout[0] == n && n < 40, "the coarsest quality: fewer points");
    grab(grabB);
    check(same_grabs(), "the coarsest quality: the pixels of v_pline of them");
    bez_qual(100);

    /* many large curves: made coarser, never more points than the VDI takes */
    /* each made of 13 points at the coarsest quality, more at any finer one */
    count = (max_vertices > 1024 ? 1024 : max_vertices) / 16;
    memset(manyflags, 0, sizeof(manyflags));
    for (i = 0; i < count; i++)
    {
        many[8 * i + 0] = AX + (i * 7) % 200;
        many[8 * i + 1] = AY + 170;
        many[8 * i + 2] = AX;
        many[8 * i + 3] = AY;
        many[8 * i + 4] = AX + 280;
        many[8 * i + 5] = AY;
        many[8 * i + 6] = AX + 270 - (i * 5) % 200;
        many[8 * i + 7] = AY + 170;
        manyflags[4 * i] = 3;
    }
    clear_area();
    attributes(1, 1, 1);
    bez_call(6, 13, many, manyflags, 4 * count, -1);
    snprintf(text, sizeof(text), "%d large curves: %d points, at most %d", count, intout[0], max_vertices);
    say(text);
    check(contrl[4] == 6 && intout[0] > 4 * count && intout[0] <= max_vertices && intout[1] == count,
          "many large curves are made coarser, within work_out[14]");
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

int main(void)
{
    short work_out[57];
    char text[120];

    say("BEZIER start");
    check(appl_init() >= 0, "AES application registration");
    screen = open_vwk();
    check(screen > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(screen, 1, work_out);
    planes = work_out[4];
    vq_extnd(screen, 0, work_out);
    memcpy(full_clip, full_clip_init, sizeof(full_clip));
    full_clip[2] = work_out[0];
    full_clip[3] = work_out[1];
    snprintf(text, sizeof(text), "BEZIER %d planes, %dx%d", planes, work_out[0] + 1, work_out[1] + 1);
    say(text);
    check(work_out[0] >= 303 && work_out[1] >= 191, "a screen of 304x192 or more");
    if (failed)
        goto out;

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    test_switches();
    say("BEZIER switches done");
    test_curve();
    say("BEZIER one curve done");
    test_moves();
    say("BEZIER moves done");
    test_fill();
    say("BEZIER fills done");
    test_limits();
    say("BEZIER limits done");
    test_clip_quality();
    say("BEZIER clip and quality done");
    clear_area();
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);

out:
    if (screen > 0)
        v_clsvwk(screen);
    appl_exit();
    if (!failed)
    {
        snprintf(text, sizeof(text), "BEZIER %ld checks", checks);
        say(text);
        say("FVDI BEZIER GUEST PASS");
    }
    return failed;
}
