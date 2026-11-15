/*
 * nvdicol_guest.c - RGB foreground and background colours as drawn.
 *
 * COLORS only asks the VDI what it keeps.  This draws: in replace mode,
 * an RGB fill colour with a pattern and an RGB fill background, an RGB text
 * colour and background, and RGB line colours with a dashed style (a
 * horizontal, a vertical and a slanted line), each into a white area, and
 * copies the area to a memory form in the device's format.  On a 16-bit
 * screen the ink must be the v_color2value of the foreground colour and the
 * rest of the pattern, the glyph cell or the gaps of the dashes the
 * v_color2value of the background; on a screen with planes the ink is the
 * hardware pen of the nearest pen and the rest pen 0 (replace mode clears
 * the planes there).  A pen set afterwards (vsf_color) is drawn and
 * queried as that pen again, and the attribute queries give a VDI pen, 0-255.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI NVDICOL GUEST PASS" or "FVDI NVDICOL FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

#define AW 96
#define AH 48

static short contrl[12], intin[16], ptsin[16], intout[16], ptsout[16];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
static int failed;
static short handle, planes, numcolors, cw, ch;
static unsigned short buf[AW * AH];     /* 16-bit, or 8 planes of 6 words a row */

static void say(const char *text)
{
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

static void check(int ok, const char *what)
{
    char line[160];

    snprintf(line, sizeof(line), "NVDICOL %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI NVDICOL FAIL: %s", what);
        say(line);
    }
}

static void vdi_raw(short opcode, short sub, short nintin)
{
    contrl[0] = opcode;
    contrl[1] = 0;
    contrl[3] = nintin;
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

typedef struct { unsigned short r, g, b; } Rgb;

/* 200 (foreground) or 201 (background), sub-function 0 text, 1 fill, 2 line, 3 marker */
static short set_rgb(short opcode, short sub, Rgb c)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short)c.r;
    intin[4] = (short)c.g;
    intin[5] = (short)c.b;
    intout[0] = 0x7777;
    vdi_raw(opcode, sub, 6);
    return intout[0];
}

/* v_color2value */
static unsigned long color2value(Rgb c)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short)c.r;
    intin[4] = (short)c.g;
    intin[5] = (short)c.b;
    vdi_raw(204, 0, 6);
    return ((unsigned long)(unsigned short)intout[0] << 16) | (unsigned short)intout[1];
}

/* the colour a pen was asked to be, in 0-65535 */
static Rgb pen_rgb(short pen)
{
    Rgb c;
    short rgb[3];

    vq_color(handle, pen, 0, rgb);
    c.r = (unsigned short)((rgb[0] * 65535L + 500) / 1000);
    c.g = (unsigned short)((rgb[1] * 65535L + 500) / 1000);
    c.b = (unsigned short)((rgb[2] * 65535L + 500) / 1000);
    return c;
}

/* a pixel of the form as the device holds it: 16 bits, or the bits of the planes */
static unsigned pixel_at(short x, short y)
{
    short wd = (AW + 15) / 16;

    if (planes == 16)
        return buf[(long)y * AW + x];
    else
    {
        const unsigned short *w = buf + ((long)y * wd + (x >> 4)) * planes;
        unsigned v = 0;
        short k;

        for (k = 0; k < planes; k++)
            v |= (unsigned)((w[k] >> (15 - (x & 15))) & 1) << k;
        return v;
    }
}

/* the area (x0, y0, AW by AH) of the screen in 'buf' */
static void grab(short x0, short y0)
{
    MFDB m, screen;
    short xy[8];

    memset(&m, 0, sizeof(m));
    memset(&screen, 0, sizeof(screen));
    m.fd_addr = buf;
    m.fd_w = AW;
    m.fd_h = AH;
    m.fd_wdwidth = (AW + 15) / 16;
    m.fd_stand = 0;
    m.fd_nplanes = planes;
    xy[0] = x0;
    xy[1] = y0;
    xy[2] = x0 + AW - 1;
    xy[3] = y0 + AH - 1;
    xy[4] = 0;
    xy[5] = 0;
    xy[6] = AW - 1;
    xy[7] = AH - 1;
    memset(buf, 0, sizeof(buf));
    vro_cpyfm(handle, S_ONLY, xy, &screen, &m);
}

/* a solid white area, in pen 0, replace mode */
static void white_area(short x0, short y0)
{
    short xy[4];

    xy[0] = x0;
    xy[1] = y0;
    xy[2] = x0 + AW - 1;
    xy[3] = y0 + AH - 1;
    vswr_mode(handle, 1);
    vsf_interior(handle, 1);
    vsf_color(handle, 0);
    vsf_perimeter(handle, 0);
    vr_recfl(handle, xy);
}

/* how many pixels of a box of the grabbed area hold each value */
typedef struct { long fg, bg, white, other; unsigned first_other; } Count;

static Count count(short x1, short y1, short x2, short y2, unsigned fg, unsigned bg, unsigned white)
{
    Count n;
    short x, y;

    memset(&n, 0, sizeof(n));
    for (y = y1; y <= y2; y++)
        for (x = x1; x <= x2; x++)
        {
            unsigned v = pixel_at(x, y);

            if (v == fg)
                n.fg++;
            else if (v == bg)
                n.bg++;
            else if (v == white)
                n.white++;
            else
            {
                if (!n.other)
                    n.first_other = v;
                n.other++;
            }
        }
    return n;
}

/*
 * What a box must hold: only ink, background and white; ink at all; and
 * background if it is not the colour of the area.  'bg_value' is the
 * value the background pixels have.
 */
static void expect(const char *what, short x1, short y1, short x2, short y2,
                   unsigned fg, unsigned bg, unsigned white)
{
    char line[200];
    Count n = count(x1, y1, x2, y2, fg, bg, white);

    snprintf(line, sizeof(line), "NVDICOL %s: ink %ld, background %ld, white %ld, other %ld (first %04x); fg %04x bg %04x white %04x",
             what, n.fg, n.bg, n.white, n.other, n.first_other, fg, bg, white);
    say(line);
    snprintf(line, sizeof(line), "%s: only the ink and the background colours are drawn", what);
    check(n.other == 0, line);
    snprintf(line, sizeof(line), "%s: the foreground colour is drawn", what);
    check(n.fg > 0, line);
    if (bg != white)
    {
        snprintf(line, sizeof(line), "%s: the background colour is drawn", what);
        check(n.bg > 0, line);
    }
}

/*
 * a box of the grabbed area is all one colour, the one of a VDI pen: on a
 * 16-bit screen the pixel v_color2value gives for the colour the pen was
 * asked to be, on a screen with planes the hardware value that
 * v_get_pixel reports as that pen
 */
static int box_is_pen(short pen, short x0, short y0, short x1, short y1)
{
    unsigned want = pixel_at(x1, y1);
    short x, y, pel, idx;
    char line[100];

    for (y = 8; y < AH - 8; y++)
        for (x = 8; x < AW - 8; x++)
            if (pixel_at(x, y) != want)
                return 0;
    if (planes == 16)
        want = want == (unsigned)color2value(pen_rgb(pen));
    else
    {
        v_get_pixel(handle, x0 + x1, y0 + y1, &pel, &idx);
        snprintf(line, sizeof(line), "NVDICOL pen %d: pixel %04x, v_get_pixel hardware %04x pen %d", pen, want, pel, idx);
        say(line);
        want = (idx == pen);
    }
    return want;
}

static short open_vwk(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short ch2, bw, bh, h;

    h = graf_handle(&cw, &ch2, &bw, &bh);
    ch = ch2;
    v_opnvwk(work_in, &h, work_out);
    return h;
}

int main(void)
{
    /* dark ink on a lighter background, so a screen of two colours tells them apart */
    static const Rgb ink = { 0x2000, 0x1000, 0x4000 };
    static const Rgb paper = { 0xc000, 0xd000, 0xb000 };
    short work_out[57];
    short x0 = 24, y0 = 24;
    unsigned fg, bg, white;
    short attrib[10], pen, i;
    short xy[8];
    Rgb whitec = { 0xffff, 0xffff, 0xffff };
    char what[100];

    say("NVDICOL start");
    check(appl_init() >= 0, "AES application registration");
    handle = open_vwk();
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(handle, 1, work_out);
    planes = work_out[4];
    numcolors = 1 << (planes < 8 ? planes : 8);
    snprintf(what, sizeof(what), "NVDICOL %d planes, cell %dx%d", planes, cw, ch);
    say(what);

    fg = (unsigned)color2value(ink);
    bg = planes == 16 ? (unsigned)color2value(paper) : 0;   /* replace mode clears the planes: pen 0 */
    white = (unsigned)color2value(whitec);
    if (planes != 16)
        bg = white;
    snprintf(what, sizeof(what), "NVDICOL ink %04x, background %04x, white %04x", fg, bg, white);
    say(what);

    {
        Rgb p0 = pen_rgb(0), p1 = pen_rgb(numcolors > 1 ? 1 : 0);

        snprintf(what, sizeof(what), "NVDICOL pen 0 asks %04x %04x %04x (value %04x), pen 1 %04x %04x %04x (value %04x)",
                 p0.r, p0.g, p0.b, (unsigned)color2value(p0), p1.r, p1.g, p1.b, (unsigned)color2value(p1));
        say(what);
    }

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);

    /* a patterned fill: ink and background */
    white_area(x0, y0);
    check(set_rgb(200, 1, ink) == 1, "vsf_fg_color accepted");
    check(set_rgb(201, 1, paper) == 1, "vsf_bg_color accepted");
    vswr_mode(handle, 1);
    vsf_interior(handle, 2);
    vsf_style(handle, 3);
    xy[0] = x0 + 8;
    xy[1] = y0 + 8;
    xy[2] = x0 + AW - 9;
    xy[3] = y0 + AH - 9;
    vr_recfl(handle, xy);
    grab(x0, y0);
    expect("pattern fill", 8, 8, AW - 9, AH - 9, fg, bg, white);

    /* the same fill, a solid one in the ink colour only */
    white_area(x0, y0);                 /* which chose pen 0 again for the fill */
    check(set_rgb(200, 1, ink) == 1, "vsf_fg_color accepted again");
    vsf_interior(handle, 1);
    vr_recfl(handle, xy);
    grab(x0, y0);
    {
        Count n = count(8, 8, AW - 9, AH - 9, fg, 0xdead, white);

        snprintf(what, sizeof(what), "NVDICOL solid fill: ink %ld, other %ld", n.fg, n.other);
        say(what);
        check(n.fg == (long)(AW - 16) * (AH - 16), "a solid fill is all ink");
    }

    /* the attribute queries answer with a VDI pen */
    vqf_attributes(handle, attrib);
    check(attrib[1] >= 0 && attrib[1] < 256 && attrib[1] < numcolors, "vqf_attributes after an RGB fill colour is a pen");
    snprintf(what, sizeof(what), "NVDICOL vqf_attributes pen %d", attrib[1]);
    say(what);

    /* a pen chosen by number goes back to drawing and querying as that pen */
    pen = numcolors > 3 ? 3 : numcolors - 1;
    vsf_color(handle, pen);
    vqf_attributes(handle, attrib);
    check(attrib[1] == pen, "vsf_color after an RGB colour is queried as that pen");
    white_area(x0, y0);
    vsf_interior(handle, 1);
    vsf_color(handle, pen);
    vr_recfl(handle, xy);
    grab(x0, y0);
    check(box_is_pen(pen, x0, y0, AW / 2, AH / 2), "vsf_color after an RGB colour draws that pen");

    /* text: the glyph cells hold ink and background */
    white_area(x0, y0);
    check(set_rgb(200, 0, ink) == 1, "vst_fg_color accepted");
    check(set_rgb(201, 0, paper) == 1, "vst_bg_color accepted");
    vswr_mode(handle, 1);
    vst_alignment(handle, 0, 5, &i, &i);
    v_gtext(handle, x0 + 4, y0 + 4, "MMMMM");
    grab(x0, y0);
    expect("text", 4, 4, 4 + 5 * cw - 1, 4 + ch - 1, fg, bg, white);
    vqt_attributes(handle, attrib);
    check(attrib[1] >= 0 && attrib[1] < 256 && attrib[1] < numcolors, "vqt_attributes after an RGB text colour is a pen");

    /* lines, dashed: ink and background along the line */
    check(set_rgb(200, 2, ink) == 1, "vsl_fg_color accepted");
    check(set_rgb(201, 2, paper) == 1, "vsl_bg_color accepted");
    vswr_mode(handle, 1);
    vsl_width(handle, 1);
    vsl_type(handle, 3);

    white_area(x0, y0);
    xy[0] = x0 + 4;
    xy[1] = y0 + 20;
    xy[2] = x0 + AW - 5;
    xy[3] = y0 + 20;
    v_pline(handle, 2, xy);
    grab(x0, y0);
    expect("horizontal dashed line", 4, 20, AW - 5, 20, fg, bg, white);

    white_area(x0, y0);
    xy[0] = x0 + 40;
    xy[1] = y0 + 2;
    xy[2] = x0 + 40;
    xy[3] = y0 + AH - 3;
    v_pline(handle, 2, xy);
    grab(x0, y0);
    expect("vertical dashed line", 40, 2, 40, AH - 3, fg, bg, white);

    white_area(x0, y0);
    xy[0] = x0 + 4;
    xy[1] = y0 + 2;
    xy[2] = x0 + AW - 5;
    xy[3] = y0 + AH - 3;
    v_pline(handle, 2, xy);
    grab(x0, y0);
    expect("slanted dashed line", 4, 2, AW - 5, AH - 3, fg, bg, white);
    vql_attributes(handle, attrib);
    check(attrib[1] >= 0 && attrib[1] < 256 && attrib[1] < numcolors, "vql_attributes after an RGB line colour is a pen");

    /* markers: only the query */
    check(set_rgb(200, 3, ink) == 1, "vsm_fg_color accepted");
    vqm_attributes(handle, attrib);
    check(attrib[1] >= 0 && attrib[1] < 256 && attrib[1] < numcolors, "vqm_attributes after an RGB marker colour is a pen");

    /*
     * a second workstation draws its own colours: A has an RGB fill colour,
     * B another; one call on A, then B draws, and B must draw its own
     * (the colours are per workstation, and the drawing code must read the
     * workstation of the call, not that of the call before)
     */
    {
        short h2 = open_vwk(), keep = handle;
        unsigned bval;
        Count n;

        check(h2 > 0 && h2 != keep, "second virtual workstation");
        handle = h2;
        white_area(x0, y0);
        vsf_interior(handle, 1);
        check(set_rgb(200, 1, paper) == 1, "second workstation: vsf_fg_color accepted");
        bval = (unsigned)color2value(paper);
        handle = keep;
        check(set_rgb(200, 1, ink) == 1, "first workstation: vsf_fg_color again");
        vsl_width(keep, 1);                     /* the call before B draws is A's */
        handle = h2;
        xy[0] = x0 + 8;
        xy[1] = y0 + 8;
        xy[2] = x0 + AW - 9;
        xy[3] = y0 + AH - 9;
        vr_recfl(handle, xy);
        grab(x0, y0);
        n = count(8, 8, AW - 9, AH - 9, bval, 0xdead, white);
        snprintf(what, sizeof(what), "NVDICOL second workstation: %04x drawn %ld of %ld times, other %ld (first %04x)",
                 bval, n.fg, (long)(AW - 16) * (AH - 16), n.other, n.first_other);
        say(what);
        check(n.fg == (long)(AW - 16) * (AH - 16), "a workstation draws its own RGB colour after a call on another");
        /* and a pen on it */
        vsf_color(handle, 1);
        vr_recfl(handle, xy);
        grab(x0, y0);
        check(box_is_pen(1, x0, y0, AW / 2, AH / 2), "a workstation without RGB colours draws its own pens");
        handle = keep;
        v_clsvwk(h2);
    }

    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
    form_dial(FMD_FINISH, 0, 0, 0, 0, 0, 0, work_out[0] + 1, work_out[1] + 1);

out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI NVDICOL GUEST PASS");
    return failed;
}
