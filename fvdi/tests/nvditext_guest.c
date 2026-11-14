/*
 * nvditext_guest.c - the NVDI/SpeedoGDOS text calls a web browser uses,
 * on an outline font: vst_arbpt and vst_arbpt32, vst_charmap and
 * vst_map_mode with Unicode text, v_ftext and v_ftext_offset,
 * vqt_f_extent, vqt_advance and vqt_advance32, vst_setsize32, vst_skew,
 * vst_kern and vst_track_offset, vst_scratch, vqt_char_index,
 * vqt_fontheader, and vqt_xfntinfo and vqt_name_and_id by id; and a
 * workstation that never called vst_load_fonts selecting an outline font
 * by its id, as HighWire does at its start.
 *
 * The checks follow NVDI's documentation and compare a VDI with itself:
 * what is drawn is read back with vro_cpyfm into a memory form.  fVDI
 * 5c79666 fails a few of them: vst_map_mode reports nothing (no intout),
 * outline text ignores v_ftext_offset's offsets and vst_skew answers 0.
 *
 * Needs an outline font in the GDOS font folder.  Run under the AES.
 * Reports over BIOS device 1 (serial) and ends with
 * "FVDI NVDITEXT GUEST PASS" or "FVDI NVDITEXT FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  fh_fmver[8];
    unsigned long fh_fntsz, fh_fbfsz;
    unsigned short fh_cbfsz, fh_hedsz, fh_fntid, fh_sfvnr;
    char  fh_fntnm[70];
    char  fh_mdate[10];
    char  fh_laynm[70];
    char  fh_cpyrt[78];
    unsigned short fh_nchrl, fh_nchrf, fh_fchrf, fh_nktks, fh_nkprs;
    char  fh_flags, fh_cflgs, fh_famcl, fh_frmcl;
    char  fh_sfntn[32];
    char  fh_sfacn[16];
    char  fh_fntfm[14];
    unsigned short fh_itang, fh_orupm;
    char  room[200];            /* NVDI's header is longer */
} FHDR;

typedef struct {
    long  size;
    short format, id, index;
    char  font_name[50], family_name[50], style_name[50];
    char  file_name1[200], file_name2[200], file_name3[200];
    short pt_cnt, pt_sizes[64];
} XFNT_INFO;

short vst_arbpt(short handle, short point, short *wchar, short *hchar, short *wcell, short *hcell);
long vst_arbpt32(short handle, long point, short *wchar, short *hchar, short *wcell, short *hcell);
long vst_setsize32(short handle, long point, short *wchar, short *hchar, short *wcell, short *hcell);
short vst_map_mode(short handle, short mode);
void vst_charmap(short handle, short mode);
void vqt_f_extent(short handle, const char *str, short extent[]);
void vqt_f_extent16(short handle, const short *wstr, short extent[]);
void v_ftext(short handle, short x, short y, const char *str);
void v_ftext16(short handle, short x, short y, const short *wstr);
void v_ftext_offset(short handle, short x, short y, const char *str, const short *offset);
void vqt_advance(short handle, short ch, short *advx, short *advy, short *xrem, short *yrem);
void vqt_advance32(short handle, short ch, long *advx, long *advy);
short vst_skew(short handle, short skew);
void vst_kern(short handle, short tmode, short pmode, short *tracks, short *pairs);
void vst_track_offset(short handle, long offset, short pairmode, short *tracks, short *pairs);
void vst_scratch(short handle, short mode);
short vqt_char_index(short handle, short scr_index, short scr_mode, short dst_mode);
void vqt_fontheader(short handle, char *buffer, char *pathname);
short vqt_xfntinfo(short handle, short flags, short id, short index, XFNT_INFO *info);
short vqt_name_and_id(short handle, short font_format, char *font_name, char *ret_name);

#define MAP_ATARI_   1
#define MAP_UNICODE_ 2

static int failed;
static short handle, scr_w, scr_h, planes;
static unsigned long blank;             /* the value of a white pixel */

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

    snprintf(line, sizeof(line), "NVDITEXT %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI NVDITEXT FAIL: %s", what);
        say(line);
    }
}

/*
 * ===========================================================================
 * Reading the screen back
 * ===========================================================================
 */

typedef struct {
    short x, y, w, h, wdw;
    unsigned short *data;
} Grab;

typedef struct {
    short x0, y0, x1, y1;       /* box of the ink, x1 < x0 if none */
    long ink;
} Box;

static int grab(Grab *g, short x, short y, short w, short h)
{
    MFDB scr, mem;
    short pxy[8];

    g->x = x; g->y = y; g->w = w; g->h = h;
    g->wdw = (w + 15) >> 4;
    g->data = (unsigned short *)Malloc((long)g->wdw * 2 * planes * h);
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
    mem.fd_nplanes = planes;
    pxy[0] = x; pxy[1] = y; pxy[2] = x + w - 1; pxy[3] = y + h - 1;
    pxy[4] = 0; pxy[5] = 0; pxy[6] = w - 1; pxy[7] = h - 1;
    vs_clip_off(handle);
    vro_cpyfm(handle, S_ONLY, pxy, &scr, &mem);
    return 1;
}

static void ungrab(Grab *g)
{
    if (g->data)
        Mfree(g->data);
    g->data = NULL;
}

static unsigned long at(const Grab *g, short x, short y)
{
    const unsigned short *row = g->data + (long)y * g->wdw * planes;
    unsigned short bit;
    unsigned long v = 0;
    short p;

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

/* the working area: x 40..40+AREA_W, y 40..40+AREA_H */
#define AREA_X  40
#define AREA_Y  40
#define AREA_W  560
#define AREA_H  160

static void clear_area(void)
{
    short box[4];

    box[0] = AREA_X; box[1] = AREA_Y;
    box[2] = AREA_X + AREA_W - 1; box[3] = AREA_Y + AREA_H - 1;
    vs_clip_off(handle);
    vswr_mode(handle, MD_REPLACE);
    vsf_interior(handle, FIS_SOLID);
    vsf_color(handle, G_WHITE);
    vsf_perimeter(handle, 0);
    vr_recfl(handle, box);
}

/* the ink in the working area, columns from..to of it (relative) */
static void ink_box(const Grab *g, short from, short to, Box *b)
{
    short x, y;

    b->x0 = b->y0 = 32767;
    b->x1 = b->y1 = -32768;
    b->ink = 0;
    if (to >= g->w)
        to = g->w - 1;
    for (y = 0; y < g->h; y++)
        for (x = from; x <= to; x++)
            if (at(g, x, y) != blank)
            {
                b->ink++;
                if (x < b->x0) b->x0 = x;
                if (x > b->x1) b->x1 = x;
                if (y < b->y0) b->y0 = y;
                if (y > b->y1) b->y1 = y;
            }
}

static int area(Grab *g, Box *b)
{
    if (!grab(g, AREA_X, AREA_Y, AREA_W, AREA_H))
        return 0;
    ink_box(g, 0, AREA_W - 1, b);
    return 1;
}

/* the leftmost ink of row y, -1 if none */
static short left_ink(const Grab *g, short y)
{
    short x;

    for (x = 0; x < g->w; x++)
        if (at(g, x, y) != blank)
            return x;
    return -1;
}

/*
 * ===========================================================================
 * The checks
 * ===========================================================================
 */

static short pick_font(short fonts, char *chosen, short *index)
{
    char name[40];
    short i, id, first = 0, first_index = 0;

    for (i = 2; i <= fonts + 1; i++)
    {
        id = vqt_name(handle, i, name);
        if (id < 5000)
            continue;
        if (strstr(name, "Liberation Sans") && !strstr(name, "Bold") && !strstr(name, "Italic"))
        {
            strcpy(chosen, name);
            *index = i;
            return id;
        }
        if (!first && !strstr(name, "ingbat") && !strstr(name, "Bold") && !strstr(name, "Italic"))
        {
            first = id;
            first_index = i;
            strcpy(chosen, name);
        }
    }
    *index = first_index;
    if (first)
        vqt_name(handle, first_index, chosen);
    return first;
}

static void sizes(void)
{
    short cw, ch, bw, bh, ch12, ch14, ch13, got;
    long got32;
    char line[200];

    got = vst_point(handle, 12, &cw, &ch12, &bw, &bh);
    vst_point(handle, 14, &cw, &ch14, &bw, &bh);
    got = vst_arbpt(handle, 13, &cw, &ch13, &bw, &bh);
    snprintf(line, sizeof(line), "NVDITEXT vst_arbpt 13 -> %d, char height %d (12: %d, 14: %d)",
             got, ch13, ch12, ch14);
    say(line);
    check(got == 13, "vst_arbpt sets a size that is not in the size list");
    check(ch13 >= ch12 && ch13 <= ch14 && ch12 < ch14, "and its height lies between the listed sizes'");

    got32 = vst_arbpt32(handle, 0x000D8000L, &cw, &ch, &bw, &bh);     /* 13.5 */
    snprintf(line, sizeof(line), "NVDITEXT vst_arbpt32 13.5 -> %08lx, char height %d", got32, ch);
    say(line);
    check(ch >= ch12 && ch <= ch14, "vst_arbpt32 sets a size between 12 and 14 points");
    check(got32 == 0 || (got32 >= 0x000D0000L && got32 <= 0x000E0000L),
          "vst_arbpt32 returns the size it set, in 1/65536 points");
}

static void charmaps(short id)
{
    static const short hamburg16[] = { 'H', 'a', 'm', 'b', 'u', 'r', 'g', 0 };
    static const short adiaer16[] = { 0xe4, 0 };
    static const short emdash16[] = { 0x2014, 0x2014, 0 };
    short e1[8], e2[8], m, cw, ch, bw, bh, adv, advy, xr, yr;
    char line[200];
    Grab g;
    Box b;

    vst_font(handle, 1);
    m = vst_map_mode(handle, MAP_UNICODE_);
    snprintf(line, sizeof(line), "NVDITEXT system font: vst_map_mode(2) -> %d", m);
    say(line);
    check(m == MAP_ATARI_, "the system font keeps the Atari character set");

    vst_font(handle, id);
    vst_arbpt(handle, 24, &cw, &ch, &bw, &bh);
    m = vst_map_mode(handle, MAP_ATARI_);
    check(m == MAP_ATARI_, "vst_map_mode(1) on the outline font");
    vqt_f_extent(handle, "Hamburg", e1);
    vqt_f_extent(handle, "\x84", e2);           /* a-umlaut, Atari */
    m = vst_map_mode(handle, MAP_UNICODE_);
    snprintf(line, sizeof(line), "NVDITEXT outline font: vst_map_mode(2) -> %d", m);
    say(line);
    check(m == MAP_UNICODE_, "vst_map_mode(2) on the outline font selects Unicode");

    {
        short u1[8], u2[8], u3[8];

        vqt_f_extent16(handle, hamburg16, u1);
        vqt_f_extent16(handle, adiaer16, u2);
        vqt_f_extent16(handle, emdash16, u3);
        snprintf(line, sizeof(line), "NVDITEXT widths: Hamburg %d/%d, a-umlaut %d/%d, 2 em dashes %d",
                 e1[2] - e1[0], u1[2] - u1[0], e2[2] - e2[0], u2[2] - u2[0], u3[2] - u3[0]);
        say(line);
        check(u1[2] - u1[0] == e1[2] - e1[0] && u1[2] - u1[0] > 0,
              "ASCII text is as wide in Unicode as in the Atari set");
        check(u2[2] - u2[0] == e2[2] - e2[0] && u2[2] - u2[0] > 0,
              "U+00E4 is the Atari set's 0x84");
        check(u3[2] - u3[0] > 0, "U+2014, beyond the Atari set, has a width");
    }

    vqt_advance(handle, 0x2014, &adv, &advy, &xr, &yr);
    check(adv > 0 && advy == 0, "vqt_advance of U+2014");

    clear_area();
    vswr_mode(handle, MD_TRANS);
    vst_color(handle, G_BLACK);
    vst_alignment(handle, 0, 5, &m, &m);       /* top */
    v_ftext16(handle, AREA_X + 10, AREA_Y + 10, emdash16);
    vst_alignment(handle, 0, 0, &m, &m);
    if (area(&g, &b))
    {
        snprintf(line, sizeof(line), "NVDITEXT U+2014 drawn: ink %ld, box %d..%d x %d..%d",
                 b.ink, b.x0, b.x1, b.y0, b.y1);
        say(line);
        check(b.ink > 0, "v_ftext16 draws U+2014");
        ungrab(&g);
    }
    vst_map_mode(handle, MAP_ATARI_);
}

static void ftext(void)
{
    static const short offsets[] = { 0, 0, 60, 0, 40, 30 };
    Grab g1, g2;
    Box b1, b2;
    short x, y, same = 1, m, gapless;
    char line[200];

    vswr_mode(handle, MD_TRANS);
    vst_alignment(handle, 0, 5, &m, &m);

    clear_area();
    v_gtext(handle, AREA_X + 10, AREA_Y + 10, "Hamburgefonts");
    if (!area(&g1, &b1))
        return;
    clear_area();
    v_ftext(handle, AREA_X + 10, AREA_Y + 10, "Hamburgefonts");
    if (!area(&g2, &b2))
    {
        ungrab(&g1);
        return;
    }
    for (y = 0; y < AREA_H && same; y++)
        for (x = 0; x < AREA_W; x++)
            if ((at(&g1, x, y) != blank) != (at(&g2, x, y) != blank))
            {
                same = 0;
                break;
            }
    snprintf(line, sizeof(line), "NVDITEXT v_gtext ink %ld box %d..%d, v_ftext ink %ld box %d..%d",
             b1.ink, b1.x0, b1.x1, b2.ink, b2.x0, b2.x1);
    say(line);
    check(b1.ink > 0 && same, "v_ftext draws what v_gtext draws");
    ungrab(&g1);
    ungrab(&g2);

    /* offsets: 'I' at 0,0, the next at +60,0 from it, the last at +40,+30 from that */
    clear_area();
    v_ftext_offset(handle, AREA_X + 10, AREA_Y + 10, "III", offsets);
    if (!area(&g1, &b1))
        return;
    {
        Box first, second, third;

        ink_box(&g1, 0, 10 + 40, &first);
        ink_box(&g1, 10 + 41, 10 + 90, &second);
        ink_box(&g1, 10 + 91, AREA_W - 1, &third);
        snprintf(line, sizeof(line), "NVDITEXT v_ftext_offset: %d..%d/%d, %d..%d/%d, %d..%d/%d",
                 first.x0, first.x1, first.y0, second.x0, second.x1, second.y0,
                 third.x0, third.x1, third.y0);
        say(line);
        check(first.ink > 0 && second.ink > 0 && third.ink > 0 &&
              second.x0 - first.x0 >= 58 && second.x0 - first.x0 <= 62 &&
              third.x0 - second.x0 >= 38 && third.x0 - second.x0 <= 42 &&
              second.y0 == first.y0 && third.y0 - second.y0 >= 28 && third.y0 - second.y0 <= 32,
              "v_ftext_offset puts each character at its offset");
    }
    ungrab(&g1);

    /* without offsets, the same three letters stay together */
    clear_area();
    v_ftext(handle, AREA_X + 10, AREA_Y + 10, "III");
    if (area(&g1, &b1))
    {
        gapless = b1.ink > 0 && b1.x1 - b1.x0 < 40;
        check(gapless, "and v_ftext keeps them together");
        ungrab(&g1);
    }
    vst_alignment(handle, 0, 0, &m, &m);
}

static void advances(void)
{
    short adv, advy, xr, yr, cw, ch, bw, bh, e[8];
    long a32x, a32y;
    char line[200];

    vst_arbpt(handle, 24, &cw, &ch, &bw, &bh);
    vqt_advance(handle, 'W', &adv, &advy, &xr, &yr);
    vqt_advance32(handle, 'W', &a32x, &a32y);
    vqt_f_extent(handle, "W", e);
    snprintf(line, sizeof(line), "NVDITEXT vqt_advance 'W' %d,%d rem %d,%d; 32: %08lx,%08lx; extent %d",
             adv, advy, xr, yr, a32x, a32y, e[2] - e[0]);
    say(line);
    check(adv > 0 && advy == 0 && adv >= e[2] - e[0] - 3 && adv <= e[2] - e[0] + 6,
          "vqt_advance gives the pen advance of a character");
    check(adv > 0 && a32x >= ((long)adv << 16) - 0x10000L && a32x <= ((long)adv << 16) + 0x10000L &&
          a32y == 0,
          "vqt_advance32 gives it in 1/65536 pixels");
}

static void widths(void)
{
    short cw, ch, bw, bh, cw2, ch2, bw2, bh2, e1[8], e2[8], w1, w2;
    long got;
    char line[200];

    vst_arbpt(handle, 24, &cw, &ch, &bw, &bh);
    vqt_f_extent(handle, "Hamburgefonts", e1);
    got = vst_setsize32(handle, 12L << 16, &cw2, &ch2, &bw2, &bh2);
    vqt_f_extent(handle, "Hamburgefonts", e2);
    w1 = e1[2] - e1[0];
    w2 = e2[2] - e2[0];
    snprintf(line, sizeof(line), "NVDITEXT vst_setsize32 12 at 24 pt: -> %08lx, char %d->%d height %d->%d, text %d->%d",
             got, cw, cw2, ch, ch2, w1, w2);
    say(line);
    check(ch2 == ch && cw2 >= cw / 2 - 2 && cw2 <= cw / 2 + 2, "vst_setsize32 halves the width alone");
    check(w2 >= w1 * 2 / 5 && w2 <= w1 * 3 / 5, "and the text gets half as wide");
    check(got == 0 || (got >= (11L << 16) && got <= (13L << 16)), "vst_setsize32 returns the width set");
    vst_arbpt(handle, 24, &cw2, &ch2, &bw2, &bh2);
    check(cw2 == cw, "vst_arbpt gives the natural width back");
}

static void skew(void)
{
    short s, cw, ch, bw, bh, m, i, top, bottom, n, stem = 0;
    Grab g;
    Box b;
    long lean[2], want;
    char line[200];

    vst_arbpt(handle, 48, &cw, &ch, &bw, &bh);
    vswr_mode(handle, MD_TRANS);
    vst_alignment(handle, 0, 5, &m, &m);
    for (i = 0; i < 2; i++)
    {
        s = vst_skew(handle, i ? 150 : 0);
        if (i)
        {
            snprintf(line, sizeof(line), "NVDITEXT vst_skew 150 -> %d", s);
            say(line);
            check(s == 150, "vst_skew sets 15 degrees");
        }
        clear_area();
        v_ftext(handle, AREA_X + 40, AREA_Y + 10, "l");
        lean[i] = -9999;
        if (!area(&g, &b))
            continue;
        n = b.y1 - b.y0;
        top = left_ink(&g, b.y0 + n / 8);
        bottom = left_ink(&g, b.y1 - n / 8);
        if (top >= 0 && bottom >= 0)
            lean[i] = top - bottom;
        if (i)
            stem = n - 2 * (n / 8);     /* the rows between the two measured */
        snprintf(line, sizeof(line), "NVDITEXT skew %d: stem %d rows, leans %ld px", i ? 150 : 0, n, lean[i]);
        say(line);
        ungrab(&g);
    }
    vst_skew(handle, 0);
    vst_alignment(handle, 0, 0, &m, &m);
    /* tan 15 degrees is 0.268 */
    want = stem * 268L / 1000;
    snprintf(line, sizeof(line), "NVDITEXT skew 150 over %d rows: want %ld px", stem, want);
    say(line);
    check(lean[0] >= -2 && lean[0] <= 2, "an unskewed 'l' stands upright");
    check(stem > 10 && lean[1] >= want * 2 / 3 && lean[1] <= want * 3 / 2 + 2,
          "a skewed 'l' leans right by tan 15 degrees");
}

static void misc(short id, const char *fname, short index)
{
    short tracks = -1, pairs = -1, c1, c2, c3;
    char line[200], name[40], path[256];
    static FHDR fh;
    XFNT_INFO xi;

    vst_kern(handle, 1, 1, &tracks, &pairs);
    check(tracks >= 0 && pairs >= 0, "vst_kern answers");
    tracks = pairs = -1;
    vst_track_offset(handle, 0x10000L, 1, &tracks, &pairs);
    check(tracks >= 0 && pairs >= 0, "vst_track_offset answers");
    vst_scratch(handle, 0);

    c1 = vqt_char_index(handle, 'A', MAP_ATARI_, MAP_UNICODE_);
    c2 = vqt_char_index(handle, 0x9e, MAP_ATARI_, MAP_UNICODE_);
    c3 = vqt_char_index(handle, 0xdf, MAP_UNICODE_, MAP_ATARI_);
    snprintf(line, sizeof(line), "NVDITEXT vqt_char_index: A -> %04x, 0x9e -> %04x, U+00DF -> %04x",
             (unsigned short)c1, (unsigned short)c2, (unsigned short)c3);
    say(line);
    check(c1 == 0x41 && c2 == 0xdf && c3 == 0x9e, "vqt_char_index maps between Atari and Unicode");

    memset(&fh, 0x55, sizeof(fh));
    path[0] = 0;
    vqt_fontheader(handle, (char *)&fh, path);
    fh.fh_fntnm[sizeof(fh.fh_fntnm) - 1] = 0;
    snprintf(line, sizeof(line), "NVDITEXT vqt_fontheader: '%.4s' name '%s' upm %u", fh.fh_fmver,
             fh.fh_fntnm, fh.fh_orupm);
    say(line);
    check(!memcmp(fh.fh_fmver, "D1.0", 4) && !strncmp(fh.fh_fntnm, fname, 32) && fh.fh_orupm > 0,
          "vqt_fontheader describes the current font");

    memset(&xi, 0, sizeof(xi));
    c1 = vqt_xfntinfo(handle, 0x0007, id, 0, &xi);
    snprintf(line, sizeof(line), "NVDITEXT vqt_xfntinfo id %d: -> %d format %d index %d '%s' family '%s' style '%s'",
             id, c1, xi.format, xi.index, xi.font_name, xi.family_name, xi.style_name);
    say(line);
    check(c1 == id && xi.format == 4 && xi.index == index && !strncmp(xi.font_name, fname, 32) &&
          xi.family_name[0], "vqt_xfntinfo by id");
    memset(&xi, 0, sizeof(xi));
    c1 = vqt_xfntinfo(handle, 0x0001, 1, 0, &xi);
    check(c1 == 1 && xi.format == 1 && xi.index == 1, "vqt_xfntinfo of the system font");
    memset(&xi, 0, sizeof(xi));
    c1 = vqt_xfntinfo(handle, 0x0001, 4999, 0, &xi);
    check(c1 == 0, "vqt_xfntinfo of an id nobody has gives 0");

    c1 = vqt_name_and_id(handle, 0, (char *)fname, name);
    check(c1 == id && !strncmp(name, fname, 32), "vqt_name_and_id finds the font by its name");
    c1 = vqt_name_and_id(handle, 0, "No Such Font At All", name);
    check(c1 == 0, "and no font for a name nobody has");
}

/*
 * HighWire selects the fonts of its configuration before it calls
 * vst_load_fonts: a workstation that has not called it must find them
 */
static void unloaded(short id, const char *fname)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57], h2, cw, ch, bw, bh, got;
    char name[40], line[200];

    h2 = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &h2, work_out);
    check(h2 > 0, "a second workstation");
    if (h2 <= 0)
        return;
    got = vst_font(h2, id);
    vqt_name(h2, 1, name);
    snprintf(line, sizeof(line), "NVDITEXT second workstation, no vst_load_fonts: vst_font(%d) -> %d", id, got);
    say(line);
    check(got == id, "a workstation that did not load the fonts selects one by its id");
    got = vqt_name_and_id(h2, 0, (char *)fname, name);
    check(got == id, "and finds it by its name");
    v_clsvwk(h2);
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, fonts, id, index;
    char fname[40], line[200];

    say("NVDITEXT start");
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;
    vq_extnd(handle, 1, work_out);
    planes = work_out[4];

    fonts = vst_load_fonts(handle, 0);
    id = pick_font(fonts, fname, &index);
    snprintf(line, sizeof(line), "NVDITEXT %d fonts; using %d '%s' (index %d), screen %dx%dx%d",
             fonts, id, fname, index, scr_w, scr_h, planes);
    say(line);
    check(id != 0, "an outline font is loaded");
    if (failed)
        goto out;
    check(vst_font(handle, id) == id, "the outline font selected");

    /* the value of a blank pixel */
    {
        Grab g;

        clear_area();
        if (grab(&g, AREA_X, AREA_Y, 16, 1))
        {
            blank = at(&g, 0, 0);
            ungrab(&g);
        }
    }

    sizes();
    charmaps(id);
    ftext();
    advances();
    widths();
    skew();
    misc(id, fname, index);
    unloaded(id, fname);

out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI NVDITEXT GUEST PASS");
    return failed;
}
