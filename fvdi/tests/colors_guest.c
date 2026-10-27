/*
 * colors_guest.c - NVDI 5's highlight, minimum, maximum and weight colours
 * (vs_/vq_ 207/209, sub-functions 0-3) and the raster foreground and
 * background colours (vs_/vq_ fg/bg 200-203, sub-function 4).
 *
 * The calls are made through the trap directly: gemlib's vq_hilite_color
 * and friends never copy the colour out of intout, so a test written on
 * them would check nothing.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI COLORS GUEST PASS" or "FVDI COLORS FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>

static short contrl[12], intin[16], ptsin[16], intout[16], ptsout[16];
static short *pblock[5] = { contrl, intin, ptsin, intout, ptsout };
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

    snprintf(line, sizeof(line), "COLORS %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI COLORS FAIL: %s", what);
        say(line);
    }
}

static void vdi_raw(short handle, short opcode, short sub, short nintin)
{
    contrl[0] = opcode;
    contrl[1] = 0;
    contrl[3] = nintin;
    contrl[5] = sub;
    contrl[6] = handle;
    __asm__ __volatile__(
        "move.l %0,%%d1\n\t"
        "move.w #115,%%d0\n\t"
        "trap #2"
        :
        : "g"(pblock)
        : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

/* vs_*_color style: colour space 'space', then COLOR_ENTRY reserved,r,g,b */
static short set_colour(short handle, short opcode, short sub, long space,
                        unsigned short r, unsigned short g, unsigned short b)
{
    intin[0] = (short) (space >> 16);
    intin[1] = (short) space;
    intin[2] = 0;
    intin[3] = (short) r;
    intin[4] = (short) g;
    intin[5] = (short) b;
    intout[0] = 0x7777;
    vdi_raw(handle, opcode, sub, 6);
    return intout[0];
}

/* vq_*_color style: returns the colour space, fills r,g,b */
static long get_colour(short handle, short opcode, short sub,
                       unsigned short *r, unsigned short *g, unsigned short *b)
{
    intout[0] = intout[1] = 0x7777;
    intout[3] = intout[4] = intout[5] = 0x7777;
    vdi_raw(handle, opcode, sub, 0);
    *r = (unsigned short) intout[3];
    *g = (unsigned short) intout[4];
    *b = (unsigned short) intout[5];
    return ((long) intout[0] << 16) | (unsigned short) intout[1];
}

/* v_color2nearest (204/2): the colour the screen really has nearest r,g,b */
static void nearest(short handle, unsigned short r, unsigned short g, unsigned short b,
                    unsigned short *nr, unsigned short *ng, unsigned short *nb)
{
    intin[0] = 0;
    intin[1] = 1;
    intin[2] = 0;
    intin[3] = (short) r;
    intin[4] = (short) g;
    intin[5] = (short) b;
    intout[3] = intout[4] = intout[5] = 0x7777;
    vdi_raw(handle, 204, 2, 6);
    *nr = (unsigned short) intout[3];
    *ng = (unsigned short) intout[4];
    *nb = (unsigned short) intout[5];
}

static short open_vwk(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, handle;

    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    return handle;
}

static int is_rgb(short handle, short opcode, short sub,
                  unsigned short r, unsigned short g, unsigned short b)
{
    unsigned short qr, qg, qb;
    long space = get_colour(handle, opcode, sub, &qr, &qg, &qb);
    char line[120];

    snprintf(line, sizeof(line), "COLORS %d/%d -> space %ld rgb %04x %04x %04x",
             opcode, sub, space, qr, qg, qb);
    say(line);
    return space == 1 && qr == r && qg == g && qb == b;
}

int main(void)
{
    static const char *const name[4] = { "hilite", "min", "max", "weight" };
    char what[80];
    short h, h2, sub;
    unsigned short r1, g1, b1, r2, g2, b2;
    short work_out[57], palette_screen;

    say("COLORS start");
    check(appl_init() >= 0, "AES application registration");
    h = open_vwk();
    check(h > 0, "virtual workstation");
    if (failed)
        goto out;

    /* Defaults before anything is set */
    check(is_rgb(h, 209, 0, 0, 0, 0), "hilite defaults to black");
    check(is_rgb(h, 209, 1, 0, 0, 0), "min defaults to black");
    check(is_rgb(h, 209, 2, 0xffff, 0xffff, 0xffff), "max defaults to white");
    check(is_rgb(h, 209, 3, 0x8000, 0x8000, 0x8000), "weight defaults to half");

    /* What is set is what is reported, per sub-function */
    for (sub = 0; sub < 4; sub++)
    {
        unsigned short r = 0x1111 * (sub + 1), g = 0x2222 + sub, b = 0xfedc - sub;

        snprintf(what, sizeof(what), "vs_%s_color accepted", name[sub]);
        check(set_colour(h, 207, sub, 1, r, g, b) == 1, what);
        snprintf(what, sizeof(what), "vq_%s_color returns it", name[sub]);
        check(is_rgb(h, 209, sub, r, g, b), what);
    }
    check(set_colour(h, 207, 0, 0, 0xaaaa, 0xbbbb, 0xcccc) == 1, "colour space 0 (current) accepted");
    check(is_rgb(h, 209, 0, 0xaaaa, 0xbbbb, 0xcccc), "and stored");

    /* Refusals */
    check(set_colour(h, 207, 4, 1, 1, 2, 3) == -1, "set with a wrong sub-function is -1");
    check(get_colour(h, 209, 4, &r1, &g1, &b1) == -1, "query with a wrong sub-function is -1");
    check(set_colour(h, 207, 0, 2, 1, 2, 3) == 0, "unsupported colour space is 0");
    check(is_rgb(h, 209, 0, 0xaaaa, 0xbbbb, 0xcccc), "and changes nothing");

    /* Each workstation has its own */
    h2 = open_vwk();
    check(h2 > 0 && h2 != h, "second virtual workstation");
    check(is_rgb(h2, 209, 0, 0, 0, 0), "second workstation starts at the default");
    v_clsvwk(h2);

    /* Raster foreground/background: same path as text/fill/line/marker */
    check(get_colour(h, 202, 4, &r1, &g1, &b1) == 1, "vqr_fg_color answers");
    check(set_colour(h, 200, 4, 1, 0xffff, 0, 0) == 1, "vsr_fg_color accepted");
    check(set_colour(h, 201, 4, 1, 0, 0xffff, 0) == 1, "vsr_bg_color accepted");
    /* Compare with the fill colours, which go through the same code */
    check(set_colour(h, 200, 1, 1, 0xffff, 0, 0) == 1, "vsf_fg_color accepted");
    check(set_colour(h, 201, 1, 1, 0, 0xffff, 0) == 1, "vsf_bg_color accepted");
    get_colour(h, 202, 1, &r1, &g1, &b1);
    check(get_colour(h, 202, 4, &r2, &g2, &b2) == 1 && r2 == r1 && g2 == g1 && b2 == b1,
          "vqr_fg_color reports what vqf_fg_color does for the same colour");
    snprintf(what, sizeof(what), "COLORS fill fg reads %04x %04x %04x", r1, g1, b1);
    say(what);
    get_colour(h, 203, 1, &r1, &g1, &b1);
    check(get_colour(h, 203, 4, &r2, &g2, &b2) == 1 && r2 == r1 && g2 == g1 && b2 == b1,
          "vqr_bg_color reports what vqf_bg_color does for the same colour");
    check(g1 != 0x7777, "the fill background query filled the colour in");

    /* Closing forgets: a reopened workstation starts over */
    v_clsvwk(h);
    h = open_vwk();
    check(h > 0, "reopened workstation");
    check(is_rgb(h, 209, 3, 0x8000, 0x8000, 0x8000), "reopened workstation has the defaults");

    /*
     * The fg/bg queries answer in COLOR_ENTRY's units, 0-65535 a component,
     * not the palette's 0-1000.  The palette keeps a component to a
     * thousandth, so a value comes back within one step of what was set.
     */
    vq_extnd(h, 1, work_out);
    palette_screen = work_out[4] < 16;
    check(set_colour(h, 200, 1, 1, 0xffff, 0, 0) == 1, "vsf_fg_color full red");
    if (palette_screen)
    {
        nearest(h, 0xffff, 0, 0, &r2, &g2, &b2);
        check(is_rgb(h, 202, 1, r2, g2, b2), "vqf_fg_color reads full red as the nearest pen");
    } else
        check(is_rgb(h, 202, 1, 0xffff, 0, 0), "vqf_fg_color reads full red as ffff");
    check(set_colour(h, 201, 0, 1, 0, 0x8000, 0xffff) == 1, "vst_bg_color half green, full blue");
    get_colour(h, 203, 0, &r1, &g1, &b1);
    snprintf(what, sizeof(what), "COLORS text bg reads %04x %04x %04x", r1, g1, b1);
    say(what);
    {
        if (!palette_screen)
            check(r1 == 0 && g1 >= 0x8000 - 66 && g1 <= 0x8000 + 66 && b1 == 0xffff,
                  "vqt_bg_color reads back within one palette step");
        else
        {
            /*
             * A screen with a colour table has no pen to spare for an RGB
             * colour; it gets the nearest pen there is, and the query says
             * which colour that is.
             */
            nearest(h, 0, 0x8000, 0xffff, &r2, &g2, &b2);
            snprintf(what, sizeof(what), "COLORS %d planes: nearest pen is %04x %04x %04x",
                     work_out[4], r2, g2, b2);
            say(what);
            check(r1 == r2 && g1 == g2 && b1 == b2,
                  "vqt_bg_color reads back the nearest colour the screen has");
        }
    }

    /* A pen chosen by number reads back as that pen's colour */
    vsf_color(h, 1);
    check(is_rgb(h, 202, 1, 0, 0, 0), "vsf_color 1 reads back black");
    vsf_color(h, 0);
    check(is_rgb(h, 202, 1, 0xffff, 0xffff, 0xffff), "vsf_color 0 reads back white");

out:
    if (h > 0)
        v_clsvwk(h);
    appl_exit();
    if (!failed)
        say("FVDI COLORS GUEST PASS");
    return failed;
}
