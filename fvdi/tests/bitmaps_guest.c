/*
 * bitmaps_guest.c - off-screen bitmaps: v_opnbm (EdDI), and NVDI's
 * v_open_bm (100 sub 3) and v_resize_bm (100 sub 2).
 *
 * Draws black bars into white bitmaps and checks the pixels where they
 * should change and where they should not, both in memory the test can
 * see and through v_get_pixel.  Needs a 16-bit (or deeper) screen, where
 * a cleared bitmap is white (0xffff) and black is 0x0000.
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

static int failed;
static short screen;
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

static unsigned short pixel_at(const unsigned char *base, long wrap, short x, short y)
{
    return *(const unsigned short *)(base + (long)y * wrap + (long)x * 2);
}

static short pel_at(short h, short x, short y)
{
    short pel = 0x5555, index;

    v_get_pixel(h, x, y, &pel, &index);
    return pel;
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
    short cw, ch, bw, bh, h, h2, white, black, planes;
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
    check(planes >= 16, "screen is 16 bits or deeper");
    if (failed)
        goto out;
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
    white = pixel_at(mfdb.fd_addr, 128, 10, 10);
    black_bar(h, 8, 8, 12, 12);
    check(white == (short) 0xffff, "v_opnbm bitmap starts white");
    check(pixel_at(mfdb.fd_addr, 128, 10, 10) == 0, "bar drawn in the v_opnbm bitmap");
    check(pixel_at(mfdb.fd_addr, 128, 20, 10) == 0xffff, "and only there");
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
    check(bm.addr && bm.width >= 112 * 2 && bm.xmax == 112 && bm.ymax == 50, "size and line width");
    check(bm.bits == planes && bm.px_format == px_format, "screen depth and format");
    black_bar(h, 50, 20, 59, 29);
    check(pixel_at(bm.addr, bm.width, 55, 25) == 0, "bar drawn in the v_open_bm bitmap");
    check(pixel_at(bm.addr, bm.width, 70, 25) == 0xffff, "and only there");
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
    memset(mem2, 0xff, 512 * 90);
    check(v_resize_bm(h, 200, 90, 512, mem2) == 1, "v_resize_bm into caller memory, 512-byte lines");
    black_bar(h, 150, 85, 159, 89);
    check(pixel_at(mem2, 512, 155, 87) == 0, "bar landed in the caller's memory");
    check(pixel_at(mem2, 512, 165, 87) == 0xffff, "and only there");
    check(v_resize_bm(h, 200, 90, 100, mem2) == 0, "too narrow a line is refused");
    check(pixel_at(mem2, 512, 155, 87) == 0, "and the bitmap is still there");
    black_bar(h, 0, 0, 3, 3);
    check(pixel_at(mem2, 512, 1, 1) == 0, "still drawing into it");
    v_clsbm(h);

    check(v_resize_bm(screen, 100, 100, 0, NULL) == 0, "v_resize_bm refuses a screen workstation");

    /* v_open_bm into the caller's memory, own colour table */
    memset(&bm, 0, sizeof(bm));
    memset(mem1, 0xff, 256 * 60);
    bm.addr = mem1;
    bm.width = 256;
    bm.xmax = 100;
    bm.ymax = 60;
    h = v_open_bm(screen, &bm, 0, 0, 0, 0);
    check(h > 0, "v_open_bm handle (caller memory, own colour table)");
    if (h > 0)
    {
        black_bar(h, 90, 50, 95, 55);
        check(pixel_at(mem1, 256, 92, 52) == 0, "bar landed in the caller's memory");
        check(pixel_at(mem1, 256, 97, 52) == 0xffff, "and only there");
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
        say("FVDI BITMAPS GUEST PASS");
    return failed;
}
