/*
 * pens_guest.c - text, vrt_cpyfm, fills and lines drawn in a pen must
 * read back, through v_get_pixel, as that pen.
 *
 * Every pen the screen has, up to 16, is drawn four ways into a white
 * box and the pixel colour read back.  On a screen with a colour table
 * the four must agree with the pen asked for.  Above 8 planes there is no
 * colour table to compare against, so the test says SKIPPED.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI PENS GUEST PASS", "FVDI PENS GUEST SKIPPED" or
 * "FVDI PENS FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

static int failed;

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

    snprintf(line, sizeof(line), "PENS %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI PENS FAIL: %s", what);
        say(line);
    }
}

/* The colour index of the first non-white pixel in a box, or -1 */
static short first_ink(short h, short x0, short y0, short x1, short y1)
{
    short x, y, pel, idx;

    for (y = y0; y <= y1; y++)
    {
        for (x = x0; x <= x1; x++)
        {
            v_get_pixel(h, x, y, &pel, &idx);
            if (idx != 0)
                return idx;
        }
    }
    return -1;
}

int main(void)
{
    static unsigned short ones[16];
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57], h, cw, ch, bw, bh, pen, pens, dummy, colours[2];
    short white[4] = { 0, 0, 199, 39 };
    char line[160];
    MFDB src, screen;

    say("PENS start");
    check(appl_init() >= 0, "AES application registration");
    h = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &h, work_out);
    check(h > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(h, 1, work_out);
    snprintf(line, sizeof(line), "PENS screen planes %d", work_out[4]);
    say(line);
    if (work_out[4] > 8)
    {
        say("PENS no colour table above 8 planes: skipped");
        v_clsvwk(h);
        appl_exit();
        say("FVDI PENS GUEST SKIPPED");
        return 0;
    }
    pens = 1 << work_out[4];
    if (pens > 16)
        pens = 16;

    memset(ones, 0xff, sizeof(ones));
    memset(&src, 0, sizeof(src));
    memset(&screen, 0, sizeof(screen));
    src.fd_addr = ones;
    src.fd_w = 16;
    src.fd_h = 16;
    src.fd_wdwidth = 1;
    src.fd_nplanes = 1;

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    vst_font(h, 1);
    vst_point(h, 10, &cw, &ch, &bw, &bh);
    vst_alignment(h, 0, 5, &dummy, &dummy);
    vsl_type(h, 1);
    vsf_interior(h, 1);
    vsf_perimeter(h, 0);

    for (pen = 1; pen < pens; pen++)
    {
        short text_pen, blit_pen, fill_pen, line_pen;
        short bar[4] = { 130, 10, 150, 30 }, ln[4] = { 160, 20, 190, 20 };
        short xy[8] = { 0, 0, 7, 7, 101, 13, 108, 20 };

        vswr_mode(h, 1);
        vsf_color(h, 0);
        vr_recfl(h, white);

        vswr_mode(h, 2);
        vst_color(h, pen);
        v_gtext(h, 8, 4, "MW");
        text_pen = first_ink(h, 8, 4, 40, 24);

        colours[0] = pen;
        colours[1] = 0;
        vrt_cpyfm(h, 1, xy, &src, &screen, colours);
        blit_pen = first_ink(h, 101, 13, 108, 20);

        vswr_mode(h, 1);
        vsf_color(h, pen);
        vr_recfl(h, bar);
        fill_pen = first_ink(h, 135, 15, 145, 25);

        vsl_color(h, pen);
        v_pline(h, 2, ln);
        line_pen = first_ink(h, 165, 20, 185, 20);

        snprintf(line, sizeof(line), "PENS %d: text %d, vrt_cpyfm %d, fill %d, line %d",
                 pen, text_pen, blit_pen, fill_pen, line_pen);
        say(line);
        snprintf(line, sizeof(line), "pen %d drawn four ways reads back as pen %d", pen, pen);
        check(text_pen == pen && blit_pen == pen && fill_pen == pen && line_pen == pen, line);
    }

    vswr_mode(h, 1);
    vsf_color(h, 0);
    vr_recfl(h, white);
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);

out:
    if (h > 0)
        v_clsvwk(h);
    appl_exit();
    if (!failed)
        say("FVDI PENS GUEST PASS");
    return failed;
}
