/*
 * bmrgb_guest.c - RGB background colours in replace mode, drawn on the
 * screen and into off-screen bitmaps (v_open_bm), on a 16-bit screen.
 *
 * The same picture goes to three places: the screen, a bitmap in memory
 * the VDI allocates, and a bitmap in the program's memory at an address
 * and with a line width that are not multiples of four bytes.  The picture
 * has a fill with a user-defined pattern (rows of all ink, all background
 * and mixed), a solid fill and a hollow one (all background), text at a
 * byte-aligned and at an unaligned position, dashed and solid horizontal,
 * vertical and slanted lines, a vrt_cpyfm expand, and two vro_cpyfm copies
 * of the patterned fill within the picture (a plain one to a position one
 * pixel further on, and an xor one).  Fills, text and lines each have their
 * own RGB foreground and background colour.
 *
 * Checks, in each of the three: the patterned fill holds exactly the
 * colours its pattern gives each pixel, the solid fill only its ink, the
 * hollow fill only its background, the text cells and the dashed lines
 * their own ink and background and nothing else, the solid lines only
 * their ink, the expand the source's bits, the copies their source (xor
 * the white under it).  Then the three must be equal
 * pixel for pixel, and the program's bitmap must leave the two bytes past
 * each line and the bytes either side alone.
 *
 * Screens with planes: skipped (the colours are pens there).
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI BMRGB GUEST PASS", "FVDI BMRGB GUEST SKIPPED" or
 * "FVDI BMRGB FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
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

short v_open_bm(short base_handle, GCBITMAP *bitmap, short color_flags,
                short unit_flags, short pixel_width, short pixel_height);
void v_clsbm(short handle);

#define AW 96                   /* the picture: a multiple of 16 pixels wide */
#define AH 112
#define PITCH_B (AW * 2 + 2)    /* bytes a line of the program's bitmap: 2 mod 4 */
#define SENTINEL 0xa5

static short contrl[12], intin[16], ptsin[16], intout[16], ptsout[16];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
static int failed;
static short screen, cw, ch;
static unsigned short scr[AW * AH];                 /* the screen's picture */
static unsigned char raw_b[PITCH_B * AH + 64];      /* the program's bitmap */

typedef struct { unsigned short r, g, b; } Rgb;

static const Rgb ink_fill = { 0x2000, 0x1000, 0x4000 }, paper_fill = { 0xc000, 0xd000, 0xb000 };
static const Rgb ink_text = { 0x8000, 0x0000, 0x0000 }, paper_text = { 0xe000, 0xe000, 0x4000 };
static const Rgb ink_line = { 0x0000, 0x6000, 0x0000 }, paper_line = { 0x4000, 0xc000, 0xf000 };

/* rows of all ink (solid store), all background (the background store) and mixed */
static short udpat[16] = {
    (short)0xffff, 0x0000, (short)0xa5a5, 0x0ff0, (short)0x8001, 0x0000, (short)0xffff, 0x1234,
    0x0000, 0x7ffe, (short)0xffff, 0x0000, 0x5555, (short)0xaaaa, (short)0xf00f, 0x0000
};

/* 32x8 monochrome source of the expand */
static unsigned short mono[2 * 8] = {
    0xffff, 0x0000, 0x8001, 0x8001, 0xf0f0, 0x0f0f, 0x1234, 0x5678,
    0x0000, 0xffff, 0xaaaa, 0x5555, 0x7fff, 0xfffe, 0x00ff, 0xff00
};

/* the colours as pixels, the same in all three */
static unsigned short v_white, v_ifill, v_pfill, v_itext, v_ptext, v_iline, v_plin;

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

    snprintf(line, sizeof(line), "BMRGB %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI BMRGB FAIL: %s", what);
        say(line);
    }
}

static void vdi_raw(short h, short opcode, short sub, short nintin)
{
    contrl[0] = opcode;
    contrl[1] = 0;
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

/* 200 (foreground) or 201 (background), sub-function 0 text, 1 fill, 2 line */
static short set_rgb(short h, short opcode, short sub, Rgb c)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short)c.r;
    intin[4] = (short)c.g;
    intin[5] = (short)c.b;
    intout[0] = 0x7777;
    vdi_raw(h, opcode, sub, 6);
    return intout[0];
}

/* v_color2value: the 16-bit pixel */
static unsigned short color2value(short h, Rgb c)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short)c.r;
    intin[4] = (short)c.g;
    intin[5] = (short)c.b;
    vdi_raw(h, 204, 0, 6);
    return (unsigned short)intout[1];
}

static void rect(short h, short x1, short y1, short x2, short y2)
{
    short xy[4] = { x1, y1, x2, y2 };

    vr_recfl(h, xy);
}

static void line(short h, short x1, short y1, short x2, short y2)
{
    short xy[4] = { x1, y1, x2, y2 };

    v_pline(h, 2, xy);
}

/* the picture, its top left corner at (ox, oy) of workstation h */
static void draw(short h, short ox, short oy)
{
    static const char *const what[6] = {
        "vsf_fg_color", "vsf_bg_color", "vst_fg_color", "vst_bg_color", "vsl_fg_color", "vsl_bg_color"
    };
    short ok[6], xy[8], cols[2] = { 1, 0 }, d, k;
    MFDB src, dst;
    char text[100];

    vswr_mode(h, 1);
    vsf_perimeter(h, 0);
    vsf_interior(h, 1);
    vsf_color(h, 0);
    rect(h, ox, oy, ox + AW - 1, oy + AH - 1);         /* white */

    /* fills */
    ok[0] = set_rgb(h, 200, 1, ink_fill);
    ok[1] = set_rgb(h, 201, 1, paper_fill);
    vsf_udpat(h, udpat, 1);
    vsf_interior(h, 4);
    rect(h, ox + 1, oy + 1, ox + 46, oy + 31);         /* user pattern */
    vsf_interior(h, 1);
    rect(h, ox + 49, oy + 1, ox + 94, oy + 10);        /* solid */
    vsf_interior(h, 0);
    rect(h, ox + 49, oy + 12, ox + 94, oy + 20);       /* hollow: all background */

    /* text */
    ok[2] = set_rgb(h, 200, 0, ink_text);
    ok[3] = set_rgb(h, 201, 0, paper_text);
    vst_alignment(h, 0, 5, &d, &d);
    v_gtext(h, ox + 48, oy + 22, "MiW");               /* x a multiple of 8 */
    v_gtext(h, ox + 51, oy + 40, "MiW");               /* and not */

    /* lines */
    ok[4] = set_rgb(h, 200, 2, ink_line);
    ok[5] = set_rgb(h, 201, 2, paper_line);
    vsl_width(h, 1);
    vsl_udsty(h, (short)0xf0f0);
    vsl_type(h, 7);
    line(h, ox + 2, oy + 34, ox + 45, oy + 34);        /* horizontal, dashed */
    line(h, ox + 4, oy + 38, ox + 4, oy + 62);         /* vertical, dashed */
    line(h, ox + 10, oy + 38, ox + 45, oy + 62);       /* slanted, dashed */
    vsl_type(h, 1);
    line(h, ox + 2, oy + 36, ox + 45, oy + 36);        /* horizontal, solid */
    line(h, ox + 7, oy + 38, ox + 7, oy + 62);         /* vertical, solid */
    line(h, ox + 40, oy + 79, ox + 94, oy + 64);       /* slanted, solid */

    /* an expand, replace mode, pens 1 and 0, into the workstation's own form */
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.fd_addr = mono;
    src.fd_w = 32;
    src.fd_h = 8;
    src.fd_wdwidth = 2;
    src.fd_stand = 0;
    src.fd_nplanes = 1;
    xy[0] = 0;
    xy[1] = 0;
    xy[2] = 31;
    xy[3] = 7;
    xy[4] = ox + 3;
    xy[5] = oy + 66;
    xy[6] = ox + 34;
    xy[7] = oy + 73;
    vrt_cpyfm(h, MD_REPLACE, xy, &src, &dst, cols);

    /* copies of the patterned fill within the workstation's own form */
    xy[0] = ox + 1;
    xy[1] = oy + 1;
    xy[2] = ox + 46;
    xy[3] = oy + 31;
    xy[4] = ox + 2;
    xy[5] = oy + 81;
    xy[6] = ox + 47;
    xy[7] = oy + 111;
    vro_cpyfm(h, S_ONLY, xy, &dst, &dst);
    xy[4] = ox + 49;
    xy[6] = ox + 94;
    vro_cpyfm(h, S_XOR_D, xy, &dst, &dst);

    for (k = 0; k < 6; k++)
    {
        snprintf(text, sizeof(text), "%s accepted on handle %d", what[k], h);
        check(ok[k] == 1, text);
    }
}

/* a pixel of one of the three pictures */
typedef struct {
    const char *name;
    const unsigned char *base;      /* (0, 0) of the picture */
    long pitch;                     /* bytes a line */
} Pic;

static unsigned short px(const Pic *p, short x, short y)
{
    return *(const unsigned short *)(p->base + (long)y * p->pitch + (long)x * 2);
}

/* a box holds only the values a and b (and white if w), at least na of a and nb of b */
static void only(const Pic *p, const char *what, short x1, short y1, short x2, short y2,
                 unsigned short a, unsigned short b, int w, long na, long nb)
{
    long n_a = 0, n_b = 0, n_w = 0, n_o = 0;
    unsigned short first = 0;
    short x, y;
    char text[200];

    for (y = y1; y <= y2; y++)
        for (x = x1; x <= x2; x++)
        {
            unsigned short v = px(p, x, y);

            if (v == a)
                n_a++;
            else if (v == b)
                n_b++;
            else if (w && v == v_white)
                n_w++;
            else
            {
                if (!n_o)
                    first = v;
                n_o++;
            }
        }
    snprintf(text, sizeof(text), "BMRGB %s %s: %04x %ld, %04x %ld, white %ld, other %ld (first %04x)",
             p->name, what, a, n_a, b, n_b, n_w, n_o, first);
    say(text);
    snprintf(text, sizeof(text), "%s, %s: only its colours", p->name, what);
    check(n_o == 0, text);
    snprintf(text, sizeof(text), "%s, %s: both colours drawn as often as expected", p->name, what);
    check(n_a >= na && n_b >= nb, text);
}

/* the checks one picture must pass on its own */
static void check_picture(const Pic *p)
{
    short x, y;
    long bad = 0, n;
    unsigned short pen1 = 0, first = 0;
    char text[200];

    /* the patterned fill: exactly what the pattern gives each pixel */
    for (y = 1; y <= 31; y++)
        for (x = 1; x <= 46; x++)
        {
            unsigned short want = (((unsigned short)udpat[y & 15] >> (15 - (x & 15))) & 1) ? v_ifill : v_pfill;

            if (px(p, x, y) != want && !bad++)
            {
                first = px(p, x, y);
                snprintf(text, sizeof(text), "BMRGB %s pattern fill: first difference at %d,%d: %04x, want %04x",
                         p->name, x, y, first, want);
                say(text);
            }
        }
    snprintf(text, sizeof(text), "%s, user pattern fill: every pixel ink or background as the pattern says (%ld wrong)",
             p->name, bad);
    check(bad == 0, text);

    only(p, "solid fill", 49, 1, 94, 10, v_ifill, v_ifill, 0, 46L * 10, 0);
    only(p, "hollow fill", 49, 12, 94, 20, v_pfill, v_pfill, 0, 46L * 9, 0);
    only(p, "text at a byte boundary", 48, 22, 48 + 3 * cw - 1, 22 + ch - 1, v_itext, v_ptext, 0, 1, 1);
    only(p, "text off a byte boundary", 51, 40, 51 + 3 * cw - 1, 40 + ch - 1, v_itext, v_ptext, 0, 1, 1);
    only(p, "horizontal dashed line", 2, 34, 45, 34, v_iline, v_plin, 0, 1, 1);
    only(p, "horizontal solid line", 2, 36, 45, 36, v_iline, v_iline, 0, 44, 0);
    only(p, "vertical dashed line", 4, 38, 4, 62, v_iline, v_plin, 0, 1, 1);
    only(p, "vertical solid line", 7, 38, 7, 62, v_iline, v_iline, 0, 25, 0);
    only(p, "slanted dashed line", 10, 38, 45, 62, v_iline, v_plin, 1, 1, 1);
    only(p, "slanted solid line", 40, 64, 94, 79, v_iline, v_iline, 1, 55, 0);

    /* the expand: the source's set bits one colour (pen 1), the clear ones pen 0 */
    bad = 0;
    n = 0;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 32; x++)
        {
            unsigned short v = px(p, 3 + x, 66 + y);

            if ((mono[y * 2 + (x >> 4)] >> (15 - (x & 15))) & 1)
            {
                if (!n++)
                    pen1 = v;
                else if (v != pen1)
                    bad++;
            }
            else if (v != v_white)
                bad++;
        }
    snprintf(text, sizeof(text), "BMRGB %s expand: pen 1 %04x, %ld wrong", p->name, pen1, bad);
    say(text);
    snprintf(text, sizeof(text), "%s, vrt_cpyfm expand: the source's bits in pen 1 and pen 0", p->name);
    check(bad == 0 && pen1 != v_white, text);

    /* the copies */
    bad = 0;
    n = 0;
    for (y = 0; y < 31; y++)
        for (x = 0; x < 46; x++)
        {
            unsigned short v = px(p, 1 + x, 1 + y);

            if (px(p, 2 + x, 81 + y) != v)
                bad++;
            if (px(p, 49 + x, 81 + y) != (unsigned short)(v ^ v_white))
                n++;
        }
    snprintf(text, sizeof(text), "%s, vro_cpyfm copy one pixel further on: equal to its source (%ld wrong)", p->name, bad);
    check(bad == 0, text);
    snprintf(text, sizeof(text), "%s, vro_cpyfm xor copy: its source xor white (%ld wrong)", p->name, n);
    check(n == 0, text);
}

static void compare(const Pic *a, const Pic *b)
{
    short x, y;
    long diff = 0;
    char text[200];

    for (y = 0; y < AH; y++)
        for (x = 0; x < AW; x++)
            if (px(a, x, y) != px(b, x, y) && !diff++)
            {
                snprintf(text, sizeof(text), "BMRGB first difference %s/%s at %d,%d: %04x %04x",
                         a->name, b->name, x, y, px(a, x, y), px(b, x, y));
                say(text);
            }
    snprintf(text, sizeof(text), "the %s and the %s are equal pixel for pixel (%ld differ)", a->name, b->name, diff);
    check(diff == 0, text);
}

static short open_vwk(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short ch2, bw, bh, h;

    h = graf_handle(&cw, &ch2, &bw, &bh);
    v_opnvwk(work_in, &h, work_out);
    return h;
}

int main(void)
{
    static const short x0 = 32, y0 = 64;   /* multiples of 16: the pattern and the dashes start alike */
    short work_out[57], planes, ha, hb, d, k, cwb, chb;
    unsigned char *mem_b = (unsigned char *)((((unsigned long)raw_b) + 16 + 15) & ~15UL) + 2;
    GCBITMAP bma, bmb;
    Pic ps, pa, pb;
    MFDB m, s;
    short xy[8];
    char text[200];
    int same;
    long y;

    say("BMRGB start");
    check(appl_init() >= 0, "AES application registration");
    screen = open_vwk();
    check(screen > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(screen, 1, work_out);
    planes = work_out[4];
    if (planes != 16)
    {
        snprintf(text, sizeof(text), "BMRGB %d planes: skipped (16-bit screens only)", planes);
        say(text);
        v_clsvwk(screen);
        appl_exit();
        say("FVDI BMRGB GUEST SKIPPED");
        return 0;
    }

    v_white = 0;
    v_ifill = color2value(screen, ink_fill);
    v_pfill = color2value(screen, paper_fill);
    v_itext = color2value(screen, ink_text);
    v_ptext = color2value(screen, paper_text);
    v_iline = color2value(screen, ink_line);
    v_plin = color2value(screen, paper_line);
    vst_alignment(screen, 0, 5, &d, &d);
    vqt_attributes(screen, work_out);
    cw = work_out[8];
    ch = work_out[9];
    snprintf(text, sizeof(text), "BMRGB fill %04x/%04x text %04x/%04x line %04x/%04x, cell %dx%d",
             v_ifill, v_pfill, v_itext, v_ptext, v_iline, v_plin, cw, ch);
    say(text);
    check(v_ifill != v_pfill && v_itext != v_ptext && v_iline != v_plin && v_pfill != v_ptext &&
          v_pfill != v_plin && v_ptext != v_plin, "the six colours are six pixels");

    /* bitmap A: the VDI's memory, colours of the screen workstation */
    memset(&bma, 0, sizeof(bma));
    bma.xmax = AW;
    bma.ymax = AH;
    ha = v_open_bm(screen, &bma, 0, 0, 0, 0);
    check(ha > 0, "v_open_bm, the VDI's memory");
    /* bitmap B: the program's memory, address and line width 2 mod 4 */
    memset(raw_b, SENTINEL, sizeof(raw_b));
    memset(&bmb, 0, sizeof(bmb));
    bmb.addr = mem_b;
    bmb.width = PITCH_B;
    bmb.xmax = AW;
    bmb.ymax = AH;
    hb = v_open_bm(screen, &bmb, 0, 0, 0, 0);
    check(hb > 0, "v_open_bm, the program's memory at 2 mod 4, lines of 2 mod 4 bytes");
    if (failed)
        goto out;
    snprintf(text, sizeof(text), "BMRGB bitmap A at %p, %ld bytes a line; B at %p, %ld bytes a line",
             bma.addr, bma.width, mem_b, (long)PITCH_B);
    say(text);
    check(bma.addr && bma.width >= AW * 2, "bitmap A described");

    for (k = 0; k < 2; k++)
    {
        short h = k ? hb : ha;

        vst_alignment(h, 0, 5, &d, &d);
        vqt_attributes(h, work_out);
        cwb = work_out[8];
        chb = work_out[9];
        snprintf(text, sizeof(text), "the bitmap %c text cell is the screen's (%dx%d)", 'A' + k, cwb, chb);
        check(cwb == cw && chb == ch, text);
    }

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);

    draw(screen, x0, y0);
    v_white = 0;
    memset(&m, 0, sizeof(m));
    memset(&s, 0, sizeof(s));
    m.fd_addr = scr;
    m.fd_w = AW;
    m.fd_h = AH;
    m.fd_wdwidth = AW / 16;
    m.fd_stand = 0;
    m.fd_nplanes = 16;
    xy[0] = x0;
    xy[1] = y0;
    xy[2] = x0 + AW - 1;
    xy[3] = y0 + AH - 1;
    xy[4] = 0;
    xy[5] = 0;
    xy[6] = AW - 1;
    xy[7] = AH - 1;
    vro_cpyfm(screen, S_ONLY, xy, &s, &m);

    draw(ha, 0, 0);
    draw(hb, 0, 0);

    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);

    ps.name = "screen";
    ps.base = (const unsigned char *)scr;
    ps.pitch = AW * 2;
    pa.name = "bitmap A";
    pa.base = bma.addr;
    pa.pitch = bma.width;
    pb.name = "bitmap B";
    pb.base = mem_b;
    pb.pitch = PITCH_B;

    /* white is pen 0, as the screen holds it (the corner is not drawn on) */
    v_white = px(&ps, 0, 0);
    snprintf(text, sizeof(text), "BMRGB white %04x; bitmap A %04x, B %04x", v_white, px(&pa, 0, 0), px(&pb, 0, 0));
    say(text);
    check(px(&pa, 0, 0) == v_white && px(&pb, 0, 0) == v_white, "pen 0 is the same pixel in all three");

    check_picture(&ps);
    check_picture(&pa);
    check_picture(&pb);
    compare(&ps, &pa);
    compare(&ps, &pb);

    /* bitmap B: the two bytes past each line, and 16 bytes either side */
    same = 1;
    for (k = 0; k < 16; k++)
        if (mem_b[-16 + k] != SENTINEL || mem_b[(long)PITCH_B * AH + k] != SENTINEL)
            same = 0;
    check(same, "bitmap B: the bytes either side are left alone");
    same = 1;
    for (y = 0; y < AH; y++)
        if (mem_b[y * PITCH_B + AW * 2] != SENTINEL || mem_b[y * PITCH_B + AW * 2 + 1] != SENTINEL)
            same = 0;
    check(same, "bitmap B: the bytes past the end of each line are left alone");

    v_clsbm(hb);
    v_clsbm(ha);
    vsf_color(screen, 0);
    vsf_interior(screen, 1);
    vswr_mode(screen, 1);
    rect(screen, x0, y0, x0 + AW - 1, y0 + AH - 1);
    form_dial(FMD_FINISH, 0, 0, 0, 0, 0, 0, x0 + AW, y0 + AH);

out:
    if (screen > 0)
        v_clsvwk(screen);
    appl_exit();
    if (!failed)
        say("FVDI BMRGB GUEST PASS");
    return failed;
}
