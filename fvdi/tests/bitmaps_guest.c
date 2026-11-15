/*
 * bitmaps_guest.c - off-screen bitmaps: v_opnbm (EdDI), and NVDI's
 * v_open_bm (100 sub 3) and v_resize_bm (100 sub 2).
 *
 * Draws black bars into white bitmaps and checks the pixels where they
 * should change and where they should not, both in memory the test can
 * see and through v_get_pixel.  Works on a 16-bit screen, where white is
 * 0xffff and black 0x0000, and on interleaved planes (1, 2, 4 or 8),
 * where white is pen 0 (every bit clear) and black has every bit set.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI BITMAPS GUEST PASS" or "FVDI BITMAPS FAIL: <check>".
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
short v_resize_bm(short handle, short width, short height, long byte_width,
                  unsigned char *addr);
long vq_px_format(short handle, unsigned long *px_format);
void v_opnbm(short *work_in, MFDB *bitmap, short *handle, short *work_out);
void v_clsbm(short handle);

static int failed, skipped;
static short screen, planes;
static unsigned short white_px, black_px;
static unsigned char white_byte;
static short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
static short work_out[57];

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

    snprintf(line, sizeof(line), "BITMAPS %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI BITMAPS FAIL: %s", what);
        say(line);
    }
}

/* A black bar from (x1,y1) to (x2,y2), inclusive */
static void black_bar(short h, short x1, short y1, short x2, short y2)
{
    short xy[4] = { x1, y1, x2, y2 };

    vswr_mode(h, 1);
    vsf_interior(h, 1);
    vsf_color(h, 1);
    vsf_perimeter(h, 0);
    v_bar(h, xy);
}

/* A pixel as memory holds it: 16 bits, or the bits of the planes as a pen */
static unsigned short pixel_at(const unsigned char *base, long wrap, short x, short y)
{
    const unsigned short *w;
    unsigned short v = 0;
    short k;

    if (planes == 16)
        return *(const unsigned short *)(base + (long)y * wrap + (long)x * 2);
    w = (const unsigned short *)(base + (long)y * wrap) + (x >> 4) * planes;
    for (k = 0; k < planes; k++)
        v |= ((w[k] >> (15 - (x & 15))) & 1) << k;
    return v;
}

/* is there an 'fVDI' cookie (the jar is in supervisor memory)? */
static long jar_has_fvdi(void)
{
    long *jar = *(long **)0x5A0L;

    while (jar && jar[0])
    {
        if ((unsigned long)jar[0] == 0x66564449UL)
            return 1;
        jar += 2;
    }
    return 0;
}

/*
 * The pixel: fVDI answers v_get_pixel with it in intout[0] and -1 in
 * intout[1], EmuTOS's own VDI with 0 in intout[0] and the pixel in
 * intout[1] on a 16-bit screen.
 */
static short pel_at(short h, short x, short y)
{
    static short fvdi = -1;
    short pel = 0x5555, index = 0x5555;

    if (fvdi < 0)
        fvdi = (short)Supexec(jar_has_fvdi);
    v_get_pixel(h, x, y, &pel, &index);
    return (planes == 16 && !fvdi) ? index : pel;
}

/* 16-byte aligned, with 16 spare bytes each side, as NVDI asks */
static unsigned char *aligned(unsigned char *raw)
{
    return (unsigned char *)((((unsigned long)raw) + 16 + 15) & ~15UL);
}

int main(void)
{
    static unsigned char raw1[256 * 60 + 64], raw2[512 * 90 + 64];
    unsigned char *mem1 = aligned(raw1), *mem2 = aligned(raw2);
    char line[200];
    short cw, ch, bw, bh, h, h2, white, black;
    long space;
    unsigned long px_format;
    GCBITMAP bm;
    MFDB mfdb;

    say("BITMAPS start");
    check(appl_init() >= 0, "AES application registration");
    screen = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &screen, work_out);
    check(screen > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(screen, 1, work_out);
    snprintf(line, sizeof(line), "BITMAPS screen planes %d", work_out[4]);
    say(line);
    planes = work_out[4];
    if (planes != 1 && planes != 2 && planes != 4 && planes != 8 && planes != 16)
    {
        /* The pixel checks below read 16-bit pixels or interleaved planes */
        say("BITMAPS screen of another depth: skipped");
        skipped = 1;
        goto out;
    }
    if (planes == 16)
    {
        white_px = 0xffff;
        black_px = 0;
        white_byte = 0xff;
    } else
    {
        white_px = 0;
        black_px = (1 << planes) - 1;
        white_byte = 0;
    }
    space = vq_px_format(screen, &px_format);
    snprintf(line, sizeof(line), "BITMAPS colour space %ld, px_format %08lx", space, px_format);
    say(line);

    /* v_opnbm, the EdDI call the refactor runs under */
    memset(&mfdb, 0, sizeof(mfdb));
    work_in[0] = 1;
    {
        short in[20];
        memset(in, 0, sizeof(in));
        memcpy(in, work_in, sizeof(work_in));
        in[11] = 63;
        in[12] = 31;
        h = graf_handle(&cw, &ch, &bw, &bh);
        v_opnbm(in, &mfdb, &h, work_out);
    }
    check(h > 0, "v_opnbm handle");
    if (failed)
        goto out;
    snprintf(line, sizeof(line), "BITMAPS v_opnbm %dx%d planes %d wd %d at %p",
             mfdb.fd_w, mfdb.fd_h, mfdb.fd_nplanes, mfdb.fd_wdwidth, mfdb.fd_addr);
    say(line);
    check(mfdb.fd_addr && mfdb.fd_w == 64 && mfdb.fd_h == 32 && mfdb.fd_wdwidth == 4,
          "v_opnbm filled in the MFDB");
    white = pixel_at(mfdb.fd_addr, 64L * planes / 8, 10, 10);
    black_bar(h, 8, 8, 12, 12);
    check((unsigned short)white == white_px, "v_opnbm bitmap starts white");
    check(pixel_at(mfdb.fd_addr, 64L * planes / 8, 10, 10) == black_px, "bar drawn in the v_opnbm bitmap");
    check(pixel_at(mfdb.fd_addr, 64L * planes / 8, 20, 10) == white_px, "and only there");
    v_clsbm(h);

    /* v_open_bm, empty GCBITMAP: fVDI allocates, fills it in */
    memset(&bm, 0, sizeof(bm));
    bm.xmax = 100;
    bm.ymax = 50;
    h = v_open_bm(screen, &bm, 1, 0, 0, 0);
    check(h > 0, "v_open_bm handle (allocated, 100x50)");
    if (failed)
        goto out;
    snprintf(line, sizeof(line), "BITMAPS v_open_bm magic %08lx addr %p width %ld bits %ld fmt %08lx %ld,%ld-%ld,%ld",
             bm.magic, bm.addr, bm.width, bm.bits, bm.px_format, bm.xmin, bm.ymin, bm.xmax, bm.ymax);
    say(line);
    check(bm.magic == 0x6362746dL && bm.length == sizeof(GCBITMAP), "GCBITMAP filled in");
    check(bm.addr && bm.width >= 112L * planes / 8 && bm.xmax == 112 && bm.ymax == 50, "size and line width");
    check(bm.bits == planes && bm.px_format == px_format, "screen depth and format");
    black_bar(h, 50, 20, 59, 29);
    check(pixel_at(bm.addr, bm.width, 55, 25) == black_px, "bar drawn in the v_open_bm bitmap");
    check(pixel_at(bm.addr, bm.width, 70, 25) == white_px, "and only there");
    black = pel_at(h, 55, 25);
    white = pel_at(h, 70, 25);
    snprintf(line, sizeof(line), "BITMAPS v_get_pixel black %04x white %04x", black, white);
    say(line);
    check(black != white, "v_get_pixel tells them apart");

    /* v_resize_bm into fVDI's memory: draw beyond the old size */
    check(v_resize_bm(h, 200, 80, 0, NULL) == 1, "v_resize_bm to 200x80");
    black_bar(h, 180, 70, 189, 79);
    check(pel_at(h, 185, 75) == black, "bar drawn beyond the old size");
    check(pel_at(h, 170, 75) == white, "new pixels start white");

    /* v_resize_bm into the caller's memory, with a wide line */
    memset(mem2, white_byte, 512 * 90);
    check(v_resize_bm(h, 200, 90, 512, mem2) == 1, "v_resize_bm into caller memory, 512-byte lines");
    black_bar(h, 150, 85, 159, 89);
    check(pixel_at(mem2, 512, 155, 87) == black_px, "bar landed in the caller's memory");
    check(pixel_at(mem2, 512, 165, 87) == white_px, "and only there");
    check(v_resize_bm(h, 200, 90, 16, mem2) == 0, "too narrow a line is refused");
    check(pixel_at(mem2, 512, 155, 87) == black_px, "and the bitmap is still there");
    black_bar(h, 0, 0, 3, 3);
    check(pixel_at(mem2, 512, 1, 1) == black_px, "still drawing into it");
    v_clsbm(h);

    check(v_resize_bm(screen, 100, 100, 0, NULL) == 0, "v_resize_bm refuses a screen workstation");

    /* v_open_bm into the caller's memory, own colour table */
    memset(&bm, 0, sizeof(bm));
    memset(mem1, white_byte, 256 * 60);
    bm.addr = mem1;
    bm.width = 256;
    bm.xmax = 100;
    bm.ymax = 60;
    h = v_open_bm(screen, &bm, 0, 0, 0, 0);
    check(h > 0, "v_open_bm handle (caller memory, own colour table)");
    if (h > 0)
    {
        black_bar(h, 90, 50, 95, 55);
        check(pixel_at(mem1, 256, 92, 52) == black_px, "bar landed in the caller's memory");
        check(pixel_at(mem1, 256, 97, 52) == white_px, "and only there");
        check(bm.addr == mem1, "caller's GCBITMAP left alone");
        v_clsbm(h);
    }

    /* Refusals */
    memset(&bm, 0, sizeof(bm));
    bm.px_format = 0x12345678UL;
    check(v_open_bm(screen, &bm, 1, 0, 0, 0) == 0, "a foreign pixel format is refused");
    memset(&bm, 0, sizeof(bm));
    check(v_open_bm(screen, &bm, 2, 0, 0, 0) == 0, "undefined colour flags are refused");

    /* No GCBITMAP: the screen's size */
    h = v_open_bm(screen, NULL, 1, -1, 90, 90);
    check(h > 0, "v_open_bm with no GCBITMAP, 90 dpi");
    if (h > 0)
    {
        vq_extnd(h, 0, work_out);
        snprintf(line, sizeof(line), "BITMAPS no-GCBITMAP bitmap %dx%d, pixel %dx%d microns",
                 work_out[0] + 1, work_out[1] + 1, work_out[3], work_out[4]);
        say(line);
        check(work_out[3] == 282 && work_out[4] == 282, "90 dpi became 282 microns");
        v_clsbm(h);
    }

    /*
     * Copies between a bitmap and the screen, word aligned and shifted:
     * the two forms have different line widths, and the copy must land
     * where it is put and nowhere else.
     */
    memset(&bm, 0, sizeof(bm));
    bm.xmax = 64;
    bm.ymax = 32;
    h = v_open_bm(screen, &bm, 1, 0, 0, 0);
    check(h > 0, "v_open_bm handle (64x32, for copies)");
    if (h > 0)
    {
        MFDB bmf, scr;
        short white_box[4] = { 180, 90, 300, 140 }, xy[8], dx;

        memset(&bmf, 0, sizeof(bmf));
        memset(&scr, 0, sizeof(scr));
        bmf.fd_addr = bm.addr;
        bmf.fd_w = (short)bm.xmax;
        bmf.fd_h = (short)bm.ymax;
        bmf.fd_wdwidth = (short)(bm.width * 8 / planes / 16);
        bmf.fd_nplanes = planes;
        black_bar(h, 8, 4, 39, 11);
        graf_mouse(M_OFF, 0);
        wind_update(BEG_UPDATE);
        for (dx = 0; dx < 2; dx++)
        {
            short x0 = dx ? 203 : 208;

            vswr_mode(screen, 1);
            vsf_interior(screen, 1);
            vsf_color(screen, 0);
            vsf_perimeter(screen, 0);
            v_bar(screen, white_box);
            xy[0] = 0;
            xy[1] = 0;
            xy[2] = 63;
            xy[3] = 31;
            xy[4] = x0;
            xy[5] = 100;
            xy[6] = x0 + 63;
            xy[7] = 131;
            vro_cpyfm(screen, S_ONLY, xy, &bmf, &scr);
            check(pel_at(screen, x0 + 8, 104) == black && pel_at(screen, x0 + 39, 111) == black,
                  dx ? "bitmap to screen, shifted: the bar arrives" : "bitmap to screen, aligned: the bar arrives");
            /*
             * beside the copy is the screen's own white (pen 0), which a
             * 16-bit screen of EmuTOS's VDI draws as 0xffdf (it has no
             * spare bit), where the bitmap's memory starts as 0xffff
             */
            check(pel_at(screen, x0 + 7, 104) == white && pel_at(screen, x0 + 40, 111) == white &&
                  pel_at(screen, x0 - 1, 104) == pel_at(screen, 190, 95) &&
                  pel_at(screen, x0 + 64, 104) == pel_at(screen, 190, 95),
                  dx ? "bitmap to screen, shifted: nothing beside it" : "bitmap to screen, aligned: nothing beside it");
        }
        /* and back: the screen's bar into the bitmap, 5 pixels further on */
        memset(bm.addr, white_byte, bm.width * bm.ymax);
        xy[0] = 203;
        xy[1] = 100;
        xy[2] = 203 + 47;
        xy[3] = 131;
        xy[4] = 5;
        xy[5] = 0;
        xy[6] = 5 + 47;
        xy[7] = 31;
        vro_cpyfm(screen, S_ONLY, xy, &scr, &bmf);
        check(pixel_at(bm.addr, bm.width, 13, 4) == black_px && pixel_at(bm.addr, bm.width, 44, 11) == black_px,
              "screen to bitmap, shifted: the bar arrives");
        check(pixel_at(bm.addr, bm.width, 12, 4) == white_px && pixel_at(bm.addr, bm.width, 45, 11) == white_px &&
              pixel_at(bm.addr, bm.width, 4, 4) == white_px,
              "screen to bitmap, shifted: nothing beside it");
        vsf_color(screen, 0);
        v_bar(screen, white_box);
        wind_update(END_UPDATE);
        graf_mouse(M_ON, 0);
        v_clsbm(h);
    }

    /*
     * Clipping off: lines and fills that cross the edges of a bitmap draw
     * inside it only.  The 16 spare bytes each side of the pixels stay as
     * they were, and what the line wrap would put on the other side stays
     * white: a line off the right edge would come back on the left of the
     * next line.  The lines are at x = 61 and 3, which are not multiples
     * of 8, and reach one line above and below.
     */
    memset(&bm, 0, sizeof(bm));
    bm.xmax = 64;
    bm.ymax = 32;
    h = v_open_bm(screen, &bm, 1, 0, 0, 0);
    check(h > 0, "v_open_bm handle (64x32, clipping off)");
    if (h > 0)
    {
        short top[4] = { 61, -1, 61, 5 }, bottom[4] = { 3, 27, 3, 32 };
        short across[4] = { -10, 16, 90, 16 };
        long size = bm.width * bm.ymax;
        int k, same;
        short clipbox[4] = { 0, 0, 0, 0 };

        vs_clip(h, 0, clipbox);
        memset(bm.addr - 16, 0xa5, 16);
        memset(bm.addr + size, 0xa5, 16);
        vsl_color(h, 1);
        vswr_mode(h, 1);
        v_pline(h, 2, top);
        v_pline(h, 2, bottom);
        v_pline(h, 2, across);
        check(pixel_at(bm.addr, bm.width, 61, 2) == black_px &&
              pixel_at(bm.addr, bm.width, 3, 30) == black_px &&
              pixel_at(bm.addr, bm.width, 30, 16) == black_px, "the parts of the lines inside are drawn");
        check(pixel_at(bm.addr, bm.width, 60, 15) == white_px &&
              pixel_at(bm.addr, bm.width, 5, 17) == white_px, "a line off the edge does not wrap to the other side");
        for (k = 0, same = 1; k < 16; k++)
            if (bm.addr[-16 + k] != 0xa5 || bm.addr[size + k] != 0xa5)
                same = 0;
        check(same, "lines leave the spare bytes either side alone");

        black_bar(h, 40, 10, 200, 12);
        check(pixel_at(bm.addr, bm.width, 50, 11) == black_px, "the part of a bar inside is drawn");
        check(pixel_at(bm.addr, bm.width, 5, 11) == white_px &&
              pixel_at(bm.addr, bm.width, 5, 12) == white_px, "and does not wrap to the line's start");
        black_bar(h, -40, -1, 200, 32);
        check(pixel_at(bm.addr, bm.width, 20, 20) == black_px, "a bar over the whole bitmap fills it");
        for (k = 0, same = 1; k < 16; k++)
            if (bm.addr[-16 + k] != 0xa5 || bm.addr[size + k] != 0xa5)
                same = 0;
        check(same, "bars leave the spare bytes either side alone");
        vq_extnd(h, 1, work_out);
        check(work_out[19] == 0, "and vq_extnd still says clipping is off");
        v_clsbm(h);
    }

    /* Many opens and closes: nothing left behind to trip over */
    for (h2 = 0; h2 < 40; h2++)
    {
        memset(&bm, 0, sizeof(bm));
        bm.xmax = 64;
        bm.ymax = 64;
        h = v_open_bm(screen, &bm, h2 & 1, 0, 0, 0);
        if (h <= 0)
            break;
        if (h2 & 2)
            v_resize_bm(h, 128, 32, 0, NULL);
        v_clsbm(h);
    }
    check(h2 == 40, "40 opens, resizes and closes");

out:
    if (screen > 0)
        v_clsvwk(screen);
    appl_exit();
    if (!failed)
        say(skipped ? "FVDI BITMAPS GUEST SKIPPED" : "FVDI BITMAPS GUEST PASS");
    return failed;
}
