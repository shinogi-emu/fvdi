/*
 * aatext_guest.c - anti-aliased outline text, in EmuTOS's VDI.
 *
 * On a 16-bit screen EmuTOS's VDI draws outline text anti-aliased in the
 * replace and transparent writing modes: the text colour is blended into
 * the pixels by how much of each the glyphs cover (vdi/vdi_textaa.c).
 * Checked within the run, with the pixels read back by vro_cpyfm:
 *
 *  1. replace mode paints the text box in the text colour, the text
 *     background colour and blends between them, component by component;
 *     there are blends and fully covered pixels;
 *  2. transparent mode leaves the pixels it does not cover, blends the
 *     others between what was there and the text colour, and over the
 *     background colour of replace mode gives what replace mode gives;
 *  3. xor and reverse transparent stay one-bit: xor complements pixels or
 *     leaves them, twice restores the screen, and reverse transparent
 *     paints exactly the pixels of the text box that xor leaves;
 *  4. the anti-aliased text lies on the one-bit text: the pixels at least
 *     half covered are about as many as xor changes, around the same centre;
 *  5. colours: red text on blue blends red and blue and has no green;
 *  6. clipping at each side and at all four, and the four screen edges:
 *     the visible pixels are those of the whole text, nothing else changes;
 *  7. rotated text is the unrotated text turned, pixel for pixel;
 *  8. the effects: bold and underline darken, light text reaches half the
 *     text colour at most, italic widens, outlined text is hollow;
 *  9. text drawn into an off-screen bitmap (with the EdDI cookie) is what
 *     the screen gets;
 * 10. v_justified draws blended text over the length asked for.
 *
 * Then it times a string at 16 and 22 pixels (the size of the em), drawn
 * again and again: ticks of the 200 Hz timer per string.
 *
 * On a screen of 8 planes or fewer, outline text must stay one-bit: replace
 * mode paints two colours only.
 *
 * C:\VTEST\AATEXT.CFG, if present, holds options separated by blanks:
 *   mono=ADDR   the address (hex) of the VDI's vdi_text_mono switch, a
 *               WORD (see EmuTOS's emutos.map): set, replace mode must
 *               paint two colours only, in exactly the pixels xor changes,
 *               and blends must come back once it is cleared; the timing
 *               then times one-bit text too
 *   coldmono    with mono=: time the first strings of new sizes one-bit
 *   face=NAME   use the face named NAME (to the end of the line) instead
 *               of Liberation Sans
 *   sample=N    after the checks, show the sample screen for N seconds:
 *               the same strings at 16, 22 and 30 pixels, one-bit on the
 *               left (with mono=) and anti-aliased on the right, in each
 *               face listed by sface= options (up to four, each to the end
 *               of its line), with "AATEXT SAMPLE READY" on the serial line
 *               when it is drawn and "AATEXT SAMPLE DONE" after
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI AATEXT GUEST PASS" or "FVDI AATEXT FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WHITE   0               /* pens */
#define BLACK   1
#define RED     2
#define GREEN   3
#define BLUE    4

#define MAX_SFACES  4
#define EDDI_COOKIE 0x45644449UL    /* 'EdDI': the VDI has off-screen bitmaps */

typedef struct {
    long magic, length, format, reserved;
    unsigned char *addr;
    long width, bits;
    unsigned long px_format;
    long xmin, ymin, xmax, ymax;
    void *ctab, *itab;
    long reserved0, reserved1;
} GCBITMAP;

void v_clsbm(short handle);
short vst_arbpt(short handle, short point, short *wchar, short *hchar, short *wcell, short *hcell);

typedef struct {
    short x, y, w, h;           /* screen rectangle */
    short wdw;                  /* words per row and plane */
    unsigned short *data;
} Grab;

static int failed;
static short handle, scr_w, scr_h, planes, dpi;
static unsigned long p_white, p_black, p_red, p_green, p_blue;
static unsigned long mono_addr;
static short coldmono, sample_secs, sface_count;
static char want_face[40];
static char sfaces[MAX_SFACES][40];
static long eddi_found;

static const char *const TEXT = "Hamburgefonts";
static const char *const LONG_LINE = "The quick brown fox jumps over the lazy dog";
static const char *LINE;        /* the timed string: LONG_LINE, or a shorter one on a narrow screen */

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

    snprintf(line, sizeof(line), "AATEXT %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI AATEXT FAIL: %s", what);
        say(line);
    }
}

static long read_hz200(void)
{
    return *(volatile long *)0x4ba;
}

static long ticks(void)
{
    return Supexec(read_hz200);
}

static long find_eddi(void)
{
    long *jar = *(long **)0x5A0L;

    eddi_found = 0;
    while (jar && jar[0])
    {
        if ((unsigned long)jar[0] == EDDI_COOKIE)
            eddi_found = 1;
        jar += 2;
    }
    return 0;
}

/* the VDI's switch: one-bit outline text when set */
static short mono_value;

static long poke_mono(void)
{
    *(volatile short *)mono_addr = mono_value;
    return 0;
}

static void set_mono(short on)
{
    if (!mono_addr)
        return;
    mono_value = on;
    Supexec(poke_mono);
}

static void copy_line(char *dst, short size, const char *p)
{
    short i;

    for (i = 0; i < size - 1 && *p && *p != '\r' && *p != '\n'; i++)
        dst[i] = *p++;
    dst[i] = 0;
}

static void read_config(void)
{
    static char buf[600];
    char *p;
    long fh, n;

    fh = Fopen("C:\\VTEST\\AATEXT.CFG", 0);
    if (fh < 0)
        return;
    n = Fread((short)fh, sizeof(buf) - 1, buf);
    Fclose((short)fh);
    if (n <= 0)
        return;
    buf[n] = 0;
    if ((p = strstr(buf, "mono=")) != NULL)
        mono_addr = strtoul(p + 5, NULL, 16);
    if (strstr(buf, "coldmono"))
        coldmono = 1;
    if ((p = strstr(buf, "sample=")) != NULL)
        sample_secs = atoi(p + 7);
    for (p = buf; (p = strstr(p, "face=")) != NULL; p += 5)
    {
        if (p > buf && p[-1] == 's')
        {
            if (sface_count < MAX_SFACES)
                copy_line(sfaces[sface_count++], sizeof(sfaces[0]), p + 5);
        }
        else
            copy_line(want_face, sizeof(want_face), p + 5);
    }
}

/*
 * ===========================================================================
 * The screen
 * ===========================================================================
 */

static void clip_off(void)
{
    vs_clip_off(handle);
}

static int grab(Grab *g, short x, short y, short w, short h)
{
    MFDB scr, mem;
    short pxy[8];
    long size;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    g->x = x; g->y = y; g->w = w; g->h = h;
    g->data = NULL;
    if (w <= 0 || h <= 0)
        return 0;
    g->wdw = (w + 15) >> 4;
    size = (long)g->wdw * 2 * planes * h;
    g->data = (unsigned short *)Malloc(size);
    if ((long)g->data <= 0)
    {
        g->data = NULL;
        return 0;
    }
    memset(&scr, 0, sizeof(scr));
    memset(&mem, 0, sizeof(mem));
    mem.fd_addr = g->data;
    mem.fd_w = g->wdw * 16;
    mem.fd_h = h;
    mem.fd_wdwidth = g->wdw;
    mem.fd_stand = 0;
    mem.fd_nplanes = planes;
    pxy[0] = x; pxy[1] = y; pxy[2] = x + w - 1; pxy[3] = y + h - 1;
    pxy[4] = 0; pxy[5] = 0; pxy[6] = w - 1; pxy[7] = h - 1;
    clip_off();
    vro_cpyfm(handle, S_ONLY, pxy, &scr, &mem);
    return 1;
}

static void ungrab(Grab *g)
{
    if (g->data)
        Mfree(g->data);
    g->data = NULL;
}

/* the pixel at screen sx,sy, which must be in the grab */
static unsigned long at(const Grab *g, short sx, short sy)
{
    short x = sx - g->x, y = sy - g->y, p;
    const unsigned short *row = g->data + (long)y * g->wdw * planes;
    unsigned short bit;
    unsigned long v = 0;

    if (planes == 16)
        return row[x];
    row += (x >> 4) * planes;
    bit = 0x8000 >> (x & 15);
    for (p = 0; p < planes; p++)
        if (row[p] & bit)
            v |= 1UL << p;
    return v;
}

static int inside(const Grab *g, short sx, short sy)
{
    return sx >= g->x && sy >= g->y && sx < g->x + g->w && sy < g->y + g->h;
}

static void fill(short x0, short y0, short x1, short y1, short colour)
{
    short box[4];

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= scr_w) x1 = scr_w - 1;
    if (y1 >= scr_h) y1 = scr_h - 1;
    if (x1 < x0 || y1 < y0)
        return;
    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
    clip_off();
    vswr_mode(handle, 1);
    vsf_interior(handle, 1);
    vsf_color(handle, colour);
    vsf_perimeter(handle, 0);
    vr_recfl(handle, box);
}

static unsigned long colour_value(short colour)
{
    Grab g;
    unsigned long v = 0;

    fill(0, 0, 3, 3, colour);
    if (grab(&g, 0, 0, 4, 4))
    {
        v = at(&g, 1, 1);
        ungrab(&g);
    }
    return v;
}

/* draw in the current font, writing mode, effects and rotation, top left at x,y */
static void text(short x, short y, const char *s, short mode, short effects, short angle)
{
    vswr_mode(handle, mode);
    vst_effects(handle, effects);
    vst_rotation(handle, angle);
    v_gtext(handle, x, y, s);
    vst_rotation(handle, 0);
    vst_effects(handle, 0);
}

/* the components of a 16-bit pixel, rrrrrggggg0bbbbb */
#define R(p) ((short)(((p) >> 11) & 31))
#define G(p) ((short)(((p) >> 6) & 31))
#define B(p) ((short)((p) & 31))

static int between1(short v, short a, short b)
{
    return (a <= b) ? (v >= a && v <= b) : (v >= b && v <= a);
}

/* is p, component by component, between a and b? */
static int between(unsigned long p, unsigned long a, unsigned long b)
{
    return between1(R(p), R(a), R(b)) && between1(G(p), G(a), G(b)) && between1(B(p), B(a), B(b));
}

/* how dark a pixel is, 0 (white) to 93 (black) */
static short darkness(unsigned long p)
{
    return (short)(93 - R(p) - G(p) - B(p));
}

/* the box of the pixels of a grab that are not back */
static int changed_box(const Grab *g, unsigned long back, short *x0, short *y0, short *x1, short *y1)
{
    short x, y;

    *x0 = 32767; *y0 = 32767; *x1 = -32768; *y1 = -32768;
    for (y = g->y; y < g->y + g->h; y++)
        for (x = g->x; x < g->x + g->w; x++)
            if (at(g, x, y) != back)
            {
                if (x < *x0) *x0 = x;
                if (x > *x1) *x1 = x;
                if (y < *y0) *y0 = y;
                if (y > *y1) *y1 = y;
            }
    return *x1 >= *x0;
}

/*
 * draw s at x,y in a cleared area of w x h around it (filled with back)
 * and grab the area
 */
static int draw_grab(Grab *g, short x, short y, short w, short h, const char *s,
                     short mode, short effects, short angle, short back)
{
    fill(x - 8, y - 8, x + w, y + h, back);
    text(x, y, s, mode, effects, angle);
    return grab(g, x - 8, y - 8, w + 9, h + 9);
}

/* the points whose em the VDI makes closest to px pixels, and that em */
static short points_for(short px, short *em_out)
{
    short pt, best = 1, best_em = 1, best_d = 32767;

    for (pt = 1; pt < 300; pt++)
    {
        short em = (short)(((long)pt * dpi + 36) / 72);
        short d = em > px ? em - px : px - em;

        if (d < best_d)
        {
            best_d = d;
            best = pt;
            best_em = em;
        }
    }
    if (em_out)
        *em_out = best_em;
    return best;
}

/* the size of the em of the text drawn next, on workstation h */
static void set_em_on(short h, short px)
{
    short cw, ch, bw, bh;

    vst_arbpt(h, points_for(px, NULL), &cw, &ch, &bw, &bh);
}

static void set_em(short px)
{
    set_em_on(handle, px);
}

static short em_of(short px)
{
    short em;

    points_for(px, &em);
    return em;
}

/*
 * ===========================================================================
 * The checks
 * ===========================================================================
 */

static short ext_w, ext_h;      /* of TEXT at the size of the checks */
static short check_em;          /* that size, the em in pixels */

static void measure(const char *s)
{
    short ext[8];

    vqt_extent(handle, s, ext);
    ext_w = ext[2];
    ext_h = ext[5];
}

/* 1, 2, 3, 4: the writing modes */
static void modes_check(void)
{
    Grab rep, trn, trw, xr, xr2, er;
    short x = 40, y = 40, w = ext_w + 16, h = ext_h + 16, i, j;
    long blends = 0, full = 0, odd = 0, outside = 0, trans_bad = 0, trans_same = 0, trans_changed = 0;
    long rep_marked = 0, xor_bad = 0, xor_changed = 0, erase_bad = 0, both = 0;
    long mono_hit = 0, aa_dark = 0, dark_hit = 0;
    short bx0, by0, bx1, by1;
    char line[200];

    /* 1. replace over red: the box in white, black and greys */
    if (!draw_grab(&rep, x, y, w, h, TEXT, 1, 0, 0, RED))
    {
        check(0, "text drawn in replace mode");
        return;
    }
    changed_box(&rep, p_red, &bx0, &by0, &bx1, &by1);
    snprintf(line, sizeof(line), "AATEXT replace: box %d,%d %d x %d (extent %d x %d)", bx0, by0,
             bx1 - bx0 + 1, by1 - by0 + 1, ext_w, ext_h);
    say(line);
    check(bx1 - bx0 + 1 == ext_w && by1 - by0 + 1 == ext_h && by0 == y,
          "replace mode paints the box of vqt_extent");
    for (j = rep.y; j < rep.y + rep.h; j++)
        for (i = rep.x; i < rep.x + rep.w; i++)
        {
            unsigned long p = at(&rep, i, j);

            if (i < bx0 || i > bx1 || j < by0 || j > by1)
            {
                if (p != p_red)
                    outside++;
                continue;
            }
            if (p == p_black)
                full++;
            else if (p != p_white)
            {
                blends++;
                if (!between(p, p_white, p_black) || (p & 0x20))
                    odd++;
            }
        }
    snprintf(line, sizeof(line), "AATEXT replace: %ld text pixels, %ld blends, %ld not between, %ld outside",
             full, blends, odd, outside);
    say(line);
    check(full > 0 && blends > 0, "replace mode draws full pixels and blends");
    check(odd == 0, "the blends lie between the text and background colours");
    check(outside == 0, "nothing outside the text box changes");

    /* 2. transparent over red, and over white */
    if (draw_grab(&trn, x, y, w, h, TEXT, 2, 0, 0, RED) &&
        draw_grab(&trw, x, y, w, h, TEXT, 2, 0, 0, WHITE))
    {
        for (j = rep.y; j < rep.y + rep.h; j++)
            for (i = rep.x; i < rep.x + rep.w; i++)
            {
                unsigned long r = at(&rep, i, j), t = at(&trn, i, j), u = at(&trw, i, j);

                /* where replace leaves the background colour, transparent leaves the pixel */
                if (r == p_white || r == p_red)
                {
                    if (t != p_red)
                        trans_bad++;
                }
                else
                {
                    trans_changed++;
                    if (!between(t, p_red, p_black) || t == p_red)
                        trans_bad++;
                }
                /* over the background colour it is replace mode */
                if (u != ((r == p_red) ? p_white : r))
                    trans_same++;
            }
        snprintf(line, sizeof(line), "AATEXT transparent: %ld pixels blended, %ld wrong, %ld unlike replace",
                 trans_changed, trans_bad, trans_same);
        say(line);
        check(trans_changed > 0 && trans_bad == 0,
              "transparent mode blends the text over what is there, and leaves the rest");
        check(trans_same == 0, "transparent over the background colour is replace mode");
        ungrab(&trn);
        ungrab(&trw);
    }
    else
        check(0, "text drawn in transparent mode");

    /* 3. xor and reverse transparent stay one-bit */
    if (draw_grab(&xr, x, y, w, h, TEXT, 3, 0, 0, RED))
    {
        text(x, y, TEXT, 3, 0, 0);
        grab(&xr2, x - 8, y - 8, w + 9, h + 9);
        draw_grab(&er, x, y, w, h, TEXT, 4, 0, 0, RED);
        for (j = rep.y; j < rep.y + rep.h; j++)
            for (i = rep.x; i < rep.x + rep.w; i++)
            {
                unsigned long a = at(&xr, i, j), e = at(&er, i, j);
                int in_box = i >= bx0 && i <= bx1 && j >= by0 && j <= by1;

                if (a != p_red)
                {
                    xor_changed++;
                    if (a != (~p_red & 0xffff) || !in_box)
                        xor_bad++;
                }
                if (at(&xr2, i, j) != p_red)
                    xor_bad++;
                if (e != p_red)
                {
                    if (e != p_black || !in_box)
                        erase_bad++;
                    if (a != p_red)
                        both++;
                }
                else if (in_box && a == p_red)
                    erase_bad++;
                /* one-bit pixels that are at least a quarter dark anti-aliased,
                 * and anti-aliased pixels at least three quarters dark that are
                 * one-bit pixels */
                if (in_box && a != p_red && darkness(at(&rep, i, j)) >= 23)
                    mono_hit++;
                if (in_box && darkness(at(&rep, i, j)) >= 70)
                {
                    aa_dark++;
                    if (a != p_red)
                        dark_hit++;
                }
                if (at(&rep, i, j) != p_white && at(&rep, i, j) != p_red)
                    rep_marked++;
            }
        snprintf(line, sizeof(line), "AATEXT xor: %ld pixels complemented, %ld wrong; reverse transparent: %ld wrong, %ld both",
                 xor_changed, xor_bad, erase_bad, both);
        say(line);
        check(xor_changed > 0 && xor_bad == 0, "xor mode stays one-bit and twice restores the screen");
        check(erase_bad == 0 && both == 0, "reverse transparent paints exactly the box pixels xor leaves");

        /* 4. on the one-bit text: a shift by a pixel would miss half */
        snprintf(line, sizeof(line), "AATEXT one-bit %ld pixels, %ld of them a quarter dark anti-aliased; %ld anti-aliased three quarters dark, %ld of them one-bit (%ld touched)",
                 xor_changed, mono_hit, aa_dark, dark_hit, rep_marked);
        say(line);
        check(xor_changed > 0 && aa_dark > 0 && mono_hit * 100 >= xor_changed * 97 && dark_hit * 100 >= aa_dark * 97,
              "the anti-aliased text lies on the one-bit text");
        ungrab(&xr);
        ungrab(&xr2);
        ungrab(&er);
    }
    else
        check(0, "text drawn in xor mode");

    /* the switch: one-bit text where it was anti-aliased */
    if (mono_addr)
    {
        Grab mono, back;
        long two = 0, differ = 0;

        set_mono(1);
        draw_grab(&mono, x, y, w, h, TEXT, 1, 0, 0, RED);
        draw_grab(&xr, x, y, w, h, TEXT, 3, 0, 0, RED);
        set_mono(0);
        draw_grab(&back, x, y, w, h, TEXT, 1, 0, 0, RED);
        for (j = rep.y; j < rep.y + rep.h; j++)
            for (i = rep.x; i < rep.x + rep.w; i++)
            {
                unsigned long m = at(&mono, i, j);
                int in_box = i >= bx0 && i <= bx1 && j >= by0 && j <= by1;

                if (in_box ? (m != p_white && m != p_black) : (m != p_red))
                    two++;
                if ((m == p_black) != (at(&xr, i, j) != p_red))
                    differ++;
                if (at(&back, i, j) != at(&rep, i, j))
                    differ++;
            }
        snprintf(line, sizeof(line), "AATEXT switched to one-bit: %ld pixels of a third colour, %ld unlike xor or the first drawing",
                 two, differ);
        say(line);
        check(two == 0, "switched off, replace mode paints two colours");
        check(differ == 0, "switched off, the text is xor's one-bit text; switched on, it is anti-aliased again");
        ungrab(&mono);
        ungrab(&xr);
        ungrab(&back);
    }
    ungrab(&rep);
}

/* 5. colours */
static void colour_check(void)
{
    Grab g;
    short x = 40, y = 40, w = ext_w + 16, h = ext_h + 16, i, j;
    long changed = 0, bad = 0, blends = 0;
    char line[160];

    vst_color(handle, RED);
    if (draw_grab(&g, x, y, w, h, TEXT, 2, 0, 0, BLUE))
    {
        for (j = g.y; j < g.y + g.h; j++)
            for (i = g.x; i < g.x + g.w; i++)
            {
                unsigned long p = at(&g, i, j);

                if (p == p_blue)
                    continue;
                changed++;
                if (p != p_red)
                    blends++;
                if (!between(p, p_red, p_blue))
                    bad++;
            }
        ungrab(&g);
    }
    vst_color(handle, BLACK);
    snprintf(line, sizeof(line), "AATEXT red on blue: %ld pixels, %ld blends, %ld not between (red %lx, blue %lx)",
             changed, blends, bad, p_red, p_blue);
    say(line);
    check(changed > 0 && blends > 0 && bad == 0, "red text on blue blends between the two");
}

/*
 * 6. clipping: the text at x,y (top left) under a clip rectangle (or none),
 * compared with the whole text drawn at rx,ry
 */
static long clip_case(short x, short y, const short *clip, short mode, const Grab *ref,
                      short rx, short ry, long *visible)
{
    Grab g;
    short w = ext_w, h = ext_h, i, j;
    long wrong = 0;
    short back = (mode == 1) ? GREEN : RED;
    unsigned long pback = (mode == 1) ? p_green : p_red;

    *visible = 0;
    fill(x - 20, y - 20, x + w + 20, y + h + 20, back);
    if (clip)
        vs_clip(handle, 1, (short *)clip);
    else
        clip_off();
    vswr_mode(handle, mode);
    v_gtext(handle, x, y, TEXT);
    clip_off();
    if (!grab(&g, x - 20, y - 20, w + 41, h + 41))
        return -1;
    for (j = g.y; j < g.y + g.h; j++)
        for (i = g.x; i < g.x + g.w; i++)
        {
            int shown = i >= x && i < x + w && j >= y && j < y + h;
            unsigned long want;

            if (clip && (i < clip[0] || i > clip[2] || j < clip[1] || j > clip[3]))
                shown = 0;
            want = shown ? at(ref, rx + (i - x), ry + (j - y)) : pback;
            if (shown)
                (*visible)++;
            if (at(&g, i, j) != want)
                wrong++;
        }
    ungrab(&g);
    return wrong;
}

static void clip_check(void)
{
    Grab ref[2];
    short rx = 20, ry = scr_h - ext_h - 40, mode, k;
    short x = 40, y = 120, w = ext_w, h = ext_h;
    char line[200];

    for (mode = 1; mode <= 2; mode++)
    {
        short back = (mode == 1) ? GREEN : RED;
        long vis, wrong;
        short cl[4];
        static const char *const what[] = {
            "left half", "right half", "top half", "bottom half", "all four sides", "beside the text"
        };

        fill(rx - 20, ry - 20, rx + w + 20, ry + h + 20, back);
        text(rx, ry, TEXT, mode, 0, 0);
        if (!grab(&ref[mode - 1], rx, ry, w, h))
        {
            check(0, "the whole text for the clipping");
            return;
        }

        for (k = 0; k < 6; k++)
        {
            cl[0] = x; cl[1] = y; cl[2] = x + w - 1; cl[3] = y + h - 1;
            switch (k)
            {
            case 0: cl[2] = x + w / 2; break;
            case 1: cl[0] = x + w / 2; break;
            case 2: cl[3] = y + h / 2; break;
            case 3: cl[1] = y + h / 2; break;
            case 4: cl[0] = x + 5; cl[1] = y + 3; cl[2] = x + w - 6; cl[3] = y + h - 4; break;
            default: cl[0] = x + w + 2; cl[2] = x + w + 30; break;
            }
            wrong = clip_case(x, y, cl, mode, &ref[mode - 1], rx, ry, &vis);
            snprintf(line, sizeof(line), "AATEXT clip %s (%s): %ld visible, %ld wrong", what[k],
                     mode == 1 ? "replace" : "transparent", vis, wrong);
            say(line);
            snprintf(line, sizeof(line), "%s mode, clip rectangle %s: the visible part of the whole text",
                     mode == 1 ? "replace" : "transparent", what[k]);
            check(wrong == 0 && (k == 5 ? vis == 0 : vis > 0), line);
        }

        /* the screen edges, clipping off */
        {
            short ex[4], ey[4];

            ex[0] = -w / 2;         ey[0] = 300;
            ex[1] = scr_w - w / 2;  ey[1] = 300;
            ex[2] = 300;            ey[2] = -h / 2;
            ex[3] = 300;            ey[3] = scr_h - h / 2;
            for (k = 0; k < 4; k++)
            {
                static const char *const edge[] = { "left", "right", "top", "bottom" };

                wrong = clip_case(ex[k], ey[k], NULL, mode, &ref[mode - 1], rx, ry, &vis);
                snprintf(line, sizeof(line), "AATEXT %s edge (%s): %ld visible, %ld wrong", edge[k],
                         mode == 1 ? "replace" : "transparent", vis, wrong);
                say(line);
                snprintf(line, sizeof(line), "%s mode, across the %s edge of the screen: the visible part",
                         mode == 1 ? "replace" : "transparent", edge[k]);
                check(wrong == 0 && vis > 0 && vis < (long)w * h, line);
            }
        }
        ungrab(&ref[mode - 1]);
    }
}

/* 7. rotation */
static void rotation_check(void)
{
    Grab ref, g;
    short x = scr_w / 2, y = scr_h / 2, w = ext_w, h = ext_h, k, i, j;
    char line[160];

    fill(x - 20, y - 20, x + w + 20, y + h + 20, RED);
    text(x, y, TEXT, 1, 0, 0);
    if (!grab(&ref, x, y, w, h))
        return;
    for (k = 1; k <= 3; k++)
    {
        short angle = k * 900, x0, y0, x1, y1;
        long wrong = 0;

        fill(x - w - 40, y - w - 40, x + w + 40, y + w + 40, RED);
        text(x, y, TEXT, 1, 0, angle);
        if (!grab(&g, x - w - 30, y - w - 30, 2 * w + 61, 2 * w + 61))
            continue;
        changed_box(&g, p_red, &x0, &y0, &x1, &y1);
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++)
            {
                short tx, ty;

                if (angle == 900)
                    tx = x0 + j, ty = y1 - i;
                else if (angle == 1800)
                    tx = x1 - i, ty = y1 - j;
                else
                    tx = x1 - j, ty = y0 + i;
                if (!inside(&g, tx, ty) || at(&g, tx, ty) != at(&ref, x + i, y + j))
                    wrong++;
            }
        snprintf(line, sizeof(line), "AATEXT rotation %d: box %d x %d, %ld pixels unlike the turned text",
                 angle, x1 - x0 + 1, y1 - y0 + 1, wrong);
        say(line);
        snprintf(line, sizeof(line), "rotation %d: the text turned, pixel for pixel", angle);
        check(wrong == 0 && (angle == 1800 ? (x1 - x0 + 1 == w && y1 - y0 + 1 == h)
                                           : (x1 - x0 + 1 == h && y1 - y0 + 1 == w)), line);
        ungrab(&g);
    }
    ungrab(&ref);
}

/* the darkness of the text box drawn in replace mode on white, and its width */
static long dark_sum(short effects, short *width, short *min_r, long *full)
{
    Grab g;
    short x = 40, y = 40, ext[8], i, j, x0, y0, x1, y1;
    long sum = 0;

    vst_effects(handle, effects);
    vqt_extent(handle, TEXT, ext);
    vst_effects(handle, 0);
    *min_r = 31;
    *full = 0;
    if (!draw_grab(&g, x, y, ext[2] + 30, ext[5] + 16, TEXT, 1, effects, 0, RED))
        return -1;
    changed_box(&g, p_red, &x0, &y0, &x1, &y1);
    *width = x1 - x0 + 1;
    for (j = g.y; j < g.y + g.h; j++)
        for (i = g.x; i < g.x + g.w; i++)
        {
            unsigned long p = at(&g, i, j);

            if (p == p_red)
                continue;
            sum += darkness(p);
            if (R(p) < *min_r)
                *min_r = R(p);
            if (p == p_black)
                (*full)++;
        }
    ungrab(&g);
    return sum;
}

/* 8. effects */
static void effects_check(void)
{
    short e, w0, w, min_r;
    long d0, d, full;
    char line[200];

    set_em(check_em);
    d0 = dark_sum(0, &w0, &min_r, &full);
    for (e = 1; e < 32; e++)
    {
        d = dark_sum(e, &w, &min_r, &full);
        snprintf(line, sizeof(line), "AATEXT effects %2d: width %d (plain %d), darkness %ld (plain %ld), darkest red %d, %ld full",
                 e, w, w0, d, d0, min_r, full);
        say(line);
        if (d < 0)
        {
            check(0, "text drawn with effects");
            continue;
        }
        if (e == 1)
            check(w > w0 && d > d0, "bold widens and darkens");
        if (e == 2)
            check(full == 0 && min_r >= 14 && min_r <= 17,
                  "light text reaches half the text colour, no more");
        if (e == 4)
            check(w > w0, "italic widens");
        if (e == 8)
            check(d > d0, "underline darkens");
        if (e & 2)
            check(full == 0, "light text never reaches the text colour");
    }

    /* outlined: a big H is hollow */
    {
        Grab g;
        short ext[8], x = 40, y = 40, x0, y0, x1, y1, i, j, stem = 0, inner = 0;
        long dark_inner = 0;

        set_em(100);
        vqt_extent(handle, "H", ext);
        if (draw_grab(&g, x, y, ext[2] + 20, ext[5] + 16, "H", 1, 0, 0, WHITE))
        {
            /* the left stem: the first dark run of a row above the bar */
            short my = y + ext[5] * 3 / 10;

            changed_box(&g, p_white, &x0, &y0, &x1, &y1);
            for (i = x0; i <= x1 && darkness(at(&g, i, my)) < 47; i++)
                ;
            for (j = i; j <= x1 && darkness(at(&g, j, my)) >= 47; j++)
                ;
            stem = j - i;
            ungrab(&g);
            if (stem > 6 && draw_grab(&g, x, y, ext[2] + 20, ext[5] + 16, "H", 1, 16, 0, WHITE))
            {
                /* the middle of the stem stays white, in rows above the bar */
                for (j = my - 4; j <= my + 4; j++)
                    for (x0 = i + 3; x0 < i + stem - 3; x0++)
                    {
                        inner++;
                        dark_inner += darkness(at(&g, x0, j));
                    }
                ungrab(&g);
            }
        }
        snprintf(line, sizeof(line), "AATEXT outlined 'H' at 100 pixels: stem %d, darkness %ld over %d inner pixels",
                 stem, dark_inner, inner);
        say(line);
        check(stem > 6 && inner > 0 && dark_inner == 0, "outlined text is hollow");
    }
}

/* v_open_bm (100 sub 3) with room for what v_opnvwk answers */
static short open_bm(short base, GCBITMAP *bm)
{
    static short contrl[16], intin[16], ptsin[16], intout[64], ptsout[16];
    static void *pb[5];

    memset(contrl, 0, sizeof(contrl));
    contrl[0] = 100;
    contrl[3] = 4;
    contrl[5] = 3;
    contrl[6] = base;
    *(GCBITMAP **)&contrl[7] = bm;
    intin[0] = 1;               /* the colours of the screen */
    intin[1] = intin[2] = intin[3] = 0;
    pb[0] = contrl; pb[1] = intin; pb[2] = ptsin; pb[3] = intout; pb[4] = ptsout;
    __asm__ volatile ("move.l %0,%%d1\n\tmove.w #115,%%d0\n\ttrap #2"
                      : : "g"(pb) : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
    return contrl[6];
}

/* 9. into an off-screen bitmap */
static void bitmap_check(short id, short em)
{
    GCBITMAP bm;
    Grab g;
    short bmh, mode, i, j, ha, va;
    long wrong;
    char line[160];

    /*
     * EmuTOS's VDI has off-screen bitmaps with its NVDI calls but adds no
     * EdDI cookie, so v_open_bm is tried: a VDI without them opens a
     * workstation instead, writes its 45 work_out words, and leaves the
     * GCBITMAP alone.  So it is called here with room for them, not
     * through the binding, which has room for one.
     */
    Supexec(find_eddi);
    memset(&bm, 0, sizeof(bm));
    bm.xmax = ext_w + 40;
    bm.ymax = ext_h + 40;
    bmh = open_bm(handle, &bm);
    if (bmh > 0 && !bm.addr)
    {
        v_clsvwk(bmh);
        bmh = 0;
    }
    if (bmh <= 0)
    {
        check(!eddi_found, "an off-screen bitmap, as the EdDI cookie says");
        say("AATEXT no off-screen bitmaps - bitmap check skipped");
        return;
    }
    vst_font(bmh, id);
    set_em_on(bmh, em);
    vst_alignment(bmh, 0, 5, &ha, &va);
    vst_color(bmh, BLACK);
    for (mode = 1; mode <= 2; mode++)
    {
        short box[4] = { 0, 0, 0, 0 };

        box[2] = (short)bm.xmax;
        box[3] = (short)bm.ymax;
        vswr_mode(bmh, 1);
        vsf_interior(bmh, 1);
        vsf_color(bmh, RED);
        vsf_perimeter(bmh, 0);
        vr_recfl(bmh, box);
        vswr_mode(bmh, mode);
        v_gtext(bmh, 20, 20, TEXT);

        draw_grab(&g, 100, 100, ext_w + 16, ext_h + 16, TEXT, mode, 0, 0, RED);
        wrong = 0;
        for (j = 0; j < ext_h + 8; j++)
            for (i = 0; i < ext_w + 8; i++)
            {
                unsigned short p = ((unsigned short *)(bm.addr + (long)(20 - 4 + j) * bm.width))[20 - 4 + i];

                if (p != at(&g, 100 - 4 + i, 100 - 4 + j))
                    wrong++;
            }
        ungrab(&g);
        snprintf(line, sizeof(line), "AATEXT bitmap, %s: %ld pixels unlike the screen",
                 mode == 1 ? "replace" : "transparent", wrong);
        say(line);
        snprintf(line, sizeof(line), "%s mode into an off-screen bitmap draws what the screen gets",
                 mode == 1 ? "replace" : "transparent");
        check(wrong == 0, line);
    }
    v_clsbm(bmh);
}

/* 10. v_justified */
static void justified_check(void)
{
    Grab g;
    short x = 40, y = 40, len = ext_w * 3 / 2, x0, y0, x1, y1, i, j;
    long blends = 0;
    char line[160];

    fill(x - 8, y - 8, x + len + 30, y + ext_h + 8, RED);
    vswr_mode(handle, 1);
    v_justified(handle, x, y, "Ham burg fonts", len, 1, 1);
    if (!grab(&g, x - 8, y - 8, len + 39, ext_h + 17))
        return;
    changed_box(&g, p_red, &x0, &y0, &x1, &y1);
    for (j = g.y; j < g.y + g.h; j++)
        for (i = g.x; i < g.x + g.w; i++)
        {
            unsigned long p = at(&g, i, j);

            if (p != p_red && p != p_white && p != p_black)
                blends++;
        }
    ungrab(&g);
    snprintf(line, sizeof(line), "AATEXT v_justified to %d (extent %d): drawn %d-%d, %ld blends", len, ext_w,
             x0, x1, blends);
    say(line);
    check(x1 - x0 + 1 >= len - 2 && x1 - x0 + 1 <= len + ext_w / 4 && blends > 0,
          "v_justified draws blended text over the length");
}

/* on 8 planes or fewer: one-bit text */
static void planes_check(void)
{
    Grab g;
    short i, j;
    long other = 0, text_px = 0;
    char line[160];

    if (!draw_grab(&g, 40, 40, ext_w + 16, ext_h + 16, TEXT, 1, 0, 0, RED))
        return;
    for (j = g.y; j < g.y + g.h; j++)
        for (i = g.x; i < g.x + g.w; i++)
        {
            unsigned long p = at(&g, i, j);

            if (p == p_black)
                text_px++;
            else if (p != p_white && p != p_red)
                other++;
        }
    ungrab(&g);
    snprintf(line, sizeof(line), "AATEXT %d planes: %ld text pixels, %ld of other colours", planes, text_px, other);
    say(line);
    check(text_px > 0 && other == 0, "outline text stays one-bit on 8 planes or fewer");
}

/*
 * ===========================================================================
 * Timing
 * ===========================================================================
 */

/*
 * draw the line for at least 2 s, each time on a white background (the
 * time of that fill is taken out), or over itself (busy: every pixel under
 * the text another colour); microseconds per string
 */
static long time_loop(short mode, short text_on, short clear)
{
    short box[4];
    long t0, t, n = 0;

    box[0] = 0; box[1] = 390; box[2] = scr_w - 1; box[3] = 440;
    vsf_interior(handle, 1);
    vsf_color(handle, WHITE);
    vsf_perimeter(handle, 0);
    vswr_mode(handle, mode);
    v_gtext(handle, 20, 400, LINE);         /* cached, and the size ready */
    t0 = ticks();
    do
    {
        if (clear)
        {
            vswr_mode(handle, 1);
            vr_recfl(handle, box);
            vswr_mode(handle, mode);
        }
        if (text_on)
            v_gtext(handle, 20, 400, LINE);
        n++;
        t = ticks() - t0;
    } while (t < 400 || n < 4);

    return (t * 5000L) / n;
}

static long time_line(short mode)
{
    return time_loop(mode, 1, 1) - time_loop(mode, 0, 1);
}

static long time_busy(void)
{
    return time_loop(2, 1, 0);
}

static void timing(void)
{
    static const short sizes[2] = { 16, 22 };
    short k;
    char line[220];

    for (k = 0; k < 2; k++)
    {
        long aa_rep, aa_trn, aa_busy, mono_rep = 0, mono_trn = 0, mono_busy = 0;

        set_em(sizes[k]);
        {
            short ext[8];

            LINE = LONG_LINE;
            vqt_extent(handle, LINE, ext);
            if (ext[2] > scr_w - 40)
                LINE = "The quick brown fox";
        }
        fill(0, 380, scr_w - 1, 460, WHITE);
        aa_rep = time_line(1);
        aa_trn = time_line(2);
        aa_busy = time_busy();
        if (mono_addr)
        {
            set_mono(1);
            mono_rep = time_line(1);
            mono_trn = time_line(2);
            mono_busy = time_busy();
            set_mono(0);
        }
        snprintf(line, sizeof(line), "AATEXT time %d px (em %d), %d characters, us per string: anti-aliased replace %ld transparent %ld (busy %ld); one-bit replace %ld transparent %ld (busy %ld)",
                 sizes[k], em_of(sizes[k]), (short)strlen(LINE), aa_rep, aa_trn, aa_busy, mono_rep, mono_trn, mono_busy);
        say(line);
    }

    /* xor: one-bit either way */
    set_em(16);
    LINE = "The quick brown fox";
    {
        long x = time_line(3);

        snprintf(line, sizeof(line), "AATEXT time 16 px xor (one-bit), %d characters: %ld us per string", (short)strlen(LINE), x);
        say(line);
    }
}

/* the first string of each of ten new sizes, before anything is cached */
static void cold_timing(void)
{
    short k;
    long t;
    char line[160];

    if (coldmono)
        set_mono(1);
    LINE = "The quick brown fox";
    vswr_mode(handle, 2);
    fill(0, 380, scr_w - 1, 460, WHITE);
    t = ticks();
    for (k = 0; k < 10; k++)
    {
        set_em(17 + k);
        v_gtext(handle, 20, 400, LINE);
    }
    t = ticks() - t;
    if (coldmono)
        set_mono(0);
    snprintf(line, sizeof(line), "AATEXT time cold, %s transparent, em 17-26: %ld ticks for the first strings of 19 characters in 10 sizes",
             (coldmono || planes < 16) ? "one-bit" : "anti-aliased", t);
    say(line);
}

/*
 * ===========================================================================
 * The sample screen
 * ===========================================================================
 */

/* the face named want, or want Regular, or the first whose name starts with want */
static short face_id(const char *want, short fonts, short sysfonts, char *chosen)
{
    char name[40], regular[60];
    short i, id, best = 0, rank, best_rank = 0;

    snprintf(regular, sizeof(regular), "%s Regular", want);
    for (i = sysfonts + 1; i <= sysfonts + fonts; i++)
    {
        id = vqt_name(handle, i, name);
        if (id < 5000)
            continue;
        rank = strcmp(name, want) == 0 ? 3 : strcmp(name, regular) == 0 ? 2 :
               strncmp(name, want, strlen(want)) == 0 ? 1 : 0;
        if (rank > best_rank)
        {
            best_rank = rank;
            best = id;
            strcpy(chosen, name);
        }
    }
    return best;
}

/* one sample line: one-bit in the left half of the screen, anti-aliased in the right */
static void sample_line(short y, const char *s, short back)
{
    short half = scr_w / 2, cl[4];

    if (back != WHITE)
        fill(0, y - 2, scr_w - 1, y + 2 + ext_h, back);
    vswr_mode(handle, 2);
    cl[0] = 0; cl[1] = 0; cl[2] = half - 5; cl[3] = scr_h - 1;
    vs_clip(handle, 1, cl);
    set_mono(1);
    v_gtext(handle, 8, y, s);
    set_mono(0);
    cl[0] = half + 4; cl[2] = scr_w - 1;
    vs_clip(handle, 1, cl);
    v_gtext(handle, half + 8, y, s);
    clip_off();
}

static void sample(short fonts, short sysfonts)
{
    static const short sizes[3] = { 16, 22, 30 };
    static const char *const words = "Desktop  File  View  Options  Hamburgefonts 0123";
    static const char *const more = "Disk A: 12 items, 723,456 bytes";
    short f, k, y = 8, ha, va, id;
    char line[200], name[40];

    fill(0, 0, scr_w - 1, scr_h - 1, WHITE);
    fill(scr_w / 2 - 1, 0, scr_w / 2, scr_h - 1, BLACK);
    vst_alignment(handle, 0, 5, &ha, &va);
    for (f = 0; f < sface_count; f++)
    {
        id = face_id(sfaces[f], fonts, sysfonts, name);
        if (!id)
        {
            snprintf(line, sizeof(line), "AATEXT sample: no face '%s'", sfaces[f]);
            say(line);
            continue;
        }
        vst_font(handle, id);
        for (k = 0; k < 3; k++)
        {
            set_em(sizes[k]);
            measure(words);
            vst_color(handle, BLACK);
            sample_line(y, words, WHITE);
            y += ext_h + 3;
            /* light text on a dark title bar, and dark text on grey */
            vst_color(handle, WHITE);
            sample_line(y, more, BLUE);
            y += ext_h + 5;
        }
        snprintf(line, sizeof(line), "AATEXT sample: '%s' at em %d, %d, %d up to row %d", name,
                 em_of(16), em_of(22), em_of(30), y);
        say(line);
        y += 6;
    }
    vst_color(handle, BLACK);
    say("AATEXT SAMPLE READY");
    evnt_timer((long)sample_secs * 1000L);
    say("AATEXT SAMPLE DONE");
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, sysfonts, fonts, id = 0, ha, va, ysize;
    char line[200], chosen[40];

    say("AATEXT start");
    read_config();
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;
    ysize = work_out[4];
    dpi = (ysize > 0) ? (short)(25400 / ysize) : 72;
    sysfonts = work_out[10];
    vq_extnd(handle, 1, work_out);
    planes = work_out[4];
    snprintf(line, sizeof(line), "AATEXT screen %d x %d x %d, pixels %d um high (%d dpi), switch %lx",
             scr_w, scr_h, planes, ysize, dpi, mono_addr);
    say(line);

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);

    p_white = colour_value(WHITE);
    p_black = colour_value(BLACK);
    p_red = colour_value(RED);
    p_green = colour_value(GREEN);
    p_blue = colour_value(BLUE);
    snprintf(line, sizeof(line), "AATEXT pixels: white %lx black %lx red %lx green %lx blue %lx",
             p_white, p_black, p_red, p_green, p_blue);
    say(line);

    fonts = vst_load_fonts(handle, 0);
    id = face_id(want_face[0] ? want_face : "Liberation Sans", fonts, sysfonts, chosen);
    check(id != 0, "the outline face is listed");
    if (!id || vst_font(handle, id) != id)
    {
        check(0, "the outline face is selected");
        goto done;
    }
    snprintf(line, sizeof(line), "AATEXT using font %d '%s'", id, chosen);
    say(line);
    vst_alignment(handle, 0, 5, &ha, &va);
    vst_color(handle, BLACK);

    cold_timing();
    /* the size of the checks: the em at 22 pixels, less on a narrow
     * screen, where the text turned by 90 degrees and back must fit */
    for (check_em = 22; check_em > 10; check_em--)
    {
        set_em(check_em);
        measure(TEXT);
        if (2 * ext_w + 90 <= scr_w)
            break;
    }
    snprintf(line, sizeof(line), "AATEXT checks at em %d: \"%s\" is %d x %d", em_of(check_em), TEXT, ext_w, ext_h);
    say(line);

    if (planes < 16)
    {
        planes_check();
        goto done;
    }

    modes_check();
    colour_check();
    clip_check();
    rotation_check();
    effects_check();
    set_em(check_em);
    measure(TEXT);
    bitmap_check(id, check_em);
    justified_check();
    timing();

    if (sample_secs > 0 && scr_w >= 640)
        sample(fonts, sysfonts);

done:
    {
        short box[4] = { 0, 0, 0, 0 };

        box[2] = scr_w - 1;
        box[3] = scr_h - 1;
        clip_off();
        vswr_mode(handle, 1);
        vsf_interior(handle, 1);
        vsf_color(handle, 0);
        vr_recfl(handle, box);
    }
    set_mono(0);
    vst_font(handle, 1);
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);
out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI AATEXT GUEST PASS");
    return failed;
}
