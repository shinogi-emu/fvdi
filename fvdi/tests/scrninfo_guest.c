/*
 * scrninfo_guest.c - vq_scrninfo on a 16-bit screen, and what it promises.
 *
 * Programs that convert images themselves (a browser decoding a PNG) build
 * pixels from vq_scrninfo's description, not from the plane count.  On a
 * 16-bit screen this checks the answer word by word against fVDI's
 * 16_bit.sys (packed pixels, software table, 65536 colours as the longword
 * 1:0, five red, six green, five blue bits, no spare bits, organisation 1,
 * red bits 11..15, green 5..10, blue 0..4, every unused table entry -1);
 * then that the description is true: a pixel built from the described bit
 * positions, copied to the screen with vro_cpyfm, is read back unchanged by
 * v_get_pixel and by a copy to memory and decodes to the intended colour, and
 * a pen the VDI itself draws decodes (read with v_get_pixel) to the colour
 * asked for, to within one step of the described bits.
 *
 * Run under the AES.  Over BIOS device 1: "SCRNINFO ok|FAILED: <what>", then
 * "FVDI SCRNINFO GUEST PASS", "FVDI SCRNINFO GUEST SKIPPED" (not a 16-bit
 * screen) or "FVDI SCRNINFO FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

static short contrl[12], intin[16], ptsin[16], intout[300], ptsout[16];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
static int failed;
static short handle;

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

    snprintf(line, sizeof(line), "SCRNINFO %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI SCRNINFO FAIL: %s", what);
        say(line);
    }
}

static void vq_scrn(void)
{
    short i;

    for (i = 0; i < 300; i++)
        intout[i] = 0x5a5a;
    contrl[0] = 102;
    contrl[1] = 0;
    contrl[3] = 1;
    contrl[5] = 1;
    contrl[6] = handle;
    intin[0] = 2;
    __asm__ __volatile__(
        "move.l %0,%%d1\n\t"
        "move.w #115,%%d0\n\t"
        "trap #2"
        :
        : "g"(pblock)
        : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

static short info[128];

/* the pixel for 8-bit components, from the described bit positions */
static unsigned build(unsigned r8, unsigned g8, unsigned b8)
{
    unsigned px = 0, r = r8 >> 3, g = g8 >> 2, b = b8 >> 3;
    short i;

    for (i = 0; i < 5; i++)
        if (info[16 + i] >= 0 && ((r >> i) & 1))
            px |= 1u << info[16 + i];
    for (i = 0; i < 6; i++)
        if (info[32 + i] >= 0 && ((g >> i) & 1))
            px |= 1u << info[32 + i];
    for (i = 0; i < 5; i++)
        if (info[48 + i] >= 0 && ((b >> i) & 1))
            px |= 1u << info[48 + i];
    return px;
}

/* the components (5, 6, 5 bit values) of a pixel, by the described bit positions */
static void decode(unsigned px, unsigned *r, unsigned *g, unsigned *b)
{
    short i;

    *r = *g = *b = 0;
    for (i = 0; i < 5; i++)
        if (info[16 + i] >= 0)
            *r |= ((px >> info[16 + i]) & 1u) << i;
    for (i = 0; i < 6; i++)
        if (info[32 + i] >= 0)
            *g |= ((px >> info[32 + i]) & 1u) << i;
    for (i = 0; i < 5; i++)
        if (info[48 + i] >= 0)
            *b |= ((px >> info[48 + i]) & 1u) << i;
}

static unsigned absdiff(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

int main(void)
{
    static const unsigned char tests[][3] = {
        { 0, 0, 0 }, { 255, 255, 255 }, { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
        { 128, 64, 200 }, { 17, 99, 250 }, { 200, 130, 33 }, { 8, 4, 8 }
    };
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, h, planes, i, bad;
    unsigned short pix[16 * 4], back[16 * 4];
    char line[160];
    MFDB m, screen;
    static const short want[] = { 2, 2, 16, 1, 0 };
    short locked = 0;

    say("SCRNINFO start");
    check(appl_init() >= 0, "AES application registration");
    h = graf_handle(&cw, &ch, &bw, &bh);
    handle = h;
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(handle, 1, work_out);
    planes = work_out[4];
    if (planes != 16)
    {
        snprintf(line, sizeof(line), "SCRNINFO %d planes, nothing to check", planes);
        say(line);
        v_clsvwk(handle);
        appl_exit();
        say("FVDI SCRNINFO GUEST SKIPPED");
        return 0;
    }

    /* the readbacks below: no desktop repaint meanwhile */
    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    locked = 1;
    vq_scrn();
    memcpy(info, intout, sizeof(info));
    snprintf(line, sizeof(line), "SCRNINFO words 0..15: %d %d %d %d %d %d %04x%04x %d %d %d %d %d %d %d %d",
             info[0], info[1], info[2], info[3], info[4], info[5], (unsigned short)info[6], (unsigned short)info[7],
             info[8], info[9], info[10], info[11], info[12], info[13], info[14], info[15]);
    say(line);
    bad = 0;
    for (i = 0; i < 5; i++)
        if (info[i] != want[i])
            bad = 1;
    check(!bad, "packed pixels, software table, 16 bits, 65536 colours (1:0)");
    check(info[8] == 5 && info[9] == 6 && info[10] == 5, "five red, six green, five blue bits");
    check(info[11] == 0 && info[12] == 0 && info[13] == 0, "no alpha, no genlock, no unused bits");
    check(info[14] == 1 && info[15] == 0, "organisation 1, then 0");
    bad = 0;
    for (i = 0; i < 5; i++)
        if (info[16 + i] != 11 + i)
            bad = 1;
    for (i = 5; i < 16; i++)
        if (info[16 + i] != -1)
            bad = 1;
    check(!bad, "red bits 11..15, rest -1");
    bad = 0;
    for (i = 0; i < 6; i++)
        if (info[32 + i] != 5 + i)
            bad = 1;
    for (i = 6; i < 16; i++)
        if (info[32 + i] != -1)
            bad = 1;
    check(!bad, "green bits 5..10, rest -1");
    bad = 0;
    for (i = 0; i < 5; i++)
        if (info[48 + i] != i)
            bad = 1;
    for (i = 5; i < 16; i++)
        if (info[48 + i] != -1)
            bad = 1;
    check(!bad, "blue bits 0..4, rest -1");
    bad = 0;
    for (i = 64; i < 128; i++)
        if (info[i] != -1)
            bad = 1;
    check(!bad, "alpha, genlock and unused tables all -1 (no spare bit)");
    check(info[5] > 0 && info[5] >= (work_out[0] + 1) * 2, "bytes per line covers a row");

    /* a pixel built from the description, written and read back */
    memset(&m, 0, sizeof(m));
    memset(&screen, 0, sizeof(screen));
    m.fd_addr = pix;
    m.fd_w = 16;
    m.fd_h = 4;
    m.fd_wdwidth = 1;
    m.fd_nplanes = 16;
    for (i = 0; i < (short)(sizeof(tests) / sizeof(tests[0])); i++)
    {
        unsigned px = build(tests[i][0], tests[i][1], tests[i][2]);
        unsigned r, g, b, pel, idx2;
        short pelw, idx, k;
        short xy[8];
        MFDB mb;

        for (k = 0; k < 16 * 4; k++)
            pix[k] = (unsigned short)px;
        xy[0] = 0; xy[1] = 0; xy[2] = 15; xy[3] = 3;
        xy[4] = 40; xy[5] = 40; xy[6] = 55; xy[7] = 43;
        vswr_mode(handle, 1);
        vro_cpyfm(handle, S_ONLY, xy, &m, &screen);
        v_get_pixel(handle, 44, 41, &pelw, &idx);
        pel = (unsigned short)idx;   /* a true colour screen: the pixel is the second word */
        mb = m;
        mb.fd_addr = back;
        memset(back, 0, sizeof(back));
        xy[0] = 40; xy[1] = 40; xy[2] = 55; xy[3] = 43;
        xy[4] = 0; xy[5] = 0; xy[6] = 15; xy[7] = 3;
        vro_cpyfm(handle, S_ONLY, xy, &screen, &mb);
        idx2 = back[5];
        decode(px, &r, &g, &b);
        snprintf(line, sizeof(line), "SCRNINFO colour %d,%d,%d: built %04x, v_get_pixel %04x, copied back %04x, decoded %u,%u,%u",
                 tests[i][0], tests[i][1], tests[i][2], px, pel, idx2, r, g, b);
        say(line);
        check(pel == px && idx2 == px, "a built pixel reads back unchanged");
        check(r == (unsigned)(tests[i][0] >> 3) && g == (unsigned)(tests[i][1] >> 2) && b == (unsigned)(tests[i][2] >> 3),
              "a built pixel decodes to the intended colour");
    }

    /* what the VDI draws decodes to the colour asked for */
    {
        short save[3], rgb[3], xy[4], pelw, idx, v;
        static const short req[][3] = {
            { 0, 0, 0 }, { 1000, 1000, 1000 }, { 1000, 0, 0 }, { 0, 1000, 0 }, { 0, 0, 1000 },
            { 500, 500, 500 }, { 123, 456, 789 }, { 800, 100, 300 }, { 66, 33, 990 }
        };

        vq_color(handle, 9, 0, save);
        for (v = 0; v < (short)(sizeof(req) / sizeof(req[0])); v++)
        {
            unsigned r, g, b, ex_r, ex_g, ex_b, px;
            int ok;

            rgb[0] = req[v][0]; rgb[1] = req[v][1]; rgb[2] = req[v][2];
            vs_color(handle, 9, rgb);
            vswr_mode(handle, 1);
            vsf_interior(handle, 1);
            vsf_color(handle, 9);
            vsf_perimeter(handle, 0);
            xy[0] = 70; xy[1] = 70; xy[2] = 80; xy[3] = 80;
            vr_recfl(handle, xy);
            v_get_pixel(handle, 75, 75, &pelw, &idx);
            px = (unsigned short)idx;
            decode(px, &r, &g, &b);
            ex_r = (unsigned)((req[v][0] * 31L + 500) / 1000);
            ex_g = (unsigned)((req[v][1] * 63L + 500) / 1000);
            ex_b = (unsigned)((req[v][2] * 31L + 500) / 1000);
            ok = absdiff(r, ex_r) <= 1 && absdiff(g, ex_g) <= 1 && absdiff(b, ex_b) <= 1;
            snprintf(line, sizeof(line), "SCRNINFO drawn %d,%d,%d: pixel %04x decodes %u,%u,%u, ideal %u,%u,%u (green bit 5 is %u)",
                     req[v][0], req[v][1], req[v][2], px, r, g, b, ex_r, ex_g, ex_b, (px >> 5) & 1);
            say(line);
            check(ok, "a drawn colour decodes to the colour asked for within one step");
        }
        vs_color(handle, 9, save);
    }

out:
    if (locked)
    {
        wind_update(END_UPDATE);
        graf_mouse(M_ON, 0);
    }
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI SCRNINFO GUEST PASS");
    return failed;
}
