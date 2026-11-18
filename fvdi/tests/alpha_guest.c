/*
 * alpha_guest.c - vr_transfer_bits with T_BLEND (64): images of 32 bits with
 * straight alpha (PX_ARGB32, PX_RGBA32) blended onto 16-bit pixels, checked
 * against the shared vectors of alpha_vectors.h.  One program for every VDI
 * that has the call (EmuTOS's, fVDI).
 *
 * The sum of the vectors is checked first.  Then:
 *
 * (a) vq_extnd: bits 0 and 1 of work_out[30] clear; bit 3 (T_BLEND) only
 *     on 16 planes, and then vq_px_format is a 16-bit format the vectors
 *     have (565 or 555, either byte order).  Without bit 3 only (f) runs,
 *     and the program ends "SKIPPED (not advertised)".
 * (b) every unclipped transfer of the vectors into memory in the screen's
 *     format holding its background: from ARGB, from RGBA (lines 8 bytes
 *     longer, the bitmap's origin at -7,5) and from ARGB with bit 7 (192).
 *     The CRC is the vectors', nothing beside the 32 x 24 pixels changes;
 *     the weight of each is set with vs_weight_color and put back.
 * (c) onto the screen, with vs_clip: every transfer (the clipped one with
 *     its rectangle, the others with the whole destination); the icon at 2x
 *     clipped at the left, right, top, bottom, on all four sides, with a
 *     clipping rectangle beside it, and cut by each of the four screen
 *     edges.  The pixels inside are (b)'s, those outside (a pattern around
 *     the background) stay, and so do the pixels under alpha 0.
 * (d) the weight: a new workstation reads 0x8000 and blends at half; each
 *     weight of the vectors reads back as set and gives its transfer; a
 *     second workstation, opened while the first has full weight, reads
 *     0x8000 and blends at half.
 * (e) modes 0 and 32 of ARGB and RGBA into memory: the image of the same
 *     pixels as xRGB32 in mode 0 (the alpha is not read).
 * (f) refusals: every one of the vectors the screen has a destination for,
 *     a source rectangle outside the source, and overlapping memory: the
 *     destination memory with 64 bytes on each side, the source and an
 *     area of the screen stay as they were.  First a pattern put in that
 *     area must read back, so that the comparisons see the screen.  The
 *     refusal onto "planes" runs wherever bit 3 is clear (except on a
 *     16-bit layout vq_px_format has no name for, which may blend without
 *     being advertised): T_BLEND then draws nothing, on 24 and 32 bits too.
 * (g) a v_open_bm bitmap in the program's memory, in the screen's format,
 *     as the destination: it starts at 0x8000 and blends at half; with full
 *     weight set on its handle (the screen workstation's own unchanged)
 *     every transfer gives the vectors' CRC.
 * (h) timing by the 200 Hz timer, in rounds of 200 calls until 40 ticks
 *     have passed, on the screen, of a 48 x 48 icon (40 % alpha 0, 50 %
 *     alpha 255, 10 % between) by T_BLEND;
 *     of the same icon by the mask path (vrt_cpyfm MD_TRANS of its mask of
 *     alpha >= 128, then vro_cpyfm S_AND_D of its device image); of it
 *     fully opaque and fully clear by T_BLEND: "ALPHA TIME <case>
 *     us=<microseconds a call>", and the ratio blend-mixed / mask-path.
 *
 * The screen is locked and the mouse off for all of it.  Run under the AES.
 * Reports over BIOS device 1: "ALPHA ok: <check>" or "ALPHA FAILED:
 * <check>", then "FVDI ALPHA GUEST PASS", "FVDI ALPHA GUEST SKIPPED (not
 * advertised)" or "FVDI ALPHA FAIL: <the first check that failed>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>
#include "alpha_vectors.h"

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

#define F_XRGB32    0x03421820UL
#define F_565       0x03021010UL
#define F_555       0x03420f10UL
#define F_INDEX8    0x01020808UL
#define F_REV       0x00800000UL

#define IW          ALPHA_IMG_W
#define IH          ALPHA_IMG_H

static short contrl[16], intin[16], ptsin[16], intout[64], ptsout[16];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
static int failed, skipped;
static long checks;
static short screen, planes, xres, yres;
static unsigned long fmt;           /* the screen's pixel format */
static int is555, swapped;          /* a 555 screen; the two bytes of a pixel swapped */
static int advertised;              /* bit 3 of work_out[30] */

static void say(const char *text)
{
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

static void report(int ok, const char *what)
{
    char line[240];

    checks++;
    snprintf(line, sizeof(line), "ALPHA %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI ALPHA FAIL: %s", what);
        say(line);
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

/* vr_transfer_bits: sr and dr are x1, y1, x2, y2; dst NULL is the workstation */
static void transfer(short h, const GCBITMAP *src, const GCBITMAP *dst, const short *sr, const short *dr, short mode)
{
    int i;

    memset(contrl, 0, sizeof(contrl));
    memcpy(&contrl[7], &src, sizeof(src));
    memcpy(&contrl[9], &dst, sizeof(dst));
    intin[0] = mode;
    intin[1] = intin[2] = intin[3] = 0;
    for (i = 0; i < 4; i++)
    {
        ptsin[i] = sr[i];
        ptsin[4 + i] = dr[i];
    }
    vdi_raw(h, 170, 0, 4, 4);
}

/* vs_weight_color (207/3) in colour space 1, RGB */
static void set_weight(short h, const unsigned short *w)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short)w[0];
    intin[4] = (short)w[1];
    intin[5] = (short)w[2];
    vdi_raw(h, 207, 3, 6, 0);
}

/* vq_weight_color (209/3): the colour space, and the weight in w */
static long get_weight(short h, unsigned short *w)
{
    int i;

    for (i = 0; i < 6; i++)
        intout[i] = 0x7777;
    vdi_raw(h, 209, 3, 0, 0);
    for (i = 0; i < 3; i++)
        w[i] = (unsigned short)intout[3 + i];
    return ((long)intout[0] << 16) | (unsigned short)intout[1];
}

static int weight_is(const unsigned short *w, const unsigned short *want)
{
    return w[0] == want[0] && w[1] == want[1] && w[2] == want[2];
}

/* vq_px_format (204/3) */
static unsigned long get_px_format(short h)
{
    intout[0] = intout[1] = intout[2] = intout[3] = 0;
    vdi_raw(h, 204, 3, 0, 0);
    return ((unsigned long)(unsigned short)intout[2] << 16) | (unsigned short)intout[3];
}

static void clip_off(short h)
{
    short r[4] = { 0, 0, 0, 0 };

    vs_clip(h, 0, r);
}

/* vs_clip to a rectangle given relative to (ox, oy) */
static void clip_at(short h, const short *cr, int ox, int oy)
{
    short r[4];

    r[0] = cr[0] + ox;
    r[1] = cr[1] + oy;
    r[2] = cr[2] + ox;
    r[3] = cr[3] + oy;
    vs_clip(h, 1, r);
}

/* ------------------------------------------------------------------ */
/* zlib's CRC-32 and the sum of the vectors */

static unsigned long crc_table[256];

static void crc_init(void)
{
    unsigned long c;
    int n, k;

    for (n = 0; n < 256; n++)
    {
        c = n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned long crc_byte(unsigned long crc, unsigned int byte)
{
    return crc_table[(crc ^ byte) & 255] ^ (crc >> 8);
}

/* the sum's unit: a scalar as 4 bytes, big-endian, a negative one sign-extended */
static unsigned long crc_word(unsigned long crc, long value)
{
    unsigned long u = (unsigned long)value;

    crc = crc_byte(crc, (u >> 24) & 255);
    crc = crc_byte(crc, (u >> 16) & 255);
    crc = crc_byte(crc, (u >> 8) & 255);
    return crc_byte(crc, u & 255);
}

static unsigned long vectors_sum(void)
{
    unsigned long crc = 0xffffffffUL;
    int i, j, k, t;

    for (i = 0; i < ALPHA_NWEIGHTS; i++)
        for (j = 0; j < 3; j++)
            crc = crc_word(crc, alpha_weight[i][j]);
    for (i = 0; i < ALPHA_NWEIGHTS; i++)
        for (j = 0; j < 256; j++)
            for (k = 0; k < 3; k++)
                crc = crc_word(crc, alpha_k[i][j][k]);
    for (i = 0; i < ALPHA_NPIXELS; i++)
    {
        crc = crc_word(crc, (long)alpha_pixel[i].argb);
        crc = crc_word(crc, alpha_pixel[i].d565);
        crc = crc_word(crc, alpha_pixel[i].e565);
        crc = crc_word(crc, alpha_pixel[i].d555);
        crc = crc_word(crc, alpha_pixel[i].e555);
        crc = crc_word(crc, alpha_pixel[i].weight);
    }
    for (t = 0; t < 4; t++)
    {
        crc = crc_word(crc, alpha_src[t].w);
        crc = crc_word(crc, alpha_src[t].h);
        for (i = 0; i < alpha_src[t].w * alpha_src[t].h; i++)
            crc = crc_word(crc, (long)alpha_src[t].argb[i]);
    }
    for (t = 0; t < 3; t++)
        for (i = 0; i < IH; i++)
            for (j = 0; j < IW; j++)
                crc = crc_word(crc, alpha_bg565[t][i][j]);
    for (t = 0; t < 3; t++)
        for (i = 0; i < IH; i++)
            for (j = 0; j < IW; j++)
                crc = crc_word(crc, alpha_bg555[t][i][j]);
    for (t = 0; t < ALPHA_NXFER; t++)
    {
        const AlphaXfer *x = &alpha_xfer[t];

        crc = crc_word(crc, x->src);
        crc = crc_word(crc, x->bg);
        crc = crc_word(crc, x->weight);
        crc = crc_word(crc, x->clip);
        for (i = 0; i < 4; i++)
            crc = crc_word(crc, x->sr[i]);
        for (i = 0; i < 4; i++)
            crc = crc_word(crc, x->dr[i]);
        for (i = 0; i < 4; i++)
            crc = crc_word(crc, x->cr[i]);
        crc = crc_word(crc, (long)x->crc565);
        crc = crc_word(crc, (long)x->crc555);
    }
    for (i = 0; i < 5; i++)
        for (j = 0; j < 7; j++)
            crc = crc_word(crc, alpha_ramp565[i][j]);
    for (t = 0; t < ALPHA_NREFUSAL; t++)
    {
        crc = crc_word(crc, (long)alpha_refusal[t].px_format);
        crc = crc_word(crc, alpha_refusal[t].mode);
        crc = crc_word(crc, alpha_refusal[t].dest);
    }
    return crc ^ 0xffffffffUL;
}

/* ------------------------------------------------------------------ */
/* 16-bit pixels in the screen's format, and the vectors' images */

static unsigned short get16(const unsigned char *p)
{
    return swapped ? (unsigned short)(p[0] | (p[1] << 8)) : (unsigned short)((p[0] << 8) | p[1]);
}

static void put16(unsigned char *p, unsigned int v)
{
    p[swapped ? 1 : 0] = (unsigned char)(v >> 8);
    p[swapped ? 0 : 1] = (unsigned char)v;
}

static unsigned short bgpix(int bg, int x, int y)
{
    return is555 ? alpha_bg555[bg][y][x] : alpha_bg565[bg][y][x];
}

static unsigned long want_crc(const AlphaXfer *e)
{
    return is555 ? e->crc555 : e->crc565;
}

/* the CRC of a 32 x 24 image as the vectors have it: each pixel 2 bytes big-endian */
static unsigned long img_crc(unsigned short pix[IH][IW])
{
    unsigned long crc = 0xffffffffUL;
    int x, y;

    for (y = 0; y < IH; y++)
        for (x = 0; x < IW; x++)
        {
            crc = crc_byte(crc, pix[y][x] >> 8);
            crc = crc_byte(crc, pix[y][x] & 255);
        }
    return crc ^ 0xffffffffUL;
}

static const char *const src_name[4] = { "icon", "ramp", "dot", "column" };
static const char *const kind_name[3] = { "ARGB", "RGBA", "xRGB32" };

static void xfer_name(int i, char *buf, size_t n)
{
    const AlphaXfer *e = &alpha_xfer[i];

    snprintf(buf, n, "xfer %d (%s to %d,%d-%d,%d%s, bg %d, weight %d)", i, src_name[e->src],
             e->dr[0], e->dr[1], e->dr[2], e->dr[3], e->clip ? " clipped" : "", e->bg, e->weight);
}

/* ------------------------------------------------------------------ */
/* sources */

enum { K_ARGB, K_RGBA, K_XRGB };

/* the largest vector source with the longer lines of RGBA: the icon, 16 x 12 */
#define SBUF (16 + (4 * 16 + 8) * 12 + 16)
static unsigned char sbuf[SBUF] __attribute__((aligned(2)));
static short org_x, org_y;          /* the origin of the source bitmap */

/*
 * vector source s as ARGB (lines of 4 w bytes, origin 0,0), RGBA (lines 8
 * bytes longer, origin -7,5) or xRGB32 (the spare byte 0x5a)
 */
static void mk_src(GCBITMAP *b, int s, int kind)
{
    int w = alpha_src[s].w, h = alpha_src[s].h, x, y;
    long pitch = 4L * w + (kind == K_RGBA ? 8 : 0);

    memset(sbuf, 0xc3, sizeof(sbuf));
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            unsigned long v = alpha_src[s].argb[y * w + x];
            unsigned char *p = sbuf + 16 + y * pitch + 4 * x;

            if (kind == K_RGBA)
            {
                p[0] = (unsigned char)(v >> 16);
                p[1] = (unsigned char)(v >> 8);
                p[2] = (unsigned char)v;
                p[3] = (unsigned char)(v >> 24);
            }
            else
            {
                p[0] = kind == K_XRGB ? 0x5a : (unsigned char)(v >> 24);
                p[1] = (unsigned char)(v >> 16);
                p[2] = (unsigned char)(v >> 8);
                p[3] = (unsigned char)v;
            }
        }
    org_x = kind == K_RGBA ? -7 : 0;
    org_y = kind == K_RGBA ? 5 : 0;
    memset(b, 0, sizeof(*b));
    b->magic = 0x6362746dL;
    b->length = sizeof(*b);
    b->addr = sbuf + 16;
    b->width = pitch;
    b->bits = 32;
    b->px_format = kind == K_ARGB ? ALPHA_PX_ARGB32 : kind == K_RGBA ? ALPHA_PX_RGBA32 : F_XRGB32;
    b->xmin = org_x;
    b->ymin = org_y;
    b->xmax = org_x + w;
    b->ymax = org_y + h;
}

/* the source rectangle of a transfer in the coordinates of the last mk_src */
static void src_rect(const AlphaXfer *e, short *sr)
{
    sr[0] = e->sr[0] + org_x;
    sr[1] = e->sr[1] + org_y;
    sr[2] = e->sr[2] + org_x;
    sr[3] = e->sr[3] + org_y;
}

/* ------------------------------------------------------------------ */
/* a 32 x 24 image in memory, with GUARD bytes each side and spare bytes at the line ends */

#define GUARD 64
#define MBUF (GUARD + 128 * IH + GUARD)

typedef struct {
    unsigned char *buf, *save;
    long pitch;                     /* bytes a line */
    long line;                      /* of which the pixels' */
} Mem;

static unsigned char dbuf[MBUF] __attribute__((aligned(2))), dsave[MBUF];
static unsigned char bbuf[MBUF] __attribute__((aligned(2))), bsave[MBUF];
static unsigned char fbuf[MBUF] __attribute__((aligned(2))), fsave[MBUF];

/* guards and spare bytes a pattern, the pixels the background; the state kept in m->save */
static void mem_fill(Mem *m, long pitch, int bg)
{
    long i;
    int x, y;

    m->pitch = pitch;
    m->line = 2L * IW;
    for (i = 0; i < MBUF; i++)
        m->buf[i] = (unsigned char)(i * 13 + 7);
    for (y = 0; y < IH; y++)
        for (x = 0; x < IW; x++)
            put16(m->buf + GUARD + y * pitch + 2 * x, bgpix(bg, x, y));
    memcpy(m->save, m->buf, MBUF);
}

/* every byte that is not a pixel is as it was */
static int mem_outside_same(const Mem *m)
{
    long i;

    for (i = 0; i < MBUF; i++)
    {
        long off = i - GUARD;
        int pixel = off >= 0 && off < m->pitch * IH && off % m->pitch < m->line;

        if (!pixel && m->buf[i] != m->save[i])
            return 0;
    }
    return 1;
}

/* the pixels, and their CRC */
static unsigned long mem_read(const Mem *m, unsigned short pix[IH][IW])
{
    int x, y;

    for (y = 0; y < IH; y++)
        for (x = 0; x < IW; x++)
            pix[y][x] = get16(m->buf + GUARD + y * m->pitch + 2 * x);
    return img_crc(pix);
}

static void mem_gcbitmap(GCBITMAP *b, const Mem *m)
{
    memset(b, 0, sizeof(*b));
    b->magic = 0x6362746dL;
    b->length = sizeof(*b);
    b->addr = m->buf + GUARD;
    b->width = m->pitch;
    b->bits = 16;
    b->px_format = fmt;
    b->xmax = IW;
    b->ymax = IH;
}

static Mem dmem = { dbuf, dsave, 64, 64 };
static unsigned short img[IH][IW], ref[IH][IW];

/*
 * transfer e from a source of 'kind' in 'mode' through handle h into memory
 * in the screen's format holding e's background (lines of 64 bytes, of 72
 * for RGBA); 'out' gets the pixels, *outside whether nothing else changed;
 * returns the CRC.  The weight is the workstation's as it is.
 */
static unsigned long draw_mem(short h, const AlphaXfer *e, int kind, short mode,
                              unsigned short out[IH][IW], int *outside)
{
    GCBITMAP src, dst;
    short sr[4];

    mk_src(&src, e->src, kind);
    src_rect(e, sr);
    mem_fill(&dmem, kind == K_RGBA ? 72 : 64, e->bg);
    mem_gcbitmap(&dst, &dmem);
    transfer(h, &src, &dst, sr, e->dr, mode);
    *outside = mem_outside_same(&dmem);
    return mem_read(&dmem, out);
}

/* ------------------------------------------------------------------ */
/* (b) into memory */

static unsigned short base_weight[3];   /* the screen workstation's weight between checks */

static void test_memory(void)
{
    static const int kinds[3] = { K_ARGB, K_RGBA, K_ARGB };
    static const short modes[3] = { ALPHA_T_BLEND, ALPHA_T_BLEND, ALPHA_T_BLEND | 128 };
    static const char *const run_name[3] = { "ARGB", "RGBA", "ARGB with bit 7" };
    char name[100], text[240];
    int i, r, outside, ok;
    unsigned long crc;

    get_weight(screen, base_weight);
    for (i = 0; i < ALPHA_NXFER; i++)
    {
        const AlphaXfer *e = &alpha_xfer[i];

        if (e->clip)
            continue;
        xfer_name(i, name, sizeof(name));
        ok = 1;
        set_weight(screen, alpha_weight[e->weight]);
        for (r = 0; r < 3; r++)
        {
            crc = draw_mem(screen, e, kinds[r], modes[r], img, &outside);
            if (crc != want_crc(e) || !outside)
            {
                snprintf(text, sizeof(text), "ALPHA   %s from %s: crc %08lx want %08lx%s", name, run_name[r],
                         crc, want_crc(e), outside ? "" : ", bytes beside the pixels changed");
                say(text);
                ok = 0;
            }
        }
        set_weight(screen, base_weight);
        snprintf(text, sizeof(text), "(b) %s into memory from ARGB, RGBA and ARGB with bit 7", name);
        report(ok, text);
    }
}

/* ------------------------------------------------------------------ */
/* the screen: a 32 x 24 destination with a border of pattern, copied by vro_cpyfm */

#define OX  64                      /* where the destination is put on the screen */
#define OY  80
#define BD  4                       /* the border around it */
#define SCW (IW + 2 * BD)
#define SCH (IH + 2 * BD)
#define SCWW 3                      /* words a line of a plane in the MFDBs: 48 pixels */
#define SCBUF (SCWW * 2 * 32 * SCH)     /* room for 32 planes (the refusals run on any screen) */

static unsigned char scene[SCBUF] __attribute__((aligned(2))), before[SCBUF] __attribute__((aligned(2))),
                     after[SCBUF] __attribute__((aligned(2)));

static long scene_pitch(void)
{
    return SCWW * 2L * planes;
}

static unsigned short pattern(int x, int y)
{
    return (unsigned short)(0x39e7 ^ (x * 0x0821) ^ (y * 0x1863));
}

/* the border and, inside it, background bg */
static void mk_scene(int bg)
{
    int x, y;

    for (y = 0; y < SCH; y++)
        for (x = 0; x < SCW; x++)
        {
            int in = x >= BD && x < BD + IW && y >= BD && y < BD + IH;

            put16(scene + y * scene_pitch() + 2 * x, in ? bgpix(bg, x - BD, y - BD) : pattern(x, y));
        }
}

/* the part of the scene on the screen when its destination is at (ox, oy) */
typedef struct { int x0, y0, x1, y1; } Vis;

static int scene_vis(int ox, int oy, Vis *v)
{
    int left = ox - BD, top = oy - BD;

    v->x0 = left < 0 ? -left : 0;
    v->y0 = top < 0 ? -top : 0;
    v->x1 = left + SCW - 1 > xres ? xres - left : SCW - 1;
    v->y1 = top + SCH - 1 > yres ? yres - top : SCH - 1;
    return v->x0 <= v->x1 && v->y0 <= v->y1;
}

/* copy the visible part of a scene buffer onto the screen, or from it; clipping must be off */
static void scene_copy(int ox, int oy, unsigned char *buf, int to_screen)
{
    MFDB mem, scr;
    short xy[8];
    Vis v;
    int left = ox - BD, top = oy - BD;

    if (!scene_vis(ox, oy, &v))
        return;
    memset(&mem, 0, sizeof(mem));
    memset(&scr, 0, sizeof(scr));
    mem.fd_addr = buf;
    mem.fd_w = SCWW * 16;
    mem.fd_h = SCH;
    mem.fd_wdwidth = SCWW;
    mem.fd_stand = 0;
    mem.fd_nplanes = planes;
    if (to_screen)
    {
        xy[0] = v.x0;
        xy[1] = v.y0;
        xy[2] = v.x1;
        xy[3] = v.y1;
        xy[4] = left + v.x0;
        xy[5] = top + v.y0;
        xy[6] = left + v.x1;
        xy[7] = top + v.y1;
        vro_cpyfm(screen, S_ONLY, xy, &mem, &scr);
    }
    else
    {
        xy[0] = left + v.x0;
        xy[1] = top + v.y0;
        xy[2] = left + v.x1;
        xy[3] = top + v.y1;
        xy[4] = v.x0;
        xy[5] = v.y0;
        xy[6] = v.x1;
        xy[7] = v.y1;
        vro_cpyfm(screen, S_ONLY, xy, &scr, &mem);
    }
}

static void grab(int ox, int oy, unsigned char *buf)
{
    memset(buf, 0x6b, SCBUF);
    scene_copy(ox, oy, buf, 0);
}

/* put the scene for background bg at (ox, oy) and read it back: TRUE if it is there */
static int put_scene(int bg, int ox, int oy)
{
    Vis v;
    int x, y;

    mk_scene(bg);
    clip_off(screen);
    scene_copy(ox, oy, scene, 1);
    grab(ox, oy, before);
    if (!scene_vis(ox, oy, &v))
        return 0;
    for (y = v.y0; y <= v.y1; y++)
        for (x = v.x0; x <= v.x1; x++)
            if (get16(before + y * scene_pitch() + 2 * x) != get16(scene + y * scene_pitch() + 2 * x))
                return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* (c) onto the screen */

/* every transfer at (OX, OY), clipped to its rectangle or to the whole destination */
static void screen_xfer(int i)
{
    static const short whole[4] = { 0, 0, IW - 1, IH - 1 };
    const AlphaXfer *e = &alpha_xfer[i];
    GCBITMAP src;
    short sr[4], dr[4];
    char name[100], text[240];
    int kind, x, y, ok = 1, border, put;
    unsigned long crc;

    xfer_name(i, name, sizeof(name));
    set_weight(screen, alpha_weight[e->weight]);
    for (kind = K_ARGB; kind <= K_RGBA; kind++)
    {
        put = put_scene(e->bg, OX, OY);
        mk_src(&src, e->src, kind);
        src_rect(e, sr);
        dr[0] = e->dr[0] + OX;
        dr[1] = e->dr[1] + OY;
        dr[2] = e->dr[2] + OX;
        dr[3] = e->dr[3] + OY;
        clip_at(screen, e->clip ? e->cr : whole, OX, OY);
        transfer(screen, &src, NULL, sr, dr, ALPHA_T_BLEND);
        clip_off(screen);
        grab(OX, OY, after);
        border = 0;
        for (y = 0; y < SCH; y++)
            for (x = 0; x < SCW; x++)
            {
                unsigned short v = get16(after + y * scene_pitch() + 2 * x);

                if (x >= BD && x < BD + IW && y >= BD && y < BD + IH)
                    img[y - BD][x - BD] = v;
                else if (v != get16(before + y * scene_pitch() + 2 * x))
                    border++;
            }
        crc = img_crc(img);
        if (!put || crc != want_crc(e) || border)
        {
            snprintf(text, sizeof(text), "ALPHA   %s on the screen from %s: crc %08lx want %08lx, %d pixels beside changed%s",
                     name, kind_name[kind], crc, want_crc(e), border, put ? "" : ", the background did not arrive");
            say(text);
            ok = 0;
        }
    }
    set_weight(screen, base_weight);
    snprintf(text, sizeof(text), "(c) %s onto the screen from ARGB and RGBA, %s", name,
             e->clip ? "clipped to its rectangle" : "the clip the whole destination");
    report(ok, text);
}

/* the icon at 2x onto (0,0)-(31,23) of background bg, unclipped */
static int icon2x(int bg)
{
    int i;

    for (i = 0; i < ALPHA_NXFER; i++)
    {
        const AlphaXfer *e = &alpha_xfer[i];

        if (e->src == 0 && e->bg == bg && !e->clip && e->weight == 0 &&
            e->dr[0] == 0 && e->dr[1] == 0 && e->dr[2] == IW - 1 && e->dr[3] == IH - 1)
            return i;
    }
    return -1;
}

typedef struct {
    const char *what;
    int edge;                       /* 0: at (OX, OY) with the clip cr; 1-4: cut by the left, right, top, bottom screen edge, no clip */
    int draws;                      /* whether anything is blended: not with the clip beside the destination */
    short cr[4];
} ClipCase;

static const ClipCase clip_cases[10] = {
    { "clipped at the left", 0, 1, { 9, 0, IW - 1, IH - 1 } },
    { "clipped at the right", 0, 1, { 0, 0, 21, IH - 1 } },
    { "clipped at the top", 0, 1, { 0, 7, IW - 1, IH - 1 } },
    { "clipped at the bottom", 0, 1, { 0, 0, IW - 1, 15 } },
    { "clipped on all four sides", 0, 1, { 7, 5, 24, 18 } },
    { "with a clipping rectangle beside it", 0, 0, { IW + 1, 0, IW + 13, IH - 1 } },
    { "cut by the left screen edge", 1, 1, { 0, 0, 0, 0 } },
    { "cut by the right screen edge", 2, 1, { 0, 0, 0, 0 } },
    { "cut by the top screen edge", 3, 1, { 0, 0, 0, 0 } },
    { "cut by the bottom screen edge", 4, 1, { 0, 0, 0, 0 } },
};

static void screen_clips(void)
{
    const AlphaXfer *e;
    GCBITMAP src;
    short sr[4], dr[4];
    char text[240];
    int bg, c, i, x, y, ox, oy, kind, bad, under0, drawn, outside, put;
    unsigned long crc;
    Vis v;

    set_weight(screen, alpha_weight[0]);
    for (bg = 0; bg < 3; bg++)
    {
        i = icon2x(bg);
        if (i < 0)
        {
            report(0, "(c) the vectors have the icon at 2x on each background");
            break;
        }
        e = &alpha_xfer[i];
        crc = draw_mem(screen, e, K_ARGB, ALPHA_T_BLEND, ref, &outside);
        snprintf(text, sizeof(text), "(c) the icon at 2x on bg %d in memory, the reference of the clipped draws (crc %08lx want %08lx)",
                 bg, crc, want_crc(e));
        report(crc == want_crc(e) && outside, text);
        kind = (bg & 1) ? K_RGBA : K_ARGB;
        for (c = 0; c < 10; c++)
        {
            const ClipCase *k = &clip_cases[c];

            ox = k->edge == 1 ? -11 : k->edge == 2 ? xres + 1 - 21 : OX;
            oy = k->edge == 3 ? -9 : k->edge == 4 ? yres + 1 - 13 : OY;
            put = put_scene(bg, ox, oy);
            mk_src(&src, e->src, kind);
            src_rect(e, sr);
            dr[0] = e->dr[0] + ox;
            dr[1] = e->dr[1] + oy;
            dr[2] = e->dr[2] + ox;
            dr[3] = e->dr[3] + oy;
            if (k->edge == 0)
                clip_at(screen, k->cr, ox, oy);
            transfer(screen, &src, NULL, sr, dr, ALPHA_T_BLEND);
            clip_off(screen);
            grab(ox, oy, after);
            bad = under0 = drawn = 0;
            scene_vis(ox, oy, &v);
            for (y = v.y0; y <= v.y1; y++)
                for (x = v.x0; x <= v.x1; x++)
                {
                    int dx = x - BD, dy = y - BD;
                    int in = dx >= 0 && dx < IW && dy >= 0 && dy < IH;
                    unsigned short was = get16(before + y * scene_pitch() + 2 * x);
                    unsigned short got = get16(after + y * scene_pitch() + 2 * x);
                    unsigned short want = was;

                    if (in && (k->edge != 0 || (dx >= k->cr[0] && dx <= k->cr[2] && dy >= k->cr[1] && dy <= k->cr[3])))
                        want = ref[dy][dx];
                    if (got != want)
                        bad++;
                    if (want != was)
                        drawn++;
                    /* the icon at 2x: destination pixel (dx, dy) is source pixel (dx / 2, dy / 2) */
                    if (in && (alpha_src[0].argb[(dy / 2) * 16 + dx / 2] >> 24) == 0 && got != was)
                        under0++;
                }
            snprintf(text, sizeof(text), "(c) the icon at 2x %s, bg %d, from %s: %d pixels differ, %d under alpha 0 changed (%d blended)%s",
                     k->what, bg, kind_name[kind], bad, under0, drawn, put ? "" : ", the background did not arrive");
            report(put && bad == 0 && under0 == 0 && (drawn > 0) == k->draws, text);
        }
    }
    set_weight(screen, base_weight);
}

/* ------------------------------------------------------------------ */
/* (d) the weight colour */

static short open_vwk(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, chh, bw, bh, h;

    h = graf_handle(&cw, &chh, &bw, &bh);
    v_opnvwk(work_in, &h, work_out);
    return h;
}

/* the icon 1:1 onto background 0 with weight w, or -1 */
static int icon1(int w)
{
    int i;

    for (i = 0; i < ALPHA_NXFER; i++)
    {
        const AlphaXfer *e = &alpha_xfer[i];

        if (e->src == 0 && e->bg == 0 && !e->clip && e->weight == w &&
            e->dr[0] == 0 && e->dr[1] == 0 && e->dr[2] == 15 && e->dr[3] == 11)
            return i;
    }
    return -1;
}

static void test_weight(void)
{
    unsigned short w[3];
    long space;
    unsigned long crc;
    short h2;
    int k, i, outside;
    char text[200];

    for (k = 0; k < ALPHA_NWEIGHTS; k++)
        if (icon1(k) < 0)
        {
            report(0, "(d) the vectors have the icon 1:1 on background 0 with each weight");
            return;
        }
    space = get_weight(screen, w);
    snprintf(text, sizeof(text), "(d) a new workstation's weight is 8000 8000 8000 (vq_weight_color: space %ld, %04x %04x %04x)",
             space, w[0], w[1], w[2]);
    report(space == 1 && weight_is(w, alpha_weight[1]), text);
    i = icon1(1);
    crc = draw_mem(screen, &alpha_xfer[i], K_ARGB, ALPHA_T_BLEND, img, &outside);
    snprintf(text, sizeof(text), "(d) the default weight blends as the weight-1 transfer, half (crc %08lx)", crc);
    report(crc == want_crc(&alpha_xfer[i]) && outside, text);

    for (k = 0; k < ALPHA_NWEIGHTS; k++)
    {
        set_weight(screen, alpha_weight[k]);
        space = get_weight(screen, w);
        i = icon1(k);
        crc = draw_mem(screen, &alpha_xfer[i], K_ARGB, ALPHA_T_BLEND, img, &outside);
        snprintf(text, sizeof(text), "(d) weight %04x %04x %04x: read back as %04x %04x %04x, and it gives xfer %d (crc %08lx)",
                 alpha_weight[k][0], alpha_weight[k][1], alpha_weight[k][2], w[0], w[1], w[2], i, crc);
        report(space == 1 && weight_is(w, alpha_weight[k]) && crc == want_crc(&alpha_xfer[i]) && outside, text);
    }

    set_weight(screen, alpha_weight[0]);
    h2 = open_vwk();
    report(h2 > 0 && h2 != screen, "(d) a second virtual workstation");
    if (h2 > 0 && h2 != screen)
    {
        space = get_weight(h2, w);
        snprintf(text, sizeof(text), "(d) a second workstation, opened while the first has full weight, reads %04x %04x %04x",
                 w[0], w[1], w[2]);
        report(space == 1 && weight_is(w, alpha_weight[1]), text);
        i = icon1(1);
        crc = draw_mem(h2, &alpha_xfer[i], K_ARGB, ALPHA_T_BLEND, img, &outside);
        report(crc == want_crc(&alpha_xfer[i]) && outside, "(d) and blends at half");
        get_weight(screen, w);
        report(weight_is(w, alpha_weight[0]), "(d) the first workstation keeps its full weight");
        v_clsvwk(h2);
    }
    set_weight(screen, alpha_weight[1]);
}

/* ------------------------------------------------------------------ */
/* (e) modes 0 and 32 */

static void test_copy_modes(void)
{
    static const short modes[2] = { 0, 32 };
    char name[100], text[240];
    int i, k, m, x, y, drawn, outside, ok;

    for (i = 0; i < ALPHA_NXFER; i++)
    {
        const AlphaXfer *e = &alpha_xfer[i];

        if (e->clip || e->weight != 0 || e->bg != 1)
            continue;
        xfer_name(i, name, sizeof(name));
        draw_mem(screen, e, K_XRGB, 0, ref, &outside);
        ok = outside;
        for (drawn = 0, y = 0; y < IH; y++)
            for (x = 0; x < IW; x++)
                if (ref[y][x] != bgpix(e->bg, x, y))
                    drawn++;
        if (!drawn)
        {
            snprintf(text, sizeof(text), "ALPHA   %s: mode 0 of the xRGB32 image draws nothing", name);
            say(text);
            ok = 0;
        }
        for (k = K_ARGB; k <= K_RGBA; k++)
            for (m = 0; m < 2; m++)
            {
                draw_mem(screen, e, k, modes[m], img, &outside);
                if (memcmp(img, ref, sizeof(img)) != 0 || !outside)
                {
                    snprintf(text, sizeof(text), "ALPHA   %s: mode %d from %s is not the xRGB32 image%s", name, modes[m],
                             kind_name[k], outside ? "" : " (bytes beside the pixels changed)");
                    say(text);
                    ok = 0;
                }
            }
        snprintf(text, sizeof(text), "(e) %s: modes 0 and 32 from ARGB and RGBA give the xRGB32 image", name);
        report(ok, text);
    }
}

/* ------------------------------------------------------------------ */
/* (f) refusals */

static unsigned char ssave[SBUF];

/* a memory destination of the refusals: MEM16, INDEX8, ARGB32 or XRGB32 */
static void mk_fdst(GCBITMAP *b, int dest)
{
    long i;

    for (i = 0; i < MBUF; i++)
        fbuf[i] = (unsigned char)(i * 29 + 11);
    memset(b, 0, sizeof(*b));
    b->magic = 0x6362746dL;
    b->length = sizeof(*b);
    b->addr = fbuf + GUARD;
    b->xmax = IW;
    b->ymax = IH;
    switch (dest)
    {
    case ALPHA_DEST_MEM16:
        b->px_format = planes == 16 && (fmt & 255) == 16 ? fmt : F_565;
        b->bits = 16;
        b->width = 2 * IW;
        break;
    case ALPHA_DEST_INDEX8:
        b->px_format = F_INDEX8;
        b->bits = 8;
        b->width = IW;
        break;
    case ALPHA_DEST_ARGB32:
        b->px_format = ALPHA_PX_ARGB32;
        b->bits = 32;
        b->width = 4 * IW;
        break;
    default:
        b->px_format = F_XRGB32;
        b->bits = 32;
        b->width = 4 * IW;
        break;
    }
}

/* a source of a refusal: the icon's bytes, described as px with the bits it says */
static void mk_fsrc(GCBITMAP *b, unsigned long px)
{
    long bits = px & 255;

    mk_src(b, 0, K_ARGB);
    b->px_format = px;
    b->bits = bits;
    b->width = bits >= 8 ? 16 * (bits / 8) : 2;
}

/* one call that must do nothing: fbuf, the source and the screen area stay */
static void refused(const char *what, const GCBITMAP *src, const GCBITMAP *dst, const short *sr, const short *dr, short mode)
{
    char text[240];
    int same_mem, same_src, same_scr, grabbed;
    long i;

    memcpy(fsave, fbuf, MBUF);
    memcpy(ssave, sbuf, SBUF);
    clip_off(screen);
    grab(OX, OY, before);
    transfer(screen, src, dst, sr, dr, mode);
    grab(OX, OY, after);
    same_mem = memcmp(fbuf, fsave, MBUF) == 0;
    same_src = memcmp(sbuf, ssave, SBUF) == 0;
    same_scr = memcmp(before, after, SCBUF) == 0;
    for (grabbed = 0, i = 0; i < SCBUF && !grabbed; i++)
        grabbed = before[i] != 0x6b;
    snprintf(text, sizeof(text), "(f) refused: %s%s%s%s%s", what, same_mem ? "" : " (memory changed)",
             same_src ? "" : " (source changed)", same_scr ? "" : " (screen changed)",
             grabbed ? "" : " (the screen grab read nothing)");
    report(same_mem && same_src && same_scr && grabbed, text);
}

/*
 * the refusals compare the screen area at (OX, OY) before and after a call:
 * a pattern of bytes put there in the screen's own format must read back.
 * The first 32 pixels of each line are whole words in every format, packed
 * or with planes: 4 bytes a plane.
 */
static int grab_reads_screen(void)
{
    long i;
    int y;

    for (i = 0; i < SCBUF; i++)
        scene[i] = (unsigned char)(i * 7 + 3);
    clip_off(screen);
    scene_copy(OX, OY, scene, 1);
    grab(OX, OY, before);
    for (y = 0; y < SCH; y++)
        if (memcmp(before + y * scene_pitch(), scene + y * scene_pitch(), 4L * planes) != 0)
            return 0;
    return 1;
}

static void test_refusals(void)
{
    static const char *const dest_name[6] = {
        "the screen", "memory of 16 bits", "memory of 8-bit indices", "ARGB memory", "xRGB32 memory",
        "the screen, bit 3 clear"
    };
    static const short sr[4] = { 0, 0, 15, 11 }, mdr[4] = { 3, 1, 18, 12 };
    short sdr[4], r2[4];
    GCBITMAP src, dst, d2;
    char what[160];
    int i;

    sdr[0] = OX + 3;
    sdr[1] = OY + 1;
    sdr[2] = OX + 18;
    sdr[3] = OY + 12;
    snprintf(what, sizeof(what), "(f) a pattern put in the screen area of the refusals reads back (%d planes)", planes);
    report(grab_reads_screen(), what);
    for (i = 0; i < ALPHA_NREFUSAL; i++)
    {
        const AlphaRefusal *r = &alpha_refusal[i];
        int to_screen = r->dest == ALPHA_DEST_SCREEN || r->dest == ALPHA_DEST_PLANES;

        /* "planes": a screen where T_BLEND is not advertised, where it must draw nothing;
           a 16-bit layout without a name may blend all the same */
        if (r->dest == ALPHA_DEST_PLANES && (advertised || (planes == 16 && fmt == 0)))
        {
            snprintf(what, sizeof(what), "ALPHA refusal %d (format %08lx mode %d onto the screen, bit 3 clear): not here, %s",
                     i, r->px_format, r->mode, advertised ? "bit 3 is set" : "a 16-bit layout without a name");
            say(what);
            continue;
        }
        if (r->dest == ALPHA_DEST_MEM16 && planes != 16)
        {
            snprintf(what, sizeof(what), "ALPHA refusal %d (format %08lx mode %d into memory of 16 bits): not on %d planes",
                     i, r->px_format, r->mode, planes);
            say(what);
            continue;
        }
        mk_fdst(&dst, to_screen ? ALPHA_DEST_ARGB32 : r->dest);
        mk_fsrc(&src, r->px_format);
        snprintf(what, sizeof(what), "format %08lx mode %d onto %s", r->px_format, r->mode, dest_name[r->dest]);
        refused(what, &src, to_screen ? NULL : &dst, sr, to_screen ? sdr : mdr, r->mode);
    }

    /* a source rectangle outside the source */
    mk_fdst(&dst, ALPHA_DEST_MEM16);
    mk_src(&src, 0, K_ARGB);
    memcpy(r2, sr, sizeof(r2));
    r2[2] = 16;
    refused("T_BLEND, a source rectangle one pixel wider than the source, onto the screen", &src, NULL, r2, sdr, ALPHA_T_BLEND);
    if (planes == 16)
        refused("T_BLEND, a source rectangle one pixel wider than the source, into memory", &src, &dst, r2, mdr, ALPHA_T_BLEND);
    memcpy(r2, sr, sizeof(r2));
    r2[1] = -1;
    refused("T_BLEND, a source rectangle above the source, onto the screen", &src, NULL, r2, sdr, ALPHA_T_BLEND);
    if (planes == 16)
        refused("T_BLEND, a source rectangle above the source, into memory", &src, &dst, r2, mdr, ALPHA_T_BLEND);

    /* overlapping memory */
    if (planes == 16)
    {
        static const short ddr[4] = { 0, 0, 15, 5 };

        d2 = dst;
        d2.addr = sbuf + 16 + 128;          /* 16 x 6 pixels in the middle of the source */
        d2.width = 32;
        d2.xmax = 16;
        d2.ymax = 6;
        refused("T_BLEND, a destination inside the source", &src, &d2, sr, ddr, ALPHA_T_BLEND);
        mk_fdst(&dst, ALPHA_DEST_MEM16);
        mk_src(&src, 0, K_ARGB);
        src.addr = fbuf + GUARD + 256;      /* the source inside the destination */
        refused("T_BLEND, a source inside the destination", &src, &dst, sr, mdr, ALPHA_T_BLEND);
    }
    mk_src(&src, 0, K_ARGB);
    src.addr = (unsigned char *)Logbase();
    refused("T_BLEND, a source in the screen's memory onto the screen", &src, NULL, sr, sdr, ALPHA_T_BLEND);
}

/* ------------------------------------------------------------------ */
/* (g) into a bitmap workstation */

static Mem bmem = { bbuf, bsave, 72, 64 };

static void test_bitmap(void)
{
    static const short whole[4] = { 0, 0, IW - 1, IH - 1 };
    GCBITMAP bm, src;
    unsigned short w[3];
    short hb, sr[4];
    char name[100], text[240];
    int i, kind, ok;
    unsigned long crc;

    /* the screen workstation has a weight of its own meanwhile */
    set_weight(screen, alpha_weight[3]);
    mem_fill(&bmem, 72, 0);
    mem_gcbitmap(&bm, &bmem);
    hb = v_open_bm(screen, &bm, 0, 0, 0, 0);
    if (hb <= 0)
    {
        say("ALPHA (g) skipped: v_open_bm does not open a bitmap in the program's memory");
        set_weight(screen, base_weight);
        return;
    }
    get_weight(hb, w);
    snprintf(text, sizeof(text), "(g) a new bitmap workstation's weight is 8000 8000 8000 (%04x %04x %04x)", w[0], w[1], w[2]);
    report(weight_is(w, alpha_weight[1]), text);

    for (i = -1; i < ALPHA_NXFER; i++)
    {
        /* first (i = -1) the weight-1 transfer at the bitmap's default weight */
        const AlphaXfer *e = &alpha_xfer[i < 0 ? icon1(1) : i];

        if (i < 0)
            snprintf(name, sizeof(name), "the default weight blends at half");
        else
            xfer_name(i, name, sizeof(name));
        if (i == 0)
        {
            set_weight(hb, alpha_weight[0]);
            get_weight(screen, w);
            report(weight_is(w, alpha_weight[3]), "(g) full weight set on the bitmap leaves the screen workstation's weight");
        }
        ok = 1;
        if (i >= 0)
            set_weight(hb, alpha_weight[e->weight]);
        for (kind = K_ARGB; kind <= K_RGBA; kind++)
        {
            mem_fill(&bmem, 72, e->bg);
            mk_src(&src, e->src, kind);
            src_rect(e, sr);
            clip_at(hb, e->clip ? e->cr : whole, 0, 0);
            transfer(hb, &src, NULL, sr, e->dr, ALPHA_T_BLEND);
            clip_off(hb);
            crc = mem_read(&bmem, img);
            if (crc != want_crc(e) || !mem_outside_same(&bmem))
            {
                snprintf(text, sizeof(text), "ALPHA   (g) %s from %s: crc %08lx want %08lx%s", name, kind_name[kind], crc,
                         want_crc(e), mem_outside_same(&bmem) ? "" : ", bytes beside the bitmap changed");
                say(text);
                ok = 0;
            }
        }
        if (i >= 0)
            set_weight(hb, alpha_weight[0]);
        snprintf(text, sizeof(text), "(g) %s into the bitmap, from ARGB and RGBA", name);
        report(ok, text);
    }
    v_clsbm(hb);
    set_weight(screen, base_weight);
}

/* ------------------------------------------------------------------ */
/* (h) timing */

#define TW 48
#define TCALLS 200
#define TTICKS 40
#define TMAXCALLS 40000L
#define TX (OX + 48)
#define TY (OY + 40)

static unsigned char tmix[TW * TW * 4] __attribute__((aligned(2))), topq[TW * TW * 4] __attribute__((aligned(2))),
                     tclr[TW * TW * 4] __attribute__((aligned(2)));
static unsigned short tmask[TW * SCWW], timg[TW * TW];
static short tcount[2 * 47 * 47 + 1], tseen[2 * 47 * 47 + 1];

static long read_hz200(void)
{
    return *(volatile long *)0x4ba;
}

static long hz200(void)
{
    return Supexec(read_hz200);
}

/*
 * the icon: 50 % of its pixels nearest the centre opaque, the next 10 %
 * between (alpha falling outwards), the rest clear; a gradient of colour
 */
static void mk_timing_icon(int *n0, int *n255, int *nmid)
{
    int x, y, d, start, rank, a, i;
    unsigned char *p;

    memset(tcount, 0, sizeof(tcount));
    memset(tseen, 0, sizeof(tseen));
    for (y = 0; y < TW; y++)
        for (x = 0; x < TW; x++)
            tcount[(2 * x - 47) * (2 * x - 47) + (2 * y - 47) * (2 * y - 47)]++;
    for (start = 0, d = 0; d <= 2 * 47 * 47; d++)
    {
        int c = tcount[d];

        tcount[d] = start;
        start += c;
    }
    *n0 = *n255 = *nmid = 0;
    memset(tmask, 0, sizeof(tmask));
    for (y = 0; y < TW; y++)
        for (x = 0; x < TW; x++)
        {
            unsigned int r = 40 + x * 4, g = 200 - y * 3, b = 90 + x + y;

            d = (2 * x - 47) * (2 * x - 47) + (2 * y - 47) * (2 * y - 47);
            rank = tcount[d] + tseen[d]++;
            if (rank < TW * TW / 2)
                a = 255;
            else if (rank < TW * TW / 2 + 230)
                a = 254 - (rank - TW * TW / 2) * 253 / 229;
            else
                a = 0;
            if (a == 0)
                (*n0)++;
            else if (a == 255)
                (*n255)++;
            else
                (*nmid)++;
            i = (y * TW + x) * 4;
            p = tmix + i;
            p[0] = (unsigned char)a;
            p[1] = (unsigned char)r;
            p[2] = (unsigned char)g;
            p[3] = (unsigned char)b;
            memcpy(topq + i, p, 4);
            topq[i] = 255;
            memcpy(tclr + i, p, 4);
            tclr[i] = 0;
            /* the mask path: a mask of alpha >= 128, the device image white outside it */
            if (a >= 128)
            {
                tmask[y * SCWW + x / 16] |= 0x8000U >> (x & 15);
                put16((unsigned char *)&timg[y * TW + x], is555 ? ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
                                                               : ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            else
                timg[y * TW + x] = 0xffff;
        }
}

static void mk_tsrc(GCBITMAP *b, unsigned char *pixels)
{
    memset(b, 0, sizeof(*b));
    b->magic = 0x6362746dL;
    b->length = sizeof(*b);
    b->addr = pixels;
    b->width = 4 * TW;
    b->bits = 32;
    b->px_format = ALPHA_PX_ARGB32;
    b->xmax = TW;
    b->ymax = TW;
}

/*
 * case c (0 blend-mixed, 1 mask-path, 2 blend-opaque, 3 blend-clear) in
 * rounds of TCALLS draws until TTICKS ticks of the 200 Hz timer have passed
 * (one round where a draw is slow, more on a fast emulator, where 200 draws
 * take a tick or two); the ticks, and the draws in *calls
 */
static long time_case(int c, long *calls)
{
    static const short sr[4] = { 0, 0, TW - 1, TW - 1 }, dr[4] = { TX, TY, TX + TW - 1, TY + TW - 1 };
    GCBITMAP src;
    MFDB mask, image, scr;
    short xy[8] = { 0, 0, TW - 1, TW - 1, TX, TY, TX + TW - 1, TY + TW - 1 };
    short pens[2] = { 0, 1 };
    long t, t0;
    int n;

    mk_tsrc(&src, c == 2 ? topq : c == 3 ? tclr : tmix);
    memset(&mask, 0, sizeof(mask));
    memset(&scr, 0, sizeof(scr));
    mask.fd_addr = tmask;
    mask.fd_w = TW;
    mask.fd_h = TW;
    mask.fd_wdwidth = SCWW;
    mask.fd_stand = 0;
    mask.fd_nplanes = 1;
    image = mask;
    image.fd_addr = timg;
    image.fd_nplanes = 16;
    t = hz200();
    while ((t0 = hz200()) == t)
        ;
    *calls = 0;
    do
    {
        for (n = 0; n < TCALLS; n++)
        {
            if (c == 1)
            {
                vrt_cpyfm(screen, MD_TRANS, xy, &mask, &scr, pens);
                vro_cpyfm(screen, S_AND_D, xy, &image, &scr);
            }
            else
                transfer(screen, &src, NULL, sr, dr, ALPHA_T_BLEND);
        }
        *calls += TCALLS;
        t = hz200() - t0;
    } while (t < TTICKS && *calls < TMAXCALLS);
    return t;
}

static void test_timing(void)
{
    static const char *const case_name[4] = { "blend-mixed", "mask-path", "blend-opaque", "blend-clear" };
    long ticks[4], calls[4];
    char text[160];
    int c, n0, n255, nmid, x, y, same;

    mk_timing_icon(&n0, &n255, &nmid);
    snprintf(text, sizeof(text), "ALPHA TIME icon %dx%d: %d alpha 0, %d alpha 255, %d between; rounds of %d calls, %d ticks at least",
             TW, TW, n0, n255, nmid, TCALLS, TTICKS);
    say(text);
    set_weight(screen, alpha_weight[0]);
    clip_off(screen);
    for (c = 0; c < 4; c++)
    {
        if (c == 3)
        {
            /* a fully clear icon touches nothing: the screen under it before and after */
            static MFDB mem;
            static unsigned short a1[TW * TW], a2[TW * TW];
            short xy[8] = { TX, TY, TX + TW - 1, TY + TW - 1, 0, 0, TW - 1, TW - 1 };
            MFDB scr;

            memset(&scr, 0, sizeof(scr));
            memset(&mem, 0, sizeof(mem));
            mem.fd_addr = a1;
            mem.fd_w = TW;
            mem.fd_h = TW;
            mem.fd_wdwidth = SCWW;
            mem.fd_nplanes = 16;
            vro_cpyfm(screen, S_ONLY, xy, &scr, &mem);
            ticks[c] = time_case(c, &calls[c]);
            mem.fd_addr = a2;
            vro_cpyfm(screen, S_ONLY, xy, &scr, &mem);
            same = 1;
            for (y = 0; y < TW; y++)
                for (x = 0; x < TW; x++)
                    if (a1[y * TW + x] != a2[y * TW + x])
                        same = 0;
            snprintf(text, sizeof(text), "(h) the fully clear icon, drawn %ld times, leaves the screen as it was", calls[c]);
            report(same, text);
        }
        else
            ticks[c] = time_case(c, &calls[c]);
        snprintf(text, sizeof(text), "ALPHA TIME %s us=%ld ticks=%ld calls=%ld", case_name[c],
                 (ticks[c] * 5000L + calls[c] / 2) / calls[c], ticks[c], calls[c]);
        say(text);
    }
    if (ticks[1] > 0)
    {
        /* a call each: ticks[0] / calls[0] against ticks[1] / calls[1], in hundredths (calls are whole rounds) */
        long r0 = calls[0] / TCALLS, r1 = calls[1] / TCALLS;
        long r = (ticks[0] * r1 * 100 + ticks[1] * r0 / 2) / (ticks[1] * r0);

        snprintf(text, sizeof(text), "ALPHA TIME ratio blend-mixed/mask-path=%ld.%02ld", r / 100, r % 100);
    }
    else
        snprintf(text, sizeof(text), "ALPHA TIME ratio blend-mixed/mask-path=n/a (mask-path under one tick)");
    say(text);
    set_weight(screen, base_weight);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    short work_out[57];
    char text[200];
    unsigned long sum;
    int usable = 0, flags = 0, i;

    say("ALPHA start");
    crc_init();
    sum = vectors_sum();
    snprintf(text, sizeof(text), "the vectors' sum is %08lx (alpha_vectors.h says %08lx)", sum, ALPHA_VECTORS_SUM);
    report(sum == ALPHA_VECTORS_SUM, text);
    if (failed)
        return 1;

    report(appl_init() >= 0, "AES application registration");
    screen = open_vwk();
    report(screen > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(screen, 0, work_out);
    xres = work_out[0];
    yres = work_out[1];
    vq_extnd(screen, 1, work_out);
    planes = work_out[4];
    flags = (unsigned short)work_out[30];
    fmt = get_px_format(screen);
    snprintf(text, sizeof(text), "ALPHA screen %dx%d, %d planes, vq_px_format %08lx, work_out[30] %04x",
             xres + 1, yres + 1, planes, fmt, flags);
    say(text);

    /* (a) */
    report((flags & 3) == 0, "(a) work_out[30] bits 0 and 1 clear: the raster calls are not advertised as complete");
    advertised = (flags & 8) != 0;
    if (planes != 16)
    {
        snprintf(text, sizeof(text), "(a) work_out[30] bit 3 clear on %d planes", planes);
        report(!advertised, text);
    }
    else if (advertised)
    {
        unsigned long base = fmt & ~F_REV;

        swapped = (fmt & F_REV) != 0;
        is555 = base == F_555;
        usable = base == F_565 || base == F_555;
        snprintf(text, sizeof(text), "(a) work_out[30] bit 3 set: the screen is 16 bits of a layout the vectors have (%08lx)",
                 fmt);
        report(usable, text);
    }
    else
        say("ALPHA work_out[30] bit 3 is clear: T_BLEND is not advertised; only the refusals run");

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    clip_off(screen);
    if (advertised && usable)
    {
        test_weight();
        say("ALPHA (d) weight done");
        test_memory();
        say("ALPHA (b) memory done");
        for (i = 0; i < ALPHA_NXFER; i++)
            screen_xfer(i);
        screen_clips();
        say("ALPHA (c) screen done");
        test_copy_modes();
        say("ALPHA (e) modes 0 and 32 done");
        test_refusals();
        say("ALPHA (f) refusals done");
        test_bitmap();
        say("ALPHA (g) bitmap done");
        test_timing();
        say("ALPHA (h) timing done");
    }
    else
    {
        test_refusals();
        say("ALPHA (f) refusals done");
        skipped = !advertised;
    }
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);

out:
    if (screen > 0)
        v_clsvwk(screen);
    appl_exit();
    if (!failed)
    {
        snprintf(text, sizeof(text), "ALPHA %ld checks", checks);
        say(text);
        say(skipped ? "FVDI ALPHA GUEST SKIPPED (not advertised)" : "FVDI ALPHA GUEST PASS");
    }
    return failed;
}
