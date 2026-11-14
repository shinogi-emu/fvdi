/*
 * blitedge_guest.c - edge cases of the drawing loops a speed-up touches,
 * and of the bugs fVDI had in them.
 *
 * Every drawing case works on a small area of the screen: a background of
 * random pixels is put there, one drawing call (or a few) is made, and the
 * area is read back with vro_cpyfm into a memory form in device format.
 * Checks are made within the run, never against another VDI:
 *
 *  - copy-inword:   vro_cpyfm S_ONLY of a rectangle that lies inside one
 *                   destination word, the source left and right of it, on
 *                   its rows and on others: every neighbour is unchanged
 *                   and the rectangle is the source (fVDI fc7a363);
 *  - copy-overlap:  all 16 operations, the source 1-17 pixels left (and
 *                   right) of the destination on the same rows: the same
 *                   as the operation applied from an untouched copy of the
 *                   source (fVDI b98fab5);
 *  - reject-*:      vrt_cpyfm from a 4-plane source, and from the screen
 *                   (fd_addr NULL) when it has more than 1 plane, draws
 *                   nothing (fVDI 40484db, 0e7e049);
 *  - fill-*:        rectangle fills of width 1-40 at x offsets 0-15 in the
 *                   four writing modes, solid and with a pattern, touch
 *                   nothing outside the rectangle and equal the same
 *                   rectangle drawn as one-pixel-wide columns;
 *  - copy-w-*:      rectangle copies of width 1-40 at x offsets 0-15 in
 *                   four operations, on other rows and on the same rows,
 *                   equal to the operation computed in memory;
 *  - text-shift-*:  a string in a system font at x offsets 0-15 in modes
 *                   1-4 is the same picture as the string drawn 48 pixels
 *                   to the right;
 *  - text-clip-*:   a string under a clip rectangle is the unclipped
 *                   string inside the rectangle, the background outside;
 *  - text-edge-*:   a string across each screen edge is the visible part
 *                   of the same string drawn a few words further in;
 *  - linea-rect-*:  Line-A filled rectangle ($A005) in its four writing
 *                   modes touches nothing outside the rectangle;
 *  - linea-copy-*:  Line-A copy raster ($A00E) and bit block transfer
 *                   ($A007, 8 planes or fewer), all 16 operations, equal
 *                   to the operation computed in memory.
 *
 * Besides the checks every drawing case prints "BLITEDGE <case> crc=<hex>",
 * a CRC-32 of everything the case read back, so that two builds of a VDI
 * can be compared line by line (the lines must be equal).  Run under the
 * AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI BLITEDGE GUEST PASS" or "FVDI BLITEDGE FAIL: <check>".
 */

#include <gem.h>
#include <mint/linea.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

static short handle, width, height, planes;
static int failed, nfail;
static unsigned long crc_table[256];
static unsigned long crc_acc;

/* A memory form in device format, a whole number of words wide */
typedef struct
{
    short w, h, wd;
    unsigned short *d;
} Img;

#define IMG_WORDS 16384L        /* 32 KB each: 224 x 48 pixels at 16 bits */
static unsigned short store[6][IMG_WORDS];
#define BG   (&imgs[0])
#define GOT  (&imgs[1])
#define EXP  (&imgs[2])
#define REF  (&imgs[3])
#define UNC  (&imgs[4])
#define OTH  (&imgs[5])
static Img imgs[6];

/*
 * Operation 5 (D' = D, the destination unchanged) is left out of the
 * all-operations cases unless BLITEDGE_OP5 is defined: EmuTOS's
 * fast_bit_blt (vdi_blit.S) has no fragment for it (mode_05 is a dummy
 * label), jumps into the one for operation 6 and exclusive-ors the
 * destination's rightmost fringe pixels with leftovers in d0, so the
 * picture it leaves depends on what the registers held.
 */
#ifdef BLITEDGE_OP5
#define SKIP_OP(mode) 0
#else
#define SKIP_OP(mode) ((mode) == 5)
#endif

static void say(const char *text)
{
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

static void fail(const char *what)
{
    char line[200];

    nfail++;
    snprintf(line, sizeof(line), "BLITEDGE FAILED: %s", what);
    say(line);
    if (!failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI BLITEDGE FAIL: %s", what);
        say(line);
    }
}

/* A failed check says so, with the case, up to 8 times */
static void check(int ok, const char *what, long a, long b, long c)
{
    char line[160];

    if (ok)
        return;
    if (nfail < 8)
    {
        snprintf(line, sizeof(line), "%s (pos %ld got %ld want %ld)", what, a, b, c);
        fail(line);
    } else
        nfail++;
}

static void crc_init(void)
{
    unsigned long c;
    int i, k;

    for (i = 0; i < 256; i++)
    {
        c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static void case_begin(void)
{
    crc_acc = 0xffffffffUL;
}

static void crc_feed(const Img *im)
{
    const unsigned char *p = (const unsigned char *)im->d;
    long n = (long)im->h * im->wd * planes * 2;
    unsigned long c = crc_acc;

    while (n-- > 0)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    crc_acc = c;
}

static void case_end(const char *name)
{
    char line[120];

    snprintf(line, sizeof(line), "BLITEDGE %s crc=%08lx", name, crc_acc ^ 0xffffffffUL);
    say(line);
}

static unsigned long seed = 12345;

static unsigned rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (unsigned)(seed >> 16) & 0xffff;
}

static void img_init(Img *im, short w, short h)
{
    im->w = w;
    im->h = h;
    im->wd = w / 16;
}

static void img_mfdb(const Img *im, MFDB *m)
{
    memset(m, 0, sizeof(*m));
    m->fd_addr = im->d;
    m->fd_w = im->w;
    m->fd_h = im->h;
    m->fd_wdwidth = im->wd;
    m->fd_stand = 0;
    m->fd_nplanes = planes;
}

static unsigned img_get(const Img *im, int x, int y)
{
    if (planes > 8)
        return im->d[(long)y * im->w + x];
    else
    {
        const unsigned short *p = im->d + ((long)y * im->wd + (x >> 4)) * planes;
        unsigned v = 0;
        int i;

        for (i = 0; i < planes; i++)
            v |= (unsigned)((p[i] >> (15 - (x & 15))) & 1) << i;
        return v;
    }
}

static void img_set(Img *im, int x, int y, unsigned v)
{
    if (planes > 8)
        im->d[(long)y * im->w + x] = v;
    else
    {
        unsigned short *p = im->d + ((long)y * im->wd + (x >> 4)) * planes;
        unsigned short bit = 0x8000U >> (x & 15);
        int i;

        for (i = 0; i < planes; i++)
            p[i] = ((v >> i) & 1) ? (p[i] | bit) : (p[i] & ~bit);
    }
}

static void img_random(Img *im)
{
    long n = (long)im->h * im->wd * planes, i;

    for (i = 0; i < n; i++)
        im->d[i] = rnd();
}

/*
 * A background that repeats every 16 pixels across and every 8 rows down,
 * so that a picture drawn on it can be compared with the same picture
 * moved by whole words and by multiples of 8 rows.
 */
static void img_periodic(Img *im)
{
    unsigned tile[8][16];
    int x, y;

    for (y = 0; y < 8; y++)
        for (x = 0; x < 16; x++)
            tile[y][x] = rnd() & ((1UL << planes) - 1);
    for (y = 0; y < im->h; y++)
        for (x = 0; x < im->w; x++)
            img_set(im, x, y, tile[y & 7][x & 15]);
}

static void img_copy(Img *to, const Img *from)
{
    memcpy(to->d, from->d, (long)from->h * from->wd * planes * 2);
}

/* Screen area to memory, and memory to the screen, replace mode */
static void snap(Img *im, short x, short y)
{
    MFDB m, screen;
    short xy[8];

    img_mfdb(im, &m);
    memset(&screen, 0, sizeof(screen));
    xy[0] = x;
    xy[1] = y;
    xy[2] = x + im->w - 1;
    xy[3] = y + im->h - 1;
    xy[4] = 0;
    xy[5] = 0;
    xy[6] = im->w - 1;
    xy[7] = im->h - 1;
    vro_cpyfm(handle, S_ONLY, xy, &screen, &m);
}

static void put(const Img *im, short x, short y)
{
    MFDB m, screen;
    short xy[8];

    img_mfdb(im, &m);
    memset(&screen, 0, sizeof(screen));
    xy[0] = 0;
    xy[1] = 0;
    xy[2] = im->w - 1;
    xy[3] = im->h - 1;
    xy[4] = x;
    xy[5] = y;
    xy[6] = x + im->w - 1;
    xy[7] = y + im->h - 1;
    vro_cpyfm(handle, S_ONLY, xy, &m, &screen);
}

static int same(const Img *a, const Img *b)
{
    return !memcmp(a->d, b->d, (long)a->h * a->wd * planes * 2);
}

/* The first pixel in which two forms differ, as y * 1000 + x, or -1 */
static long first_diff(const Img *a, const Img *b)
{
    int x, y;

    for (y = 0; y < a->h; y++)
        for (x = 0; x < a->w; x++)
            if (img_get(a, x, y) != img_get(b, x, y))
                return (long)y * 1000 + x;
    return -1;
}

/* A failed comparison of two forms: the case, the first pixel and both values */
static void diff_fail(const char *what, const Img *a, const Img *b)
{
    long d = first_diff(a, b);

    if (d >= 0)
        check(0, what, d, img_get(a, d % 1000, d / 1000), img_get(b, d % 1000, d / 1000));
    else
        check(0, what, d, 0, 0);
}

/* A VDI logic operation, bit by bit: mode 3 is S, 6 is S xor D */
static unsigned logop(int mode, unsigned s, unsigned d)
{
    unsigned r = 0;

    if (mode & 8)
        r |= ~s & ~d;
    if (mode & 4)
        r |= ~s & d;
    if (mode & 2)
        r |= s & ~d;
    if (mode & 1)
        r |= s & d;
    return r & ((1UL << planes) - 1);
}

/*
 * exp = pre with the rectangle (dx, dy, w, h) set to mode applied from the
 * rectangle (sx, sy) of pre.  The source is read before anything is
 * written, as an overlapping copy has to be.
 */
static void model_copy(Img *exp, const Img *pre, int sx, int sy, int dx, int dy, int w, int h, int mode)
{
    int x, y;

    img_copy(exp, pre);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            img_set(exp, dx + x, dy + y, logop(mode, img_get(pre, sx + x, sy + y), img_get(pre, dx + x, dy + y)));
}

static void vro(short mode, short sx, short sy, short dx, short dy, short w, short h)
{
    MFDB screen;
    short xy[8];

    memset(&screen, 0, sizeof(screen));
    xy[0] = sx;
    xy[1] = sy;
    xy[2] = sx + w - 1;
    xy[3] = sy + h - 1;
    xy[4] = dx;
    xy[5] = dy;
    xy[6] = dx + w - 1;
    xy[7] = dy + h - 1;
    vro_cpyfm(handle, mode, xy, &screen, &screen);
}

static void full_clip(void)
{
    short clip[4] = { 0, 0, 0, 0 };

    clip[2] = width - 1;
    clip[3] = height - 1;
    vs_clip(handle, 1, clip);
}

/* ---- fc7a363: a copy inside one destination word ---- */
static void copy_inword(void)
{
    char what[80];
    int dx, w, variant;

    case_begin();
    img_init(BG, 160, 16);
    img_init(GOT, 160, 16);
    img_init(EXP, 160, 16);
    img_random(BG);
    for (dx = 0; dx < 16; dx++)
    {
        for (w = 1; dx + w <= 16; w++)
        {
            for (variant = 0; variant < 4; variant++)
            {
                int so = (dx * 3 + w * 5 + variant) & 15;
                int sx = (variant & 1) ? 112 + so : 16 + so;    /* right or left */
                int dxs = 80 + dx;
                int sy = (variant & 2) ? 9 : 4;                 /* other rows or same */
                int dy = 4;

                put(BG, 0, 0);
                vro(S_ONLY, sx, sy, dxs, dy, w, 4);
                snap(GOT, 0, 0);
                model_copy(EXP, BG, sx, sy, dxs, dy, w, 4, S_ONLY);
                if (!same(GOT, EXP))
                {
                    snprintf(what, sizeof(what), "copy-inword dx %d w %d variant %d at", dx, w, variant);
                    diff_fail(what, GOT, EXP);
                }
                crc_feed(GOT);
            }
        }
    }
    case_end("copy-inword");
}

/* ---- b98fab5: overlapping copies, every operation ---- */
static void copy_overlap(int right)
{
    char what[80], name[40];
    int mode, k;

    case_begin();
    img_init(BG, 160, 8);
    img_init(GOT, 160, 8);
    img_init(EXP, 160, 8);
    img_random(BG);
    for (mode = 0; mode < 16; mode++)
    {
        if (SKIP_OP(mode))
            continue;
        for (k = 1; k <= 17; k++)
        {
            int sx = right ? 40 + k : 40;           /* source left of the destination */
            int dxs = right ? 40 : 40 + k;          /* (or right of it) */

            put(BG, 0, 0);
            vro(mode, sx, 2, dxs, 2, 59, 3);
            snap(GOT, 0, 0);
            model_copy(EXP, BG, sx, 2, dxs, 2, 59, 3, mode);
            if (!same(GOT, EXP))
            {
                snprintf(what, sizeof(what), "copy-overlap mode %d k %d %s at", mode, k, right ? "right" : "left");
                diff_fail(what, GOT, EXP);
            }
            crc_feed(GOT);
        }
    }
    snprintf(name, sizeof(name), "copy-overlap-%s", right ? "right" : "left");
    case_end(name);
}

/* ---- 40484db, 0e7e049: vrt_cpyfm sources that must draw nothing ---- */
static void reject(int null_source)
{
    static unsigned short form[16 * 4];
    MFDB src, screen;
    short xy[8], colours[2] = { 1, 0 }, mode;
    char what[80];

    case_begin();
    img_init(BG, 96, 16);
    img_init(GOT, 96, 16);
    img_random(BG);
    memset(&screen, 0, sizeof(screen));
    memset(form, 0xa5, sizeof(form));
    memset(&src, 0, sizeof(src));
    src.fd_addr = null_source ? 0 : form;
    src.fd_w = 16;
    src.fd_h = 16;
    src.fd_wdwidth = 1;
    src.fd_nplanes = null_source ? planes : 4;
    for (mode = 1; mode <= 4; mode++)
    {
        put(BG, 0, 0);
        xy[0] = 0;
        xy[1] = 0;
        xy[2] = 15;
        xy[3] = 7;
        xy[4] = 20 + mode;
        xy[5] = 4;
        xy[6] = 35 + mode;
        xy[7] = 11;
        vrt_cpyfm(handle, mode, xy, &src, &screen, colours);
        snap(GOT, 0, 0);
        snprintf(what, sizeof(what), "reject-%s drew something, mode %d at", null_source ? "screen-source" : "4-plane", mode);
        check(same(GOT, BG), what, first_diff(GOT, BG), 0, 0);
        crc_feed(GOT);
    }
    case_end(null_source ? "reject-screen-source" : "reject-4-plane");
}

/* ---- fills ---- */
static void fill_cases(short interior, short style, short mode, const char *name)
{
    static short udpat[16] = { 0x8f1e, 0x4a2d, 0x2b3c, 0x1d4b, 0x0f5a, 0x8769, 0xc378, 0xe187,
                               0xf00f, 0x78f0, 0x3cc3, 0x1e3c, 0x0f78, 0x87e1, 0xc3d2, 0xe1b4 };
    short box[4], xo, w, c;
    int x, y;
    char what[80];

    case_begin();
    img_init(BG, 96, 8);
    img_init(GOT, 96, 8);
    img_init(EXP, 96, 8);
    img_init(REF, 96, 8);
    img_random(BG);
    vswr_mode(handle, mode);
    vsf_interior(handle, interior);
    if (interior == 4)
        vsf_udpat(handle, udpat, 1);
    else
        vsf_style(handle, style);
    vsf_perimeter(handle, 0);
    vsf_color(handle, planes > 1 ? 5 : 1);
    for (xo = 0; xo < 16; xo++)
    {
        for (w = 1; w <= 40; w++)
        {
            int x0 = 16 + xo, y0 = 2, h = 4;

            put(BG, 0, 0);
            box[0] = x0;
            box[1] = y0;
            box[2] = x0 + w - 1;
            box[3] = y0 + h - 1;
            vr_recfl(handle, box);
            snap(GOT, 0, 0);
            /* nothing outside the rectangle changed */
            img_copy(EXP, BG);
            for (y = y0; y < y0 + h; y++)
                for (x = x0; x < x0 + w; x++)
                    img_set(EXP, x, y, img_get(GOT, x, y));
            if (!same(GOT, EXP))
            {
                snprintf(what, sizeof(what), "%s xo %d w %d wrote outside the rectangle at", name, xo, w);
                diff_fail(what, GOT, EXP);
            }
            /* the same as the rectangle in one-pixel columns */
            if (w <= 18)
            {
                put(BG, 0, 0);
                for (c = 0; c < w; c++)
                {
                    box[0] = box[2] = x0 + c;
                    vr_recfl(handle, box);
                }
                snap(REF, 0, 0);
                if (!same(GOT, REF))
                {
                    snprintf(what, sizeof(what), "%s xo %d w %d differs from its columns at", name, xo, w);
                    diff_fail(what, GOT, REF);
                }
            }
            crc_feed(GOT);
        }
    }
    case_end(name);
}

/* ---- copies of width 1-40 at every x offset ---- */
static void copy_widths(short mode, int same_rows, const char *name)
{
    short xo, w;
    char what[80];

    case_begin();
    img_init(BG, 160, 16);
    img_init(GOT, 160, 16);
    img_init(EXP, 160, 16);
    img_random(BG);
    for (xo = 0; xo < 16; xo++)
    {
        for (w = 1; w <= 40; w++)
        {
            int dx = 16 + xo;
            int sx = 96 + ((xo * 5 + 3) & 15);
            int sy = same_rows ? 4 : 10;
            int dy = 4;

            put(BG, 0, 0);
            vro(mode, sx, sy, dx, dy, w, 4);
            snap(GOT, 0, 0);
            model_copy(EXP, BG, sx, sy, dx, dy, w, 4, mode);
            if (!same(GOT, EXP))
            {
                snprintf(what, sizeof(what), "%s xo %d w %d at", name, xo, w);
                diff_fail(what, GOT, EXP);
            }
            crc_feed(GOT);
        }
    }
    case_end(name);
}

/* ---- text ---- */
#define TEXT "Edge:gjQ_[01] xy,"
#define TW 224
#define TH 48

static void set_font(short point)
{
    short d;

    vst_font(handle, 1);
    vst_point(handle, point, &d, &d, &d, &d);
    vst_alignment(handle, 0, 5, &d, &d);
    vst_effects(handle, 0);
    vst_rotation(handle, 0);
}

static void text_background(void)
{
    img_init(BG, TW, TH);
    img_init(GOT, TW, TH);
    img_init(EXP, TW, TH);
    img_init(UNC, TW, TH);
    img_init(OTH, TW, TH);
    img_periodic(BG);
}

/* The string drawn at xo and 48 pixels further right is the same picture */
static void text_shift(short point, short mode, const char *name)
{
    short xo;
    char what[80];

    case_begin();
    text_background();
    set_font(point);
    vswr_mode(handle, mode);
    vst_color(handle, planes > 1 ? 5 : 1);
    for (xo = 0; xo < 16; xo++)
    {
        long d;
        int x, y;

        put(BG, 0, 0);
        v_gtext(handle, 16 + xo, 8, TEXT);
        snap(GOT, 0, 0);
        put(BG, 0, 0);
        v_gtext(handle, 16 + xo + 48, 8, TEXT);
        snap(OTH, 0, 0);
        d = -1;
        for (y = 0; y < TH && d < 0; y++)
            for (x = 0; x < TW - 48; x++)
                if (img_get(GOT, x, y) != img_get(OTH, x + 48, y))
                {
                    d = (long)y * 1000 + x;
                    break;
                }
        snprintf(what, sizeof(what), "%s xo %d moved by 48 at", name, xo);
        check(d < 0, what, d, 0, 0);
        crc_feed(GOT);
    }
    case_end(name);
}

/* Under a clip rectangle: the unclipped string inside it, the background outside */
static void text_clip(short point, short mode, const char *name)
{
    short xo, variant;
    char what[80];

    case_begin();
    text_background();
    set_font(point);
    vswr_mode(handle, mode);
    vst_color(handle, planes > 1 ? 5 : 1);
    for (xo = 0; xo < 16; xo += 3)
    {
        for (variant = 0; variant < 5; variant++)
        {
            short clip[4];
            int x, y;
            long d = -1;

            switch (variant)
            {
            case 0:     /* cuts through both ends of the string */
                clip[0] = 16 + xo + 13;
                clip[1] = 0;
                clip[2] = 16 + xo + 90;
                clip[3] = TH - 1;
                break;
            case 1:     /* cuts the rows */
                clip[0] = 0;
                clip[1] = 8 + 3;
                clip[2] = TW - 1;
                clip[3] = 8 + 9;
                break;
            case 2:     /* one pixel wide */
                clip[0] = clip[2] = 16 + xo + 37;
                clip[1] = 0;
                clip[3] = TH - 1;
                break;
            case 3:     /* inside one word */
                clip[0] = 32 + 3;
                clip[1] = 0;
                clip[2] = 32 + 11;
                clip[3] = TH - 1;
                break;
            default:    /* a word boundary on each side */
                clip[0] = 48;
                clip[1] = 10;
                clip[2] = 111;
                clip[3] = 17;
                break;
            }
            put(BG, 0, 0);
            v_gtext(handle, 16 + xo, 8, TEXT);
            snap(UNC, 0, 0);
            put(BG, 0, 0);
            vs_clip(handle, 1, clip);
            v_gtext(handle, 16 + xo, 8, TEXT);
            full_clip();
            snap(GOT, 0, 0);
            for (y = 0; y < TH && d < 0; y++)
                for (x = 0; x < TW; x++)
                {
                    int in = x >= clip[0] && x <= clip[2] && y >= clip[1] && y <= clip[3];
                    unsigned want = in ? img_get(UNC, x, y) : img_get(BG, x, y);

                    if (img_get(GOT, x, y) != want)
                    {
                        d = (long)y * 1000 + x;
                        break;
                    }
                }
            snprintf(what, sizeof(what), "%s xo %d clip %d at", name, xo, variant);
            check(d < 0, what, d, 0, 0);
            crc_feed(GOT);
        }
    }
    case_end(name);
}

/*
 * A string over a screen edge: its visible part is the same as the visible
 * part of the string drawn 48 pixels (24 rows) further in, which shows more.
 */
static void text_edge(short point, short mode, int edge, const char *name)
{
    short k;
    char what[80];

    case_begin();
    text_background();
    set_font(point);
    vswr_mode(handle, mode);
    vst_color(handle, planes > 1 ? 5 : 1);
    full_clip();
    for (k = 0; k < 40; k += (k < 20 ? 1 : 5))
    {
        short x0 = 0, y0 = 0, tx, ty, ox, oy;
        long d = -1;
        int x, y;

        switch (edge)
        {
        case 0:     /* left: string starts k pixels off the screen */
            x0 = 0;
            y0 = 40;
            tx = -k;
            ty = y0 + 8;
            ox = tx + 48;
            oy = ty;
            break;
        case 1:     /* right: string ends k pixels past the screen edge */
            x0 = width - TW;
            y0 = 40;
            tx = width - 8 * 17 + k;
            ty = y0 + 8;
            ox = tx - 48;
            oy = ty;
            break;
        case 2:     /* top */
            x0 = 16;
            y0 = 0;
            tx = x0 + 16;
            ty = -(k % 16);
            ox = tx;
            oy = ty + 24;
            break;
        default:    /* bottom */
            x0 = 16;
            y0 = height - TH;
            tx = x0 + 16;
            ty = height - 16 + (k % 16);
            ox = tx;
            oy = ty - 24;
            break;
        }
        put(BG, x0, y0);
        v_gtext(handle, tx, ty, TEXT);
        snap(GOT, x0, y0);
        put(BG, x0, y0);
        v_gtext(handle, ox, oy, TEXT);
        snap(OTH, x0, y0);
        for (y = 0; y < TH && d < 0; y++)
            for (x = 0; x < TW; x++)
            {
                int ox2 = x, oy2 = y;
                unsigned a, b;

                switch (edge)
                {
                case 0:
                    ox2 = x + 48;
                    break;
                case 1:
                    ox2 = x - 48;
                    break;
                case 2:
                    oy2 = y + 24;
                    break;
                default:
                    oy2 = y - 24;
                    break;
                }
                if (ox2 < 0 || ox2 >= TW || oy2 < 0 || oy2 >= TH)
                    continue;
                a = img_get(GOT, x, y);
                b = img_get(OTH, ox2, oy2);
                if (a != b)
                {
                    d = (long)y * 1000 + x;
                    break;
                }
            }
        snprintf(what, sizeof(what), "%s k %d at", name, k);
        check(d < 0, what, d, 0, 0);
        crc_feed(GOT);
    }
    case_end(name);
}

/* ---- Line-A ---- */
static void linea_rect_case(short wmode, const char *name)
{
    static short solid[16];
    short xo, w, i;
    char what[80];
    int x, y;

    case_begin();
    img_init(BG, 96, 8);
    img_init(GOT, 96, 8);
    img_init(EXP, 96, 8);
    img_random(BG);
    for (i = 0; i < 16; i++)
        solid[i] = (short)0xffff;
    linea0();
    for (xo = 0; xo < 16; xo++)
    {
        for (w = 1; w <= 40; w += (w < 20 ? 1 : 5))
        {
            int x0 = 16 + xo, y0 = 2, h = 4;

            put(BG, 0, 0);
            __aline->_WMODE = wmode;
            __aline->_COLBIT0 = 1;
            __aline->_COLBIT1 = planes > 1 ? 0 : 1;
            __aline->_COLBIT2 = planes > 2 ? 1 : 0;
            __aline->_COLBIT3 = 0;
            __aline->_X1 = x0;
            __aline->_Y1 = y0;
            __aline->_X2 = x0 + w - 1;
            __aline->_Y2 = y0 + h - 1;
            __aline->_PATPTR = solid;
            __aline->_PATMSK = 15;
            __aline->_MFILL = 0;
            __aline->_CLIP = 0;
            linea5();
            snap(GOT, 0, 0);
            img_copy(EXP, BG);
            for (y = y0; y < y0 + h; y++)
                for (x = x0; x < x0 + w; x++)
                    img_set(EXP, x, y, img_get(GOT, x, y));
            if (!same(GOT, EXP))
            {
                snprintf(what, sizeof(what), "%s xo %d w %d wrote outside the rectangle at", name, xo, w);
                diff_fail(what, GOT, EXP);
            }
            crc_feed(GOT);
        }
    }
    case_end(name);
}

/* $A00E copy raster form, opaque, screen to screen */
static void linea_copy_case(int bitblt)
{
    short mode, xo;
    char what[80], name[40];
    MFDB screen;

    case_begin();
    img_init(BG, 160, 8);
    img_init(GOT, 160, 8);
    img_init(EXP, 160, 8);
    img_random(BG);
    memset(&screen, 0, sizeof(screen));
    linea0();
    for (mode = 0; mode < 16; mode++)
    {
        if (SKIP_OP(mode))
            continue;
        for (xo = 0; xo < 16; xo += (bitblt ? 5 : 3))
        {
            int w = 19 + xo * 2, sx = 96 + ((xo * 7 + 1) & 15), dx = 8 + xo, sy = 4, dy = 1;

            put(BG, 0, 0);
            if (bitblt)
            {
                BBPB b;
                long bytes = (long)((width * planes) / 8);

                memset(&b, 0, sizeof(b));
                b.bb_b_wd = w;
                b.bb_b_ht = 3;
                b.bb_plane_ct = planes;
                b.bb_fg_col = 0;
                b.bb_bg_col = 0;
                b.bb_op_tab.fg0bg0 = mode;
                b.bb_op_tab.fg0bg1 = mode;
                b.bb_op_tab.fg1bg0 = mode;
                b.bb_op_tab.fg1bg1 = mode;
                b.bb_s.bl_xmin = sx;
                b.bb_s.bl_ymin = sy;
                b.bb_s.bl_form = Logbase();
                b.bb_s.bl_nxwd = planes * 2;
                b.bb_s.bl_nxln = bytes;
                b.bb_s.bl_nxpl = 2;
                b.bb_d = b.bb_s;
                b.bb_d.bl_xmin = dx;
                b.bb_d.bl_ymin = dy;
                b.bb_p_addr = 0;
                linea7(&b);
            } else
            {
                short *pts, *intin, *contrl;

                __aline->_COPYTRAN = 0;
                __aline->_MFILL = 0;
                __aline->_CLIP = 0;
                contrl = __aline->_CONTRL;
                intin = __aline->_INTIN;
                pts = __aline->_PTSIN;
                *(MFDB **)&contrl[7] = &screen;
                *(MFDB **)&contrl[9] = &screen;
                intin[0] = mode;
                pts[0] = sx;
                pts[1] = sy;
                pts[2] = sx + w - 1;
                pts[3] = sy + 2;
                pts[4] = dx;
                pts[5] = dy;
                pts[6] = dx + w - 1;
                pts[7] = dy + 2;
                lineae();
            }
            snap(GOT, 0, 0);
            model_copy(EXP, BG, sx, sy, dx, dy, w, 3, mode);
            if (!same(GOT, EXP))
            {
                snprintf(what, sizeof(what), "linea-%s mode %d xo %d at", bitblt ? "bitblt" : "copy", mode, xo);
                diff_fail(what, GOT, EXP);
            }
            crc_feed(GOT);
        }
    }
    snprintf(name, sizeof(name), "%s", bitblt ? "linea-bitblt" : "linea-copy-raster");
    case_end(name);
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57], cw, ch, bw, bh, i, mode;
    static const short points[2] = { 9, 10 };
    char line[120], name[48];
    int p, e;

    appl_init();
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    if (handle <= 0)
    {
        say("FVDI BLITEDGE FAIL: virtual workstation");
        return 1;
    }
    width = work_out[0] + 1;
    height = work_out[1] + 1;
    vq_extnd(handle, 1, work_out);
    planes = work_out[4];
    crc_init();
    for (i = 0; i < 6; i++)
        imgs[i].d = store[i];
    full_clip();
    snprintf(line, sizeof(line), "BLITEDGE screen %dx%dx%d", width, height, planes);
    say(line);
    if (SKIP_OP(5))
        say("BLITEDGE note: operation 5 left out of the all-operations cases");

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);

    copy_inword();
    copy_overlap(0);
    copy_overlap(1);
    reject(0);
    if (planes > 1)
        reject(1);
    else
        say("BLITEDGE reject-screen-source skipped: one plane");

    fill_cases(1, 0, 1, "fill-solid-replace");
    fill_cases(1, 0, 2, "fill-solid-transparent");
    fill_cases(1, 0, 3, "fill-solid-xor");
    fill_cases(1, 0, 4, "fill-solid-revtransparent");
    fill_cases(4, 0, 1, "fill-pattern-replace");
    fill_cases(4, 0, 2, "fill-pattern-transparent");
    fill_cases(4, 0, 3, "fill-pattern-xor");
    fill_cases(4, 0, 4, "fill-pattern-revtransparent");

    copy_widths(S_ONLY, 0, "copy-w-s-rows");
    copy_widths(S_ONLY, 1, "copy-w-s-same");
    copy_widths(S_XOR_D, 0, "copy-w-xor-rows");
    copy_widths(S_OR_D, 1, "copy-w-or-same");
    copy_widths(S_AND_D, 0, "copy-w-and-rows");

    for (p = 0; p < 2; p++)
    {
        for (mode = 1; mode <= 4; mode++)
        {
            snprintf(name, sizeof(name), "text-shift-p%d-m%d", points[p], mode);
            text_shift(points[p], mode, name);
            snprintf(name, sizeof(name), "text-clip-p%d-m%d", points[p], mode);
            text_clip(points[p], mode, name);
            for (e = 0; e < 4; e++)
            {
                static const char *const edges[4] = { "left", "right", "top", "bottom" };

                snprintf(name, sizeof(name), "text-edge-%s-p%d-m%d", edges[e], points[p], mode);
                text_edge(points[p], mode, e, name);
            }
        }
    }

    linea_rect_case(0, "linea-rect-replace");
    linea_rect_case(1, "linea-rect-transparent");
    linea_rect_case(2, "linea-rect-xor");
    linea_rect_case(3, "linea-rect-revtransparent");
    linea_copy_case(0);
    if (planes <= 8)
        linea_copy_case(1);
    else
        say("BLITEDGE linea-bitblt skipped: more than 8 planes");

    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);
    v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI BLITEDGE GUEST PASS");
    return failed;
}
