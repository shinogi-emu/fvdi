/*
 * outline_guest.c - v_getoutline (243), v_get_outline (243 sub 1) and
 * vst_width (231).
 *
 * The outline of an 'O' in an outline font is checked for its form -
 * contours that start with a jump and close, Bezier segments - and then by
 * filling it with v_bez_fill into an off-screen bitmap: the ring must be
 * filled and the hole must not.  Needs a 16-bit (or deeper) screen for
 * that, and off-screen bitmaps, which the EdDI cookie announces (on a VDI
 * without them v_open_bm would open a screen workstation and write its
 * 45 work_out words into the binding's one); without either, the fill
 * check is skipped.  Needs an outline font.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI OUTLINE GUEST PASS" or "FVDI OUTLINE FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    long magic, length, format, reserved;
    unsigned char *addr;
    long width, bits;
    unsigned long px_format;
    long xmin, ymin, xmax, ymax;
    void *ctab, *itab;
    long reserved0, reserved1;
} GCBITMAP;

void v_getoutline(short handle, short ch, short *xyarray, char *bezarray, short maxverts, short *numverts);
short v_get_outline(short handle, short index, short x_offset, short y_offset,
                    short *pts, char *flags, short max_pts);
void vst_width(short handle, short width, short *char_width, short *char_height,
               short *cell_width, short *cell_height);
void v_bez_fill(short handle, short count, short *xyarr, char *bezarr,
                short *extent, short *totpts, short *totmoves);
short v_open_bm(short base_handle, GCBITMAP *bitmap, short color_flags,
                short unit_flags, short pixel_width, short pixel_height);
void v_clsbm(short handle);

#define MAXPTS 600
#define EDDI_COOKIE 0x45644449UL    /* 'EdDI': the VDI has off-screen bitmaps */

static int failed;
static short xy[MAXPTS * 2], xy2[MAXPTS * 2];
static char bez[MAXPTS], bez2[MAXPTS];

static void say(const char *text)
{
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

/* is there an EdDI cookie?  (the jar is in supervisor memory) */
static long eddi_found;

static long find_eddi(void)
{
    long *jar = *(long **) 0x5A0L;

    eddi_found = 0;
    while (jar && jar[0])
    {
        if ((unsigned long) jar[0] == EDDI_COOKIE)
            eddi_found = 1;
        jar += 2;
    }
    return 0;
}

static void check(int ok, const char *what)
{
    char line[200];

    snprintf(line, sizeof(line), "OUTLINE %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI OUTLINE FAIL: %s", what);
        say(line);
    }
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short h, cw, ch, bw, bh, w2, h2, cw2, ch2, n, n2, i, id, fonts, beziers, contours, closed;
    short start, minx = 32767, maxx = -32768, miny = 32767, maxy = -32768, totpts, totmoves, extent[4];
    char fname[40], line[200];
    GCBITMAP bm;

    say("OUTLINE start");
    check(appl_init() >= 0, "AES application registration");
    h = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &h, work_out);
    check(h > 0, "virtual workstation");
    if (failed)
        goto out;

    /* The system font has no outline */
    vst_font(h, 1);
    v_getoutline(h, 'O', xy, bez, MAXPTS, &n);
    check(n == 0, "no outline from the bitmap system font");

    fonts = vst_load_fonts(h, 0);
    for (id = 0, i = 2; i <= fonts + 1 && !id; i++)
    {
        short fid = vqt_name(h, i, fname);
        if (fid >= 5000)
            id = fid;
    }
    check(id != 0, "an outline font is loaded");
    if (failed)
        goto out;
    snprintf(line, sizeof(line), "OUTLINE font %d '%s'", id, fname);
    say(line);
    vst_font(h, id);
    vst_point(h, 48, &cw, &ch, &bw, &bh);

    /* vst_width: as NVDI, an outline font takes the character width alone;
     * the height stays and the cell narrows with the characters */
    w2 = h2 = cw2 = ch2 = -1;
    vst_width(h, cw / 2, &w2, &h2, &cw2, &ch2);
    snprintf(line, sizeof(line), "OUTLINE vst_width %d -> %d %d %d %d (point gave %d %d %d %d)",
             cw / 2, w2, h2, cw2, ch2, cw, ch, bw, bh);
    say(line);
    check(w2 == cw / 2 && h2 == ch && ch2 == bh && cw2 >= w2 && cw2 < bw,
          "vst_width sets the character width alone");

    /* The form of the outline */
    n = -1;
    v_getoutline(h, 'O', xy, bez, MAXPTS, &n);
    snprintf(line, sizeof(line), "OUTLINE 'O': %d points", n);
    say(line);
    check(n > 8, "'O' has an outline");
    if (failed)
        goto out;
    check((bez[0] & 2) != 0, "the first point is a jump");
    beziers = contours = closed = 0;
    start = 0;
    for (i = 0; i < n; i++)
    {
        if (bez[i] & 1)
            beziers++;
        if (bez[i] & 2)
        {
            if (i > 0 && xy[(i - 1) * 2] == xy[start * 2] && xy[(i - 1) * 2 + 1] == xy[start * 2 + 1])
                closed++;
            contours++;
            start = i;
        }
        if (xy[i * 2] < minx) minx = xy[i * 2];
        if (xy[i * 2] > maxx) maxx = xy[i * 2];
        if (xy[i * 2 + 1] < miny) miny = xy[i * 2 + 1];
        if (xy[i * 2 + 1] > maxy) maxy = xy[i * 2 + 1];
    }
    if (xy[(n - 1) * 2] == xy[start * 2] && xy[(n - 1) * 2 + 1] == xy[start * 2 + 1])
        closed++;
    snprintf(line, sizeof(line), "OUTLINE 'O': %d contours, %d closed, %d Beziers, box %d,%d-%d,%d",
             contours, closed, beziers, minx, miny, maxx, maxy);
    say(line);
    check(contours == 2 && closed == 2, "two contours, both closed");
    check(beziers >= 4, "made of Bezier curves");
    check(minx >= -2 && maxx < 3 * cw && miny >= 0 && maxy <= bh + 2, "inside the character cell, y 0 at its top");

    /* v_get_outline: the same points, placed at the offsets on the baseline */
    n2 = v_get_outline(h, 'O', 100, 200, xy2, bez2, MAXPTS);
    check(n2 == n, "v_get_outline gives the same number of points");
    {
        short top = 0, ok = 1;
        short dist[5], effects[3], minade, maxade, maxw;

        vqt_fontinfo(h, &minade, &maxade, dist, &maxw, effects);
        top = dist[4];
        for (i = 0; i < n && ok; i++)
        {
            if (xy2[i * 2] != xy[i * 2] + 100 || xy2[i * 2 + 1] != xy[i * 2 + 1] - top + 200 || bez2[i] != bez[i])
                ok = 0;
        }
        check(ok, "placed at the offsets, by the baseline");
    }
    v_getoutline(h, 'O', xy2, bez2, 4, &n2);
    check(n2 == 0, "too small a buffer gives nothing, not half an outline");

    /* Fill it: ring black, hole white */
    vq_extnd(h, 1, work_out);
    Supexec(find_eddi);
    if (work_out[4] >= 16 && !eddi_found)
        say("OUTLINE no EdDI cookie, no off-screen bitmaps - fill check skipped");
    else if (work_out[4] >= 16)
    {
        short bmh, x, y, ok_ring, ok_hole;
        unsigned short *row;

        memset(&bm, 0, sizeof(bm));
        bm.xmax = 160;
        bm.ymax = 120;
        bmh = v_open_bm(h, &bm, 1, 0, 0, 0);
        check(bmh > 0, "a bitmap to fill in");
        if (bmh > 0)
        {
            for (i = 0; i < n; i++)
            {
                xy2[i * 2] = xy[i * 2] + 20;
                xy2[i * 2 + 1] = xy[i * 2 + 1] + 20;
            }
            vsf_interior(bmh, 1);
            vsf_color(bmh, 1);
            vsf_perimeter(bmh, 0);
            vswr_mode(bmh, 1);
            v_bez_fill(bmh, n, xy2, bez, extent, &totpts, &totmoves);

            y = 20 + (miny + maxy) / 2;
            row = (unsigned short *)(bm.addr + (long)y * bm.width);
            x = 20 + (minx + maxx) / 2;
            ok_hole = row[x] == 0xffff;
            ok_ring = 0;
            for (x = 20 + minx; x < 20 + (minx + maxx) / 2; x++)
            {
                if (row[x] == 0)
                {
                    ok_ring = 1;
                    break;
                }
            }
            snprintf(line, sizeof(line), "OUTLINE filled: %d points drawn, centre %04x", totpts, row[20 + (minx + maxx) / 2]);
            say(line);
            if (!ok_ring)
            {
                /* Show what was drawn: every 4th row, every 2nd column */
                short yy, xx;
                char dump[100];

                for (yy = 0; yy < 120; yy += 4)
                {
                    unsigned short *r = (unsigned short *)(bm.addr + (long)yy * bm.width);
                    for (xx = 0; xx < 160 && xx / 2 < 98; xx += 2)
                        dump[xx / 2] = r[xx] == 0 ? '#' : r[xx] == 0xffff ? '.' : '?';
                    dump[xx / 2] = 0;
                    snprintf(line, sizeof(line), "OUTLINE row %3d %s", yy, dump);
                    say(line);
                }
            }
            check(ok_ring, "the ring of the 'O' is filled");
            check(ok_hole, "the hole of the 'O' is not");
            v_clsbm(bmh);
        }
    } else
        say("OUTLINE screen under 16 bits - fill check skipped");

out:
    if (h > 0)
        v_clsvwk(h);
    appl_exit();
    if (!failed)
        say("FVDI OUTLINE GUEST PASS");
    return failed;
}
