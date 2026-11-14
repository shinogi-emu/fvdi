/*
 * outledge_guest.c - outline text at its edges, in a VDI without fVDI.
 *
 * The outline fonts of C:\GEMSYS\FONTS are checked through the plain VDI
 * text calls, without NVDI opcodes or the NVDI cookie:
 *
 *  1. vst_load_fonts finds them, vqt_name lists their ids (5000 and up)
 *     with plausible names, and a font file that cannot be read does not
 *     stop the others from listing; every listed face is either selected
 *     or replaced by the system font, and draws;
 *  2. vst_font, vst_point and vst_height select a size, and vqt_fontinfo,
 *     vqt_extent and vqt_width agree with what v_gtext draws;
 *  3. codes 0x80-0xff and characters the face lacks draw without fault;
 *  4. text across the four screen edges and the corners, far off the
 *     screen, and under a clip rectangle is the unclipped drawing masked
 *     to what is visible;
 *  5. text rotated by 90, 180 and 270 degrees is the unrotated text
 *     turned, placed as the VDI places rotated system-font text;
 *  6. writing modes 1-4 with effects 0-31: replace paints the text box,
 *     transparent leaves the background, xor twice restores it, reverse
 *     transparent paints what replace leaves blank; bold widens, light
 *     thins, italic leans by 12 degrees, underline adds pixels below the
 *     baseline and outlined text is hollow;
 *  7. a string the engine cannot render (here for want of memory) is drawn
 *     in the system font, and outline text draws again afterwards.
 *
 * Pixels are compared within the run, never with another VDI: what is
 * drawn is read back with vro_cpyfm into a memory form in device format.
 * Needs 2 or more planes for the checks that tell the painted text box
 * from the background; on 1 plane they are skipped.
 *
 * C:\VTEST\OUTLEDGE.CFG, if present, holds options separated by blanks:
 *   faces=N    at least N outline faces must be listed, all selectable
 *   lowmem     memory is short: faces may fall back to the system font,
 *              and only the listing and per-face checks are made
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI OUTLEDGE GUEST PASS" or "FVDI OUTLEDGE FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILL    2       /* background colour, where it differs from 0 and 1 */
#define MARGIN  12      /* around a text box when reading it back */

typedef struct {
    short x, y, w, h;           /* screen rectangle */
    short wdw;                  /* words per row and plane */
    unsigned short *data;
} Grab;

typedef struct {
    short x, y, w, h;           /* the text box on the screen */
    long ink;                   /* pixels in the text colour */
    unsigned char *map;         /* w * h: 1 for ink */
} Map;

static int failed;
static short handle, scr_w, scr_h, planes, distinct, bg = FILL;
static unsigned long p0, p1, pfill;     /* blank, text and background pixels */
static short want_faces, lowmem;

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

    snprintf(line, sizeof(line), "OUTLEDGE %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI OUTLEDGE FAIL: %s", what);
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

static void read_config(void)
{
    char buf[200], *p;
    long fh, n;

    fh = Fopen("C:\\VTEST\\OUTLEDGE.CFG", 0);
    if (fh < 0)
        return;
    n = Fread((short)fh, sizeof(buf) - 1, buf);
    Fclose((short)fh);
    if (n <= 0)
        return;
    buf[n] = 0;
    if ((p = strstr(buf, "faces=")) != NULL)
        want_faces = atoi(p + 6);
    if (strstr(buf, "lowmem"))
        lowmem = 1;
}

/*
 * ===========================================================================
 * Reading the screen back
 * ===========================================================================
 */

static void clip_off(void)
{
    vs_clip_off(handle);
}

/* the on-screen part of a rectangle, into memory */
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

/* the value of the pixel at screen sx,sy, which must be in the grab */
static unsigned long at(const Grab *g, short sx, short sy)
{
    short x = sx - g->x, y = sy - g->y, p;
    const unsigned short *row = g->data + (long)y * g->wdw * planes;
    unsigned short bit;
    unsigned long v = 0;

    if (planes == 16)
        return row[x];
    if (planes == 32)
        return ((const unsigned long *)row)[x];
    row += (x >> 4) * planes;
    bit = 0x8000 >> (x & 15);
    for (p = 0; p < planes; p++)
        if (row[p] & bit)
            v |= 1UL << p;
    return v;
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

/* draw in the current font, writing mode, effects and rotation */
static void text(short x, short y, const char *s, short mode, short effects, short angle)
{
    vswr_mode(handle, mode);
    vst_effects(handle, effects);
    vst_rotation(handle, angle);
    v_gtext(handle, x, y, s);
    vst_rotation(handle, 0);
    vst_effects(handle, 0);
}

/* the box of the pixels of a grab that are not the background */
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

static void free_map(Map *m)
{
    if (m->map)
        free(m->map);
    m->map = NULL;
}

/*
 * draw s in replace mode at x,y (top left alignment assumed by the
 * caller) on the fill colour and take its box and ink
 */
static int draw_map(Map *m, short x, short y, const char *s, short effects, short angle,
                    short rw, short rh)
{
    Grab g;
    short x0, y0, x1, y1, i, j;

    memset(m, 0, sizeof(*m));
    fill(x - rw, y - rh, x + rw, y + rh, bg);
    text(x, y, s, 1, effects, angle);
    if (!grab(&g, x - rw, y - rh, 2 * rw + 1, 2 * rh + 1))
        return 0;
    if (!changed_box(&g, pfill, &x0, &y0, &x1, &y1))
    {
        ungrab(&g);
        return 0;
    }
    m->x = x0; m->y = y0; m->w = x1 - x0 + 1; m->h = y1 - y0 + 1;
    m->map = calloc((long)m->w * m->h, 1);
    if (!m->map)
    {
        ungrab(&g);
        return 0;
    }
    for (j = 0; j < m->h; j++)
        for (i = 0; i < m->w; i++)
            if (at(&g, x0 + i, y0 + j) == p1)
            {
                m->map[(long)j * m->w + i] = 1;
                m->ink++;
            }
    ungrab(&g);
    return 1;
}

static int ink_at(const Map *m, short sx, short sy)
{
    if (sx < m->x || sx >= m->x + m->w || sy < m->y || sy >= m->y + m->h)
        return 0;
    return m->map[(long)(sy - m->y) * m->w + (sx - m->x)];
}

/*
 * ===========================================================================
 * 1. The fonts
 * ===========================================================================
 */

static int plausible_name(const char *name)
{
    const unsigned char *p;
    int n = 0;

    for (p = (const unsigned char *)name; *p; p++, n++)
        if (*p < 0x20 || *p == 0x7f)
            return 0;
    return n >= 3 && n <= 32;
}

static short pick_font(short fonts, short sysfonts, char *chosen)
{
    char name[40];
    short i, id, rank, best = 0, best_rank = 0;

    /*
     * a proportional face with plain stems, where H, a and g tell the
     * effects apart: Liberation Sans, then any of its styles, then the
     * first face with a name
     */
    for (i = sysfonts + 1; i <= sysfonts + fonts; i++)
    {
        id = vqt_name(handle, i, name);
        if (id < 5000 || !plausible_name(name))
            continue;
        rank = strcmp(name, "Liberation Sans") == 0 ? 3 :
               strncmp(name, "Liberation Sans", 15) == 0 ? 2 : 1;
        if (rank > best_rank)
        {
            best = id;
            best_rank = rank;
            strcpy(chosen, name);
        }
    }
    return best;
}

static short fonts_check(short sysfonts, short *fonts_out)
{
    char line[200], name[40];
    short fonts, i, id, outline = 0, named = 0, again;
    short cw, ch, bw, bh, fallback = 0, drawn = 0, blank = 0, blank_fallback = 0, bad_name = 0, ids_ok = 1, k;
    static short seen[512];
    long t;

    t = ticks();
    fonts = vst_load_fonts(handle, 0);
    t = ticks() - t;
    snprintf(line, sizeof(line), "OUTLEDGE vst_load_fonts: %d fonts in %ld ticks (200 Hz)", fonts, t);
    say(line);
    *fonts_out = fonts;
    check(fonts > 0, "vst_load_fonts finds outline fonts");
    if (fonts <= 0)
        return 0;

    t = ticks();
    again = vst_load_fonts(handle, 0);
    t = ticks() - t;
    snprintf(line, sizeof(line), "OUTLEDGE vst_load_fonts again: %d in %ld ticks", again, t);
    say(line);
    check(again == fonts, "a second vst_load_fonts gives the same count");

    for (i = 1; i <= sysfonts; i++)
    {
        id = vqt_name(handle, i, name);
        snprintf(line, sizeof(line), "OUTLEDGE system face %d: id %d '%s'", i, id, name);
        say(line);
    }
    for (i = sysfonts + 1; i <= sysfonts + fonts; i++)
    {
        memset(name, 0, sizeof(name));
        id = vqt_name(handle, i, name);
        if (id >= 5000)
        {
            outline++;
            if (plausible_name(name))
                named++;
            for (k = 0; k < outline - 1 && k < 512; k++)
                if (seen[k] == id)
                    ids_ok = 0;
            if (outline - 1 < 512)
                seen[outline - 1] = id;
        }
        if (i <= sysfonts + 3 || i == sysfonts + fonts)
        {
            snprintf(line, sizeof(line), "OUTLEDGE font %d: id %d '%s'", i, id, name);
            say(line);
        }
    }
    snprintf(line, sizeof(line), "OUTLEDGE %d outline faces listed, %d with plausible names", outline, named);
    say(line);
    check(outline == fonts, "vqt_name lists every loaded font with an outline id");
    check(ids_ok, "outline ids are distinct");
    if (want_faces)
    {
        snprintf(line, sizeof(line), "at least %d faces listed, past an unreadable font file", want_faces);
        check(outline >= want_faces, line);
    }
    id = vqt_name(handle, sysfonts + fonts + 1, name);
    check(id == 1, "past the last font, vqt_name gives the system font");

    /* every face: selected, or replaced by the system font; either draws */
    for (i = sysfonts + 1; i <= sysfonts + fonts; i++)
    {
        Grab g;
        short got, x0, y0, x1, y1;

        id = vqt_name(handle, i, name);
        got = vst_font(handle, id);
        if (got != id)
        {
            fallback++;
            snprintf(line, sizeof(line), "OUTLEDGE face %d '%s' not selectable: vst_font gives %d", id, name, got);
            say(line);
            if (got != 1)
                check(0, "a face that cannot be used falls back to the system font");
        }
        vst_point(handle, 12, &cw, &ch, &bw, &bh);
        vst_alignment(handle, 0, 5, &cw, &ch);
        fill(0, 0, 159, 39, 0);
        vst_color(handle, 1);
        text(4, 4, "Abg", 2, 0, 0);
        if (grab(&g, 0, 0, 160, 40))
        {
            if (changed_box(&g, p0, &x0, &y0, &x1, &y1))
            {
                drawn++;
                if (got == id && !plausible_name(name))
                {
                    bad_name++;
                    snprintf(line, sizeof(line), "OUTLEDGE face %d draws, but its name '%s' is not plausible", id, name);
                    say(line);
                }
            }
            else
            {
                blank++;
                if (got != id)
                    blank_fallback++;
                snprintf(line, sizeof(line), "OUTLEDGE face %d '%s' draws nothing", id, name);
                say(line);
            }
            ungrab(&g);
        }
    }
    snprintf(line, sizeof(line), "OUTLEDGE per face: %d drawn of %d, %d fell back to the system font",
             drawn, fonts, fallback);
    say(line);
    check(blank_fallback == 0, "a face replaced by the system font draws in the system font");
    check(bad_name == 0, "every face that draws has a plausible name");
    if (want_faces)
    {
        /* a damaged file may still give a face, which then has nothing to draw */
        check(blank <= fonts - want_faces, "only faces past the good ones draw nothing");
        if (!lowmem)
            check(fallback + blank <= fonts - want_faces, "every good face is selected and draws");
    }
    else
        check(drawn > 0, "the faces draw text");
    vst_font(handle, 1);
    return outline;
}

/*
 * ===========================================================================
 * 2. Sizes and metrics against what is drawn
 * ===========================================================================
 */

static void metrics_check(short id)
{
    static const char *s = "Hamburgefonts";
    char line[220];
    short cw, ch, bw, bh, pt, hh, minade, maxade, dist[5], maxw, eff[3], ext[8];
    short i, sum, adv, l, r;
    short x = 40, y = 40;
    Map m;

    pt = vst_point(handle, 24, &cw, &ch, &bw, &bh);
    snprintf(line, sizeof(line), "OUTLEDGE vst_point 24 -> %d: char %d x %d, cell %d x %d", pt, cw, ch, bw, bh);
    say(line);
    check(pt > 0 && pt <= 24, "vst_point sets a size no larger than asked");
    check(ch > 0 && bh > ch && cw > 0 && bw >= cw, "vst_point gives plausible character and cell sizes");

    vqt_fontinfo(handle, &minade, &maxade, dist, &maxw, eff);
    snprintf(line, sizeof(line), "OUTLEDGE fontinfo: ade %d-%d, bottom %d descent %d half %d ascent %d top %d, max width %d",
             minade, maxade, dist[0], dist[1], dist[2], dist[3], dist[4], maxw);
    say(line);
    check(dist[4] == ch, "vqt_fontinfo top is the character height");
    check(dist[0] == bh - ch - 1, "vqt_fontinfo bottom is the cell height less top and baseline");
    check(dist[3] > 0 && dist[3] <= dist[4] && dist[2] > 0 && dist[2] <= dist[4] && dist[1] <= dist[0],
          "vqt_fontinfo distances in order");
    check(maxw == bw, "vqt_fontinfo maximum width is the cell width");
    check(minade == 0 && maxade == 255, "vqt_fontinfo character range 0-255");

    vqt_extent(handle, s, ext);
    snprintf(line, sizeof(line), "OUTLEDGE extent: %d,%d %d,%d %d,%d %d,%d", ext[0], ext[1], ext[2], ext[3],
             ext[4], ext[5], ext[6], ext[7]);
    say(line);
    check(ext[0] == 0 && ext[1] == 0 && ext[2] == ext[4] && ext[5] == ext[7] && ext[3] == 0 && ext[6] == 0,
          "vqt_extent is a rectangle from 0,0");
    check(ext[5] == dist[4] + dist[0], "vqt_extent height is top plus bottom");

    vst_alignment(handle, 0, 5, &l, &r);
    vst_color(handle, 1);
    if (!draw_map(&m, x, y, s, 0, 0, 30 + ext[2], 30 + ext[5]))
    {
        check(0, "text drawn for the metrics");
        return;
    }
    snprintf(line, sizeof(line), "OUTLEDGE drawn at %d,%d (top left): box %d,%d %d x %d, %ld ink",
             x, y, m.x, m.y, m.w, m.h, m.ink);
    say(line);
    check(m.ink > 0, "v_gtext draws the outline text");
    if (distinct)
    {
        check(m.w == ext[2], "the painted box is as wide as vqt_extent");
        check(m.h == ext[5], "the painted box is as high as vqt_extent");
        check(m.y == y, "top alignment puts the box top at y");
        check(m.x <= x && m.x > x - ext[2] / 4, "left alignment puts the text at x");
    }
    {
        /* ink both above the baseline and below it ('g') */
        short above = 0, below = 0, j;

        for (j = 0; j < m.h; j++)
            for (i = 0; i < m.w; i++)
                if (m.map[(long)j * m.w + i])
                {
                    if (m.y + j < y + ch)
                        above = 1;
                    else
                        below = 1;
                }
        check(above && below, "ink above the baseline and in the descender");
    }

    sum = 0;
    for (i = 0; s[i]; i++)
    {
        vqt_width(handle, s[i], &adv, &l, &r);
        sum += adv;
    }
    snprintf(line, sizeof(line), "OUTLEDGE vqt_width sum %d, extent %d", sum, ext[2]);
    say(line);
    check(sum > 0 && sum <= ext[2] + 1 && sum >= ext[2] - ext[2] / 8,
          "vqt_width advances add up to the extent");
    check(vqt_width(handle, 'A', &adv, &l, &r) == 'A' && adv > 0, "vqt_width of 'A'");
    free_map(&m);

    /* centred and right alignment move the box by half and all of the width */
    {
        Map c;
        short ha, va;

        vst_alignment(handle, 1, 5, &ha, &va);
        draw_map(&c, x + ext[2], y, s, 0, 0, 30 + ext[2], 30 + ext[5]);
        vst_alignment(handle, 2, 5, &ha, &va);
        draw_map(&m, x + ext[2], y, s, 0, 0, 30 + 2 * ext[2], 30 + ext[5]);
        snprintf(line, sizeof(line), "OUTLEDGE centred box at %d, right box at %d (x %d, width %d)",
                 c.x, m.x, x + ext[2], ext[2]);
        say(line);
        if (distinct)
            check(c.x >= x + ext[2] / 2 - ext[2] / 8 && c.x <= x + ext[2] / 2 + 1 &&
                  m.x >= x - ext[2] / 8 && m.x <= x + 1,
                  "centred and right alignment by the extent");
        free_map(&c);
        free_map(&m);
        vst_alignment(handle, 0, 5, &ha, &va);
    }

    hh = 30;
    vst_height(handle, hh, &cw, &ch, &bw, &bh);
    vqt_fontinfo(handle, &minade, &maxade, dist, &maxw, eff);
    snprintf(line, sizeof(line), "OUTLEDGE vst_height %d: char %d x %d, cell %d x %d, top %d",
             hh, cw, ch, bw, bh, dist[4]);
    say(line);
    check(ch <= hh && ch >= hh - hh / 5, "vst_height sets a character height close to and not above the request");
    check(dist[4] == ch, "after vst_height, vqt_fontinfo top is the character height");

    vst_font(handle, 1);
    check(vst_font(handle, id) == id, "the outline face is selected again after the system font");
}

/*
 * ===========================================================================
 * 3. Codes 0x80-0xff and missing glyphs
 * ===========================================================================
 */

static void codes_check(void)
{
    short codes[160], n = 0, c, ext[8], cw, ch, bw, bh, ha, va;
    char line[160];
    Grab g;
    short x0, y0, x1, y1;

    for (c = 0x80; c <= 0xff; c++)
        codes[n++] = c;
    for (c = 1; c < 0x20; c++)
        codes[n++] = c;
    codes[n++] = 0x7f;
    vst_point(handle, 12, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    vqt_extent16n(handle, codes, n, ext);
    snprintf(line, sizeof(line), "OUTLEDGE %d codes 0x80-0xff, 1-0x1f, 0x7f: extent %d x %d", n, ext[2], ext[5]);
    say(line);
    check(ext[2] > 0 && ext[5] > 0, "vqt_extent of codes 0x80-0xff and control codes");
    fill(0, 0, scr_w - 1, 3 * bh, 0);
    vswr_mode(handle, 2);
    v_gtext16n(handle, 4, 4, codes, n);
    if (grab(&g, 0, 0, scr_w, 3 * bh))
    {
        check(changed_box(&g, p0, &x0, &y0, &x1, &y1), "codes 0x80-0xff draw");
        ungrab(&g);
    }
    /* one at a time, too */
    for (c = 0; c < n; c++)
        v_gtext16n(handle, 4, 4 + bh, &codes[c], 1);
    check(1, "each code alone draws without fault");
}

/*
 * ===========================================================================
 * 4. Clipping
 * ===========================================================================
 */

/*
 * draw s at ex,ey with clipping cl (NULL: off) after filling its
 * surroundings, and compare what is visible with the reference drawing
 * ref (made at rx,ry) masked to the clip rectangle and the screen
 */
static long clip_case(const char *label, const Map *ref, short rx, short ry, const char *s,
                      short ex, short ey, short *cl)
{
    Grab g;
    short x, y, rw = ref->w + 2 * MARGIN, rh = ref->h + 2 * MARGIN;
    short gx = ex + (ref->x - rx) - MARGIN, gy = ey + (ref->y - ry) - MARGIN;
    long bad = 0, shown = 0;
    char line[200];

    fill(gx, gy, gx + rw - 1, gy + rh - 1, bg);
    if (cl)
        vs_clip(handle, 1, cl);
    else
        clip_off();
    vswr_mode(handle, 1);
    v_gtext(handle, ex, ey, s);
    clip_off();
    if (!grab(&g, gx, gy, rw, rh))
    {
        snprintf(line, sizeof(line), "OUTLEDGE clip %s: nothing on the screen", label);
        say(line);
        return 0;
    }
    for (y = g.y; y < g.y + g.h; y++)
        for (x = g.x; x < g.x + g.w; x++)
        {
            short sx = x - ex + rx, sy = y - ey + ry;
            int in_box = sx >= ref->x && sx < ref->x + ref->w && sy >= ref->y && sy < ref->y + ref->h;
            int visible = !cl || (x >= cl[0] && x <= cl[2] && y >= cl[1] && y <= cl[3]);
            unsigned long want = pfill, got = at(&g, x, y);

            /* without a distinct background, p0 is pfill */
            if (in_box && visible)
                want = ink_at(ref, sx, sy) ? p1 : p0;
            if (got != want)
                bad++;
            if (want == p1)
                shown++;
        }
    snprintf(line, sizeof(line), "OUTLEDGE clip %s: %d x %d visible, %ld ink, %ld wrong", label, g.w, g.h,
             shown, bad);
    say(line);
    ungrab(&g);
    return bad;
}

static void clip_check(void)
{
    static const char *s = "Hamburg";
    Map ref;
    short rx = 60, ry = 60, cw, ch, bw, bh, ha, va, w, h, cl[4], i;
    char what[120];
    struct { const char *label; short x, y; } edges[8];

    vst_point(handle, 24, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    vst_color(handle, 1);
    if (!draw_map(&ref, rx, ry, s, 0, 0, 2 * MARGIN + 300, 2 * MARGIN + 80))
    {
        check(0, "reference text for the clipping checks");
        return;
    }
    w = ref.w;
    h = ref.h;
    edges[0].label = "left edge";    edges[0].x = -w / 2;          edges[0].y = scr_h / 2;
    edges[1].label = "right edge";   edges[1].x = scr_w - w / 2;   edges[1].y = scr_h / 2;
    edges[2].label = "top edge";     edges[2].x = scr_w / 2;       edges[2].y = -h / 2;
    edges[3].label = "bottom edge";  edges[3].x = scr_w / 2;       edges[3].y = scr_h - h / 2;
    edges[4].label = "top left";     edges[4].x = -w / 3;          edges[4].y = -h / 3;
    edges[5].label = "top right";    edges[5].x = scr_w - w / 3;   edges[5].y = -h / 3;
    edges[6].label = "bottom left";  edges[6].x = -w / 3;          edges[6].y = scr_h - h / 3;
    edges[7].label = "bottom right"; edges[7].x = scr_w - w / 3;   edges[7].y = scr_h - h / 3;

    for (i = 0; i < 8; i++)
    {
        snprintf(what, sizeof(what), "%s, clipping off", edges[i].label);
        check(clip_case(what, &ref, rx, ry, s, edges[i].x, edges[i].y, NULL) == 0, what);
        cl[0] = 0; cl[1] = 0; cl[2] = scr_w - 1; cl[3] = scr_h - 1;
        snprintf(what, sizeof(what), "%s, clipped to the screen", edges[i].label);
        check(clip_case(what, &ref, rx, ry, s, edges[i].x, edges[i].y, cl) == 0, what);
    }

    /* in the middle of the screen: a clip rectangle that cuts the text on all four sides */
    {
        short tx = scr_w / 2 - w / 2, ty = scr_h / 2 - h / 2;

        cl[0] = tx + w / 4; cl[1] = ty + h / 4; cl[2] = tx + 3 * w / 4; cl[3] = ty + 3 * h / 4;
        check(clip_case("rectangle inside the text", &ref, rx, ry, s, tx, ty, cl) == 0,
              "clip rectangle cuts the text on all four sides");
        /* one that holds only the left half, one that holds none of it */
        cl[0] = 0; cl[1] = 0; cl[2] = tx + w / 2; cl[3] = scr_h - 1;
        check(clip_case("left half", &ref, rx, ry, s, tx, ty, cl) == 0, "clip rectangle holds the left half");
        cl[0] = 0; cl[1] = 0; cl[2] = tx - 1 - MARGIN; cl[3] = ty - 1 - MARGIN;
        check(clip_case("rectangle beside the text", &ref, rx, ry, s, tx, ty, cl) == 0,
              "clip rectangle beside the text: nothing drawn");
    }

    /* far off the screen, and at the ends of the coordinate range */
    {
        static const short far[][2] = {
            { -2000, 100 }, { 3000, 100 }, { 100, -2000 }, { 100, 3000 },
            { 32000, 32000 }, { -32000, -32000 }, { 32767, 100 }, { -32768, 100 }
        };
        Grab g;
        short x0, y0, x1, y1, k, clean = 1;

        fill(0, 0, scr_w - 1, 2 * MARGIN, bg);
        fill(0, 0, 2 * MARGIN, scr_h - 1, bg);
        for (k = 0; k < (short)(sizeof(far) / sizeof(far[0])); k++)
        {
            clip_off();
            vswr_mode(handle, 1);
            v_gtext(handle, far[k][0], far[k][1], s);
            cl[0] = 0; cl[1] = 0; cl[2] = scr_w - 1; cl[3] = scr_h - 1;
            vs_clip(handle, 1, cl);
            v_gtext(handle, far[k][0], far[k][1], s);
            clip_off();
        }
        if (grab(&g, 0, 0, scr_w, 2 * MARGIN))
        {
            if (changed_box(&g, pfill, &x0, &y0, &x1, &y1))
                clean = 0;
            ungrab(&g);
        }
        if (grab(&g, 0, 0, 2 * MARGIN, scr_h))
        {
            if (changed_box(&g, pfill, &x0, &y0, &x1, &y1))
                clean = 0;
            ungrab(&g);
        }
        check(clean, "text far off the screen draws nothing and does not fault");
    }
    free_map(&ref);
}

/*
 * ===========================================================================
 * 5. Rotation
 * ===========================================================================
 */

static void rotation_check(void)
{
    static const char *s = "Hamburg";
    static const short angles[3] = { 900, 1800, 2700 };
    Map ref;
    short rx = 60, ry = 60, cw, ch, bw, bh, ha, va, ox, oy, k, ext0[8];
    char line[200];

    vst_point(handle, 18, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    vst_color(handle, 1);
    vqt_extent(handle, s, ext0);
    if (!draw_map(&ref, rx, ry, s, 0, 0, 2 * MARGIN + 250, 2 * MARGIN + 70))
    {
        check(0, "reference text for the rotation checks");
        return;
    }
    ox = ref.x - rx;
    oy = ref.y - ry;
    for (k = 0; k < 3; k++)
    {
        short a = angles[k], px = scr_w / 2, py = scr_h / 2, r = ref.w + ref.h + MARGIN;
        short ext[8], wx, wy, ww, wh, i, j;
        long bad = 0;
        Map m;
        char what[100];

        vst_rotation(handle, a);
        vqt_extent(handle, s, ext);
        vst_rotation(handle, 0);
        snprintf(line, sizeof(line), "OUTLEDGE rotation %d extent: %d,%d %d,%d %d,%d %d,%d", a,
                 ext[0], ext[1], ext[2], ext[3], ext[4], ext[5], ext[6], ext[7]);
        say(line);
        if (a == 1800)
            snprintf(what, sizeof(what), "rotation %d: vqt_extent keeps width and height", a);
        else
            snprintf(what, sizeof(what), "rotation %d: vqt_extent swaps width and height", a);
        if (a == 900)
            check(ext[0] == ext0[5] && ext[3] == ext0[2], what);
        else if (a == 1800)
            check(ext[0] == ext0[2] && ext[1] == ext0[5], what);
        else
            check(ext[1] == ext0[2] && ext[4] == ext0[5], what);

        if (!draw_map(&m, px, py, s, 0, a, r, r))
        {
            snprintf(what, sizeof(what), "rotation %d draws", a);
            check(0, what);
            continue;
        }
        /* where the VDI puts rotated system-font text: (u,v) -> ... */
        if (a == 900)
        {
            ww = ref.h; wh = ref.w; wx = px + oy; wy = py - ox - ref.w + 1;
        }
        else if (a == 1800)
        {
            ww = ref.w; wh = ref.h; wx = px - ox - ref.w + 1; wy = py - oy - ref.h + 1;
        }
        else
        {
            ww = ref.h; wh = ref.w; wx = px - oy - ref.h + 1; wy = py + ox;
        }
        for (j = 0; j < ref.h; j++)
            for (i = 0; i < ref.w; i++)
            {
                short u = ox + i, v = oy + j, sx, sy;

                if (a == 900)       { sx = px + v; sy = py - u; }
                else if (a == 1800) { sx = px - u; sy = py - v; }
                else                { sx = px - v; sy = py + u; }
                if (ink_at(&m, sx, sy) != ref.map[(long)j * ref.w + i])
                    bad++;
            }
        snprintf(line, sizeof(line),
                 "OUTLEDGE rotation %d: box %d,%d %d x %d (expected %d,%d %d x %d), %ld ink (unrotated %ld), %ld pixels differ",
                 a, m.x, m.y, m.w, m.h, wx, wy, ww, wh, m.ink, ref.ink, bad);
        say(line);
        if (distinct)
        {
            snprintf(what, sizeof(what), "rotation %d: the box is the unrotated box turned, in place", a);
            check(m.x == wx && m.y == wy && m.w == ww && m.h == wh, what);
        }
        snprintf(what, sizeof(what), "rotation %d: the same pixels as the unrotated text, turned", a);
        check(m.ink == ref.ink && bad == 0, what);
        free_map(&m);
    }
    free_map(&ref);
}

/*
 * ===========================================================================
 * 6. Writing modes and effects
 * ===========================================================================
 */

/* the x of the leftmost ink in a screen row of a map, or -1 */
static short left_edge(const Map *m, short sy)
{
    short x;

    for (x = m->x; x < m->x + m->w; x++)
        if (ink_at(m, x, sy))
            return x;
    return -1;
}

/*
 * italic leans by 12 degrees: the left edge of an 'H' moves right going
 * up.  The shift is the least-squares slope of the italic edge less the
 * plain one, row by row above the baseline, times the rows measured; it
 * must be tan 12 degrees times those rows, give or take a third (and at
 * least a pixel and a half, for the rounding of small sizes).
 */
static void italic_check(short x, short y, short top)
{
    Map n, it;
    double sr = 0, sd = 0, srr = 0, srd = 0, slope, shift, want, tol;
    short r, r0 = 32767, r1 = -1, rows = 0;
    char line[200];

    if (!draw_map(&n, x, y, "H", 0, 0, 140, 90) || !draw_map(&it, x, y, "H", 4, 0, 140, 90))
    {
        check(0, "plain and italic 'H' draw");
        free_map(&n);
        return;
    }
    for (r = y; r < y + top; r++)
    {
        short a = left_edge(&n, r), b = left_edge(&it, r);

        if (a < 0 || b < 0)
            continue;
        sr += r;
        sd += b - a;
        srr += (double)r * r;
        srd += (double)r * (b - a);
        rows++;
        if (r < r0)
            r0 = r;
        if (r > r1)
            r1 = r;
    }
    if (rows < 4)
    {
        check(0, "an 'H' with rows to measure the italic on");
        free_map(&n);
        free_map(&it);
        return;
    }
    slope = (rows * srd - sr * sd) / (rows * srr - sr * sr);
    shift = -slope * (r1 - r0);         /* rows go down the screen */
    want = (r1 - r0) * 0.212557;        /* tan 12 degrees */
    tol = want / 3 > 1.5 ? want / 3 : 1.5;
    snprintf(line, sizeof(line),
             "OUTLEDGE italic 'H': the left edge leans %ld/100 px over %d rows, %ld/100 expected (+-%ld/100)",
             (long)(shift * 100), r1 - r0, (long)(want * 100), (long)(tol * 100));
    say(line);
    check(shift > want - tol && shift < want + tol, "italic leans the text right by 12 degrees");
    free_map(&n);
    free_map(&it);
}

static long ink_below(const Map *m, short base)
{
    long n = 0;
    short x, y;

    for (y = base; y < m->y + m->h; y++)
        for (x = m->x; x < m->x + m->w; x++)
            n += ink_at(m, x, y);
    return n;
}

static void modes_check(void)
{
    static const char *s = "Hag";
    static Map maps[32];
    short x = 60, y = 60, rw = 140, rh = 90, cw, ch, bw, bh, ha, va, e, base;
    long bad_trans = 0, bad_xor = 0, bad_xor2 = 0, bad_rev = 0, bad_rep = 0;
    char line[200];

    vst_point(handle, 36, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    vst_color(handle, 1);
    base = y + ch;
    for (e = 0; e < 32; e++)
    {
        Grab g;
        Map *m = &maps[e];
        short sx, sy;
        long bt = 0, bx = 0, bx2 = 0, br = 0, bp = 0;

        if (!draw_map(m, x, y, s, e, 0, rw, rh))
        {
            snprintf(line, sizeof(line), "effects %d: replace mode draws", e);
            check(0, line);
            continue;
        }
        /* replace: the box holds only text and blank pixels */
        if (distinct && grab(&g, m->x, m->y, m->w, m->h))
        {
            for (sy = g.y; sy < g.y + g.h; sy++)
                for (sx = g.x; sx < g.x + g.w; sx++)
                    if (at(&g, sx, sy) != p0 && at(&g, sx, sy) != p1)
                        bp++;
            ungrab(&g);
        }

        /* transparent */
        fill(x - rw, y - rh, x + rw, y + rh, bg);
        text(x, y, s, 2, e, 0);
        if (grab(&g, x - rw, y - rh, 2 * rw + 1, 2 * rh + 1))
        {
            for (sy = g.y; sy < g.y + g.h; sy++)
                for (sx = g.x; sx < g.x + g.w; sx++)
                    if (at(&g, sx, sy) != (ink_at(m, sx, sy) ? p1 : pfill))
                        bt++;
            ungrab(&g);
        }

        /* xor: the text pixels change; twice, nothing has */
        fill(x - rw, y - rh, x + rw, y + rh, bg);
        text(x, y, s, 3, e, 0);
        if (grab(&g, x - rw, y - rh, 2 * rw + 1, 2 * rh + 1))
        {
            for (sy = g.y; sy < g.y + g.h; sy++)
                for (sx = g.x; sx < g.x + g.w; sx++)
                    if ((at(&g, sx, sy) != pfill) != ink_at(m, sx, sy))
                        bx++;
            ungrab(&g);
        }
        text(x, y, s, 3, e, 0);
        if (grab(&g, x - rw, y - rh, 2 * rw + 1, 2 * rh + 1))
        {
            for (sy = g.y; sy < g.y + g.h; sy++)
                for (sx = g.x; sx < g.x + g.w; sx++)
                    if (at(&g, sx, sy) != pfill)
                        bx2++;
            ungrab(&g);
        }

        /* reverse transparent: the blank pixels of the box, in the text colour */
        if (distinct)
        {
            fill(x - rw, y - rh, x + rw, y + rh, bg);
            text(x, y, s, 4, e, 0);
            if (grab(&g, x - rw, y - rh, 2 * rw + 1, 2 * rh + 1))
            {
                for (sy = g.y; sy < g.y + g.h; sy++)
                    for (sx = g.x; sx < g.x + g.w; sx++)
                    {
                        int in_box = sx >= m->x && sx < m->x + m->w && sy >= m->y && sy < m->y + m->h;
                        unsigned long want = (in_box && !ink_at(m, sx, sy)) ? p1 : pfill;

                        if (at(&g, sx, sy) != want)
                            br++;
                    }
                ungrab(&g);
            }
        }
        if (bt || bx || bx2 || br || bp || e == 0 || e == 31)
        {
            snprintf(line, sizeof(line),
                     "OUTLEDGE effects %2d: box %d x %d, %ld ink; wrong: replace %ld, transparent %ld, xor %ld, xor twice %ld, reverse %ld",
                     e, m->w, m->h, m->ink, bp, bt, bx, bx2, br);
            say(line);
        }
        bad_rep += bp;
        bad_trans += bt;
        bad_xor += bx;
        bad_xor2 += bx2;
        bad_rev += br;
    }
    if (distinct)
        check(bad_rep == 0, "replace mode paints the text box in the text and blank colours");
    check(bad_trans == 0, "transparent mode draws the text pixels of replace mode and leaves the rest");
    check(bad_xor == 0, "xor mode changes the text pixels and nothing else");
    check(bad_xor2 == 0, "xor mode twice restores the screen");
    if (distinct)
        check(bad_rev == 0, "reverse transparent paints the blank pixels of the text box");

    if (maps[0].map && maps[1].map && maps[2].map && maps[4].map && maps[8].map)
    {
        long n = maps[0].ink;

        snprintf(line, sizeof(line), "OUTLEDGE ink: normal %ld, bold %ld, light %ld, italic %ld, underlined %ld, outlined %ld",
                 n, maps[1].ink, maps[2].ink, maps[4].ink, maps[8].ink, maps[16].ink);
        say(line);
        check(maps[1].ink > n + n / 20 && maps[1].w >= maps[0].w, "bold widens the text");
        check(maps[2].ink < (n * 3) / 4 && maps[2].ink > n / 4, "light thins the text");
        italic_check(x, y, ch);
        snprintf(line, sizeof(line), "OUTLEDGE ink below the baseline: normal %ld, underlined %ld",
                 ink_below(&maps[0], base + 1), ink_below(&maps[8], base + 1));
        say(line);
        check(ink_below(&maps[8], base + 1) > ink_below(&maps[0], base + 1) + maps[0].w / 2,
              "underline adds pixels below the baseline");
    }
    for (e = 0; e < 32; e++)
        free_map(&maps[e]);

    /* outlined text is hollow: large, so that stems have an inside */
    {
        Map n, o;
        short hh = 100, i, j, k, l, deep, inner = 0, filled = 0;

        vst_height(handle, hh, &cw, &ch, &bw, &bh);
        if (draw_map(&n, 20, 20, "H", 0, 0, 200, 200) && draw_map(&o, 20, 20, "H", 16, 0, 200, 200))
        {
            for (j = 2; j < n.h - 2; j++)
                for (i = 2; i < n.w - 2; i++)
                {
                    deep = 1;
                    for (k = -2; k <= 2 && deep; k++)
                        for (l = -2; l <= 2 && deep; l++)
                            if (!n.map[(long)(j + k) * n.w + i + l])
                                deep = 0;
                    if (!deep)
                        continue;
                    inner++;
                    if (ink_at(&o, n.x + i, n.y + j))
                        filled++;
                }
            snprintf(line, sizeof(line), "OUTLEDGE outlined 'H' at height %d: %ld ink (plain %ld), %d of %d inner pixels set",
                     ch, o.ink, n.ink, filled, inner);
            say(line);
            check(inner > 0 && o.ink > 0 && filled * 10 < inner, "outlined text is hollow");
        }
        else
            check(0, "outlined text draws");
        free_map(&n);
        free_map(&o);
    }
}

/*
 * ===========================================================================
 * Nothing left over from bitmap-font text
 * ===========================================================================
 */

/*
 * skewed, thickened and outlined text in the system font sets line-A
 * variables that the text blit reads; outline text drawn next must not
 * pick them up
 */
static void leftover_check(short id)
{
    Map before, after;
    short cw, ch, bw, bh, ha, va, i, j, same = 1;
    char line[160];

    vst_point(handle, 24, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    vst_color(handle, 1);
    if (!draw_map(&before, 40, 40, "Hamburg", 0, 0, 200, 60))
    {
        check(0, "outline text for the leftover check");
        return;
    }
    vst_font(handle, 1);
    vst_alignment(handle, 0, 0, &ha, &va);
    vst_effects(handle, 1 | 4 | 16);
    vswr_mode(handle, 2);
    v_gtext(handle, 20, scr_h - 20, "skewed system font");
    vst_effects(handle, 0);
    vst_font(handle, id);
    vst_point(handle, 24, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    if (!draw_map(&after, 40, 40, "Hamburg", 0, 0, 200, 60))
    {
        free_map(&before);
        check(0, "outline text after the system font");
        return;
    }
    if (after.x != before.x || after.y != before.y || after.w != before.w || after.h != before.h)
        same = 0;
    for (j = 0; same && j < before.h; j++)
        for (i = 0; i < before.w; i++)
            if (before.map[(long)j * before.w + i] != after.map[(long)j * after.w + i])
                same = 0;
    snprintf(line, sizeof(line), "OUTLEDGE before skewed system-font text: box %d,%d %d x %d, %ld ink; after: %d,%d %d x %d, %ld ink",
             before.x, before.y, before.w, before.h, before.ink, after.x, after.y, after.w, after.h, after.ink);
    say(line);
    check(same, "outline text is the same after skewed, bold, outlined system-font text");
    free_map(&before);
    free_map(&after);
}

/*
 * ===========================================================================
 * 7. What the engine cannot render
 * ===========================================================================
 */

/* take all memory but pieces under 16 KB; returns the blocks taken */
static short take_memory(void **blocks, short max)
{
    short n = 0;
    long size;

    while (n < max)
    {
        size = (long)Mxalloc(-1, 3);        /* the largest block, any RAM */
        if (size == -32)                    /* no Mxalloc */
            size = (long)Malloc(-1);
        if (size < 16384)
            break;
        blocks[n] = (void *)Mxalloc(size, 3);
        if ((long)blocks[n] == -32)
            blocks[n] = (void *)Malloc(size);
        if ((long)blocks[n] <= 0)
            break;
        n++;
    }
    return n;
}

/*
 * A string whose bitmap needs a block of its own (over 16 KB) cannot be
 * rendered while the memory is taken: it must then be drawn in the system
 * font, not left out.  If the engine still finds the memory, nothing can
 * be told, and that is said.
 */
static void fallback_check(void)
{
    static const char *s = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";   /* 30, over 2000 px */
    static void *blocks[512];
    short cw, ch, bw, bh, ha, va, x0, y0, x1, y1, n, k, ext[8], cl[4];
    char line[200];
    Grab g;

    vst_height(handle, 100, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    vst_color(handle, 1);
    vqt_extent(handle, s, ext);         /* the glyphs are loaded while there is memory */
    fill(0, 0, scr_w - 1, 3 * ch, 0);

    n = take_memory(blocks, 512);
    cl[0] = 0; cl[1] = 0; cl[2] = scr_w - 1; cl[3] = scr_h - 1;
    vs_clip(handle, 1, cl);             /* the system font does not clip itself */
    vswr_mode(handle, 2);
    v_gtext(handle, 20, 20, s);
    clip_off();
    for (k = 0; k < n; k++)
        Mfree(blocks[k]);

    if (!grab(&g, 0, 0, scr_w, 3 * ch))
    {
        check(0, "the screen read back after the fallback");
        return;
    }
    if (!changed_box(&g, p0, &x0, &y0, &x1, &y1))
    {
        x0 = y0 = 0;
        x1 = y1 = -1;
    }
    ungrab(&g);
    snprintf(line, sizeof(line),
             "OUTLEDGE %d blocks taken; a %d px string at height %d drew %d x %d at %d,%d",
             n, ext[2], ch, x1 - x0 + 1, y1 - y0 + 1, x0, y0);
    say(line);
    if (y1 - y0 + 1 > 2 * 16)
        say("OUTLEDGE the engine still had the memory: the fallback could not be provoked");
    else
        check(y1 >= y0, "a string the engine cannot render is drawn in the system font");

    /* with the memory back, outline text again */
    fill(0, 0, scr_w - 1, 3 * ch, 0);
    vswr_mode(handle, 2);
    v_gtext(handle, 20, 20, "MH");
    if (grab(&g, 0, 0, scr_w, 3 * ch))
    {
        int any = changed_box(&g, p0, &x0, &y0, &x1, &y1);

        ungrab(&g);
        snprintf(line, sizeof(line), "OUTLEDGE afterwards \"MH\" drew %d rows", any ? y1 - y0 + 1 : 0);
        say(line);
        check(any && y1 - y0 + 1 > ch / 2, "outline text draws again once memory is back");
    }
}

/*
 * ===========================================================================
 * v_justified, briefly
 * ===========================================================================
 */

static void justified_check(void)
{
    Grab g;
    short cw, ch, bw, bh, ha, va, x0, y0, x1, y1, len, ext[8];
    char line[160];

    vst_point(handle, 18, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &ha, &va);
    vqt_extent(handle, "Ham burg fonts", ext);
    len = ext[2] + ext[2] / 2;      /* half as long again */
    if (20 + len + 2 * bw > scr_w)
        len = scr_w - 20 - 2 * bw;
    fill(0, 0, scr_w - 1, 3 * bh, 0);
    vswr_mode(handle, 2);
    v_justified(handle, 20, 10, "Ham burg fonts", len, 1, 1);
    if (grab(&g, 0, 0, scr_w, 3 * bh))
    {
        int any = changed_box(&g, p0, &x0, &y0, &x1, &y1);

        snprintf(line, sizeof(line), "OUTLEDGE v_justified to %d (extent %d): ink %d-%d", len, ext[2], x0, x1);
        say(line);
        check(any && x0 >= 20 - cw && x1 > 20 + len - 2 * bw && x1 < 20 + len + bw,
              "v_justified spreads the text over the length");
        ungrab(&g);
    }
}

/*
 * the outline face is still the current font: if it could not be sized
 * again, the VDI has put the system font in its place, and the checks
 * below would measure that (and, with clipping off, draw it off the
 * screen, which bitmap fonts do not survive)
 */
static int still_selected(short id, const char *before)
{
    short attrib[10];
    char what[120];

    vqt_attributes(handle, attrib);
    if (attrib[0] == id)
        return 1;
    snprintf(what, sizeof(what), "the outline face is still the current font before the %s checks", before);
    check(0, what);
    return 0;
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, sysfonts, fonts = 0, id;
    char line[200], chosen[40];

    say("OUTLEDGE start");
    read_config();
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;
    sysfonts = work_out[10];
    vq_extnd(handle, 1, work_out);
    planes = work_out[4];
    snprintf(line, sizeof(line), "OUTLEDGE screen %d x %d x %d, %d system faces", scr_w, scr_h, planes, sysfonts);
    say(line);

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);

    p0 = colour_value(0);
    p1 = colour_value(1);
    pfill = colour_value(FILL);
    distinct = pfill != p0 && pfill != p1 && p0 != p1;
    if (!distinct)
    {
        bg = 0;             /* 1 plane: the box blends into the background */
        pfill = p0;
    }
    snprintf(line, sizeof(line), "OUTLEDGE pixel values: blank %lx, text %lx, background %lx%s", p0, p1, pfill,
             distinct ? "" : " (the text box cannot be told from the background)");
    say(line);
    vst_color(handle, 1);

    if (!fonts_check(sysfonts, &fonts) || lowmem)
        goto done;

    id = pick_font(fonts, sysfonts, chosen);
    snprintf(line, sizeof(line), "OUTLEDGE using font %d '%s'", id, chosen);
    say(line);
    if (!id || vst_font(handle, id) != id)
    {
        check(0, "the outline face is selected");
        goto done;
    }
    check(1, "the outline face is selected");

    metrics_check(id);
    if (!still_selected(id, "code"))
        goto done;
    codes_check();
    if (!still_selected(id, "clipping"))
        goto done;
    clip_check();
    if (!still_selected(id, "rotation"))
        goto done;
    rotation_check();
    if (!still_selected(id, "mode and effect"))
        goto done;
    modes_check();
    if (!still_selected(id, "leftover"))
        goto done;
    leftover_check(id);
    if (!still_selected(id, "justification"))
        goto done;
    justified_check();
    if (!still_selected(id, "fallback"))
        goto done;
    fallback_check();

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
    vst_font(handle, 1);
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);
out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI OUTLEDGE GUEST PASS");
    return failed;
}
