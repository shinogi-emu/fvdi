/*
 * vdibench_guest.c - time the screen operations a driver spends its life
 * on, and checksum what each one leaves on the screen.
 *
 * Each test clears the screen, draws a fixed sequence, reads the 200 Hz
 * system timer around it and then a CRC-32 of the whole screen.  A faster
 * driver must leave the same CRC as the one it replaces: the CRC is the
 * check that an optimisation changed no pixel.  Ticks are only comparable
 * between runs on the same machine, mode and emulator settings.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial):
 *   VDIBENCH <test> ticks=<n> crc=<hex>
 * and ends with "FVDI VDIBENCH GUEST PASS".  Any argument is taken as a
 * repeat factor for every test (default 1).
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static short handle, width, height, planes;
static unsigned long crc_table[256];
static int repeat = 1;

static void say(const char *text)
{
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

static long read_hz200(void)
{
    return *(volatile long *)0x4ba;
}

static long ticks(void)
{
    return Supexec(read_hz200);
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

static unsigned long screen_crc(void)
{
    const unsigned char *p = Logbase();
    long n = ((long)width * planes + 7) / 8 * height;
    unsigned long c = 0xffffffffUL;

    while (n-- > 0)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffUL;
}

static void clear(void)
{
    short box[4] = { 0, 0, 0, 0 };

    box[2] = width - 1;
    box[3] = height - 1;
    vswr_mode(handle, 1);
    vsf_interior(handle, 1);
    vsf_color(handle, 0);
    vsf_perimeter(handle, 0);
    vr_recfl(handle, box);
}

static void report(const char *name, long t)
{
    char line[120];

    snprintf(line, sizeof(line), "VDIBENCH %s ticks=%ld crc=%08lx", name, t, screen_crc());
    say(line);
}

/* Lines: solid, replace mode, colour cycling through the pens */
static void lines(const char *name, int kind, short mode)
{
    short xy[4], i, n, r;
    long t;

    clear();
    vswr_mode(handle, mode);
    vsl_type(handle, 1);
    vsl_width(handle, 1);
    t = ticks();
    for (r = 0; r < repeat; r++)
    {
        n = kind == 0 ? height : width;
        for (i = 0; i < n; i++)
        {
            vsl_color(handle, (i % 15) + 1);
            switch (kind)
            {
            case 0:     /* horizontal, varying start and end inside a word */
                xy[0] = i % 37;
                xy[1] = i;
                xy[2] = width - 1 - (i % 29);
                xy[3] = i;
                break;
            case 1:     /* vertical */
                xy[0] = i;
                xy[1] = i % 23;
                xy[2] = i;
                xy[3] = height - 1 - (i % 19);
                break;
            default:    /* diagonal fan from the top left */
                xy[0] = 0;
                xy[1] = 0;
                xy[2] = i;
                xy[3] = height - 1;
                break;
            }
            v_pline(handle, 2, xy);
        }
    }
    report(name, ticks() - t);
}

/*
 * Every line style in every writing mode, short and long, horizontal,
 * vertical and slanted, from starts all over a word: mostly a pixel
 * check, the pattern has to start where it did.
 */
static void line_styles(const char *name)
{
    short xy[4], mode, style, i, y = 0, r;
    long t;

    clear();
    vsl_width(handle, 1);
    vsl_udsty(handle, (short)0xa5c3);
    t = ticks();
    for (r = 0; r < repeat; r++)
    {
        for (mode = 1; mode <= 4; mode++)
        {
            vswr_mode(handle, mode);
            for (style = 1; style <= 7; style++)
            {
                vsl_type(handle, style);
                for (i = 0; i < 6; i++)
                {
                    vsl_color(handle, (mode * 7 + style + i) % 15 + 1);
                    xy[0] = (i * 7 + style * 3) % 40;
                    xy[1] = y % height;
                    xy[2] = width - 1 - (i * 5 + style) % 37;
                    xy[3] = xy[1];
                    v_pline(handle, 2, xy);
                    /* the same, drawn right to left */
                    xy[1] = xy[3] = (y + 1) % height;
                    xy[0] ^= xy[2];
                    xy[2] ^= xy[0];
                    xy[0] ^= xy[2];
                    v_pline(handle, 2, xy);
                    /* a short one inside a word, and one across two */
                    xy[0] = 3 + i;
                    xy[2] = 9 + i;
                    xy[1] = xy[3] = (y + 2) % height;
                    v_pline(handle, 2, xy);
                    xy[0] = 13 + i;
                    xy[2] = 21 + i * 3;
                    v_pline(handle, 2, xy);
                    /* vertical, and slanted both ways */
                    xy[0] = xy[2] = (mode * 61 + style * 13 + i * 3) % width;
                    xy[1] = (i * 11) % 40;
                    xy[3] = height - 1 - (style * 7) % 30;
                    v_pline(handle, 2, xy);
                    xy[2] = (xy[0] + 40 + i * 9) % width;
                    v_pline(handle, 2, xy);
                    xy[0] = (xy[0] + 200) % width;
                    xy[2] = (xy[0] + width - 90 - i * 17) % width;
                    xy[3] = xy[1] + 25 + i;
                    v_pline(handle, 2, xy);
                    y += 3;
                }
            }
        }
    }
    vsl_type(handle, 1);
    report(name, ticks() - t);
}

/* Text in the system font, at word-aligned and unaligned positions */
static void text(const char *name, short font, short point, short mode)
{
    static const char msg[] = "The quick brown fox jumps over the lazy dog 0123456789";
    short cw, ch, bw, bh, y, x, r, dummy;
    long t;

    clear();
    vswr_mode(handle, mode);
    vst_font(handle, font);
    vst_point(handle, point, &cw, &ch, &bw, &bh);
    vst_alignment(handle, 0, 5, &dummy, &dummy);
    if (bh <= 0)
        bh = 16;
    t = ticks();
    for (r = 0; r < repeat; r++)
    {
        for (y = 0, x = 0; y + bh <= height; y += bh, x = (x + 3) % 16)
        {
            vst_color(handle, (y / bh) % 15 + 1);
            v_gtext(handle, x, y, (char *)msg);
        }
    }
    report(name, ticks() - t);
}

/*
 * Monochrome bitmaps expanded to the screen with vrt_cpyfm, the way icons
 * and text arrive: every writing mode, foreground and background colours
 * across the pens, at every alignment within a word.
 */
static void expands(const char *name)
{
    static unsigned short icon[64 * 4];        /* 64x64, one plane */
    MFDB src, screen;
    short xy[8], colours[2], i, mode, r;
    long t;

    for (i = 0; i < 64 * 4; i++)
        icon[i] = (unsigned short)(0x9a3c * (i + 1)) ^ (unsigned short)(i * 0x1111);
    memset(&src, 0, sizeof(src));
    src.fd_addr = icon;
    src.fd_w = 64;
    src.fd_h = 64;
    src.fd_wdwidth = 4;
    src.fd_stand = 0;
    src.fd_nplanes = 1;
    memset(&screen, 0, sizeof(screen));
    clear();
    t = ticks();
    for (r = 0; r < repeat; r++)
    {
        for (mode = 1; mode <= 4; mode++)
        {
            for (i = 0; i < 24; i++)
            {
                colours[0] = (mode * 5 + i) % 16;
                colours[1] = (mode * 3 + i * 7 + 1) % 16;
                xy[0] = i % 7;
                xy[1] = i % 5;
                xy[2] = xy[0] + 20 + i * 2;
                xy[3] = xy[1] + 30 + i;
                xy[4] = (mode * 37 + i * 13) % (width - 70);
                xy[5] = (mode * 53 + i * 19) % (height - 70);
                xy[6] = xy[4] + xy[2] - xy[0];
                xy[7] = xy[5] + xy[3] - xy[1];
                vrt_cpyfm(handle, mode, xy, &src, &screen, colours);
            }
        }
    }
    report(name, ticks() - t);
}

/* Screen to screen copies: a pattern first, then the copies */
static void blits(const char *name, short dx_offset, short mode, int scroll)
{
    MFDB screen;
    short xy[8], i, r, w, h;
    long t;

    memset(&screen, 0, sizeof(screen));
    clear();
    /* Something to copy: diagonal lines in several colours */
    vswr_mode(handle, 1);
    for (i = 0; i < width + height; i += 7)
    {
        short l[4];

        l[0] = i;
        l[1] = 0;
        l[2] = i - height;
        l[3] = height - 1;
        vsl_color(handle, (i / 7) % 15 + 1);
        v_pline(handle, 2, l);
    }
    w = width / 2;
    h = height / 2;
    t = ticks();
    for (r = 0; r < repeat; r++)
    {
        for (i = 0; i < 40; i++)
        {
            if (scroll)
            {
                /* Scroll the whole width up by a line, as a window does */
                xy[0] = 0;
                xy[1] = 1;
                xy[2] = width - 1;
                xy[3] = height - 1;
                xy[4] = 0;
                xy[5] = 0;
                xy[6] = width - 1;
                xy[7] = height - 2;
            } else
            {
                xy[0] = (i * 16) % (width - w);
                xy[1] = (i * 5) % (height - h);
                xy[2] = xy[0] + w - 1;
                xy[3] = xy[1] + h - 1;
                xy[4] = (width - w - 16 - (i * 16) % (width - w - 16)) + dx_offset;
                xy[5] = (height - h) - (i * 3) % (height - h);
                xy[6] = xy[4] + w - 1;
                xy[7] = xy[5] + h - 1;
                if (xy[6] >= width)
                {
                    xy[4] -= xy[6] - (width - 1);
                    xy[6] = width - 1;
                }
            }
            vro_cpyfm(handle, mode, xy, &screen, &screen);
        }
    }
    report(name, ticks() - t);
}

/*
 * Copies along the same rows, right and left by whole words (the overlap
 * a horizontal scroll makes), and narrow copies inside one word.
 */
static void blit_sideways(const char *name)
{
    MFDB screen;
    short xy[8], i, r;
    long t;

    memset(&screen, 0, sizeof(screen));
    clear();
    vswr_mode(handle, 1);
    for (i = 0; i < width + height; i += 5)
    {
        short l[4];

        l[0] = i;
        l[1] = 0;
        l[2] = i - height;
        l[3] = height - 1;
        vsl_color(handle, (i / 5) % 15 + 1);
        v_pline(handle, 2, l);
    }
    t = ticks();
    for (r = 0; r < repeat; r++)
    {
        for (i = 0; i < 12; i++)
        {
            /* right by 16 or 32, start inside a word */
            xy[0] = 3 + i;
            xy[1] = i * 7;
            xy[2] = width - 1 - 48 - i;
            xy[3] = xy[1] + height / 3;
            xy[4] = xy[0] + 16 * (1 + i % 2);
            xy[5] = xy[1];
            xy[6] = xy[2] + 16 * (1 + i % 2);
            xy[7] = xy[3];
            vro_cpyfm(handle, S_ONLY, xy, &screen, &screen);
            /* left by 32, back over the same rows */
            xy[0] = 37 + i;
            xy[1] = height / 2 + i * 3;
            xy[2] = width - 1 - i * 2;
            xy[3] = xy[1] + height / 4;
            xy[4] = xy[0] - 32;
            xy[5] = xy[1];
            xy[6] = xy[2] - 32;
            xy[7] = xy[3];
            vro_cpyfm(handle, S_ONLY, xy, &screen, &screen);
            /* narrow: inside one word, and across one word boundary */
            xy[0] = 2 + i;
            xy[1] = 10 + i;
            xy[2] = xy[0] + 5;
            xy[3] = xy[1] + 40;
            xy[4] = xy[0] + 64;
            xy[5] = xy[1] + 50;
            xy[6] = xy[2] + 64;
            xy[7] = xy[3] + 50;
            vro_cpyfm(handle, S_ONLY, xy, &screen, &screen);
            xy[0] = 12 + i;
            xy[2] = xy[0] + 9;
            xy[4] = xy[0] + 96;
            xy[6] = xy[2] + 96;
            vro_cpyfm(handle, S_ONLY, xy, &screen, &screen);
            /* down by a few rows, overlapping */
            xy[0] = 16;
            xy[1] = 20 + i;
            xy[2] = width / 2 + 15;
            xy[3] = xy[1] + height / 3;
            xy[4] = xy[0];
            xy[5] = xy[1] + 3;
            xy[6] = xy[2];
            xy[7] = xy[3] + 3;
            vro_cpyfm(handle, S_ONLY, xy, &screen, &screen);
        }
    }
    report(name, ticks() - t);
}

/* Filled rectangles: solid or patterned, at unaligned edges */
static void fills(const char *name, short interior, short style, short mode)
{
    short box[4], i, r;
    long t;

    clear();
    vswr_mode(handle, mode);
    vsf_interior(handle, interior);
    vsf_style(handle, style);
    vsf_perimeter(handle, 0);
    t = ticks();
    for (r = 0; r < repeat; r++)
    {
        for (i = 0; i < 100; i++)
        {
            vsf_color(handle, (i % 15) + 1);
            box[0] = (i * 7) % (width / 2);
            box[1] = (i * 3) % (height / 2);
            box[2] = box[0] + width / 2 - 1 - (i % 13);
            box[3] = box[1] + height / 2 - 1 - (i % 11);
            vr_recfl(handle, box);
        }
    }
    report(name, ticks() - t);
}

int main(int argc, char **argv)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57], cw, ch, bw, bh, fonts, i, ttf = 0;
    char line[120], fname[40];

    if (argc > 1)
        repeat = atoi(argv[1]) > 0 ? atoi(argv[1]) : 1;
    appl_init();
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    if (handle <= 0)
    {
        say("FVDI VDIBENCH FAIL: virtual workstation");
        return 1;
    }
    width = work_out[0] + 1;
    height = work_out[1] + 1;
    vq_extnd(handle, 1, work_out);
    planes = work_out[4];
    crc_init();
    {
        short clip[4] = { 0, 0, 0, 0 };

        clip[2] = width - 1;
        clip[3] = height - 1;
        vs_clip(handle, 1, clip);
    }
    snprintf(line, sizeof(line), "VDIBENCH screen %dx%dx%d repeat %d", width, height, planes, repeat);
    say(line);

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);

    lines("hline", 0, 1);
    lines("vline", 1, 1);
    lines("dline", 2, 1);
    lines("hline-xor", 0, 3);
    line_styles("line-styles");
    text("text-sys", 1, 10, 2);
    text("text-sys-replace", 1, 10, 1);
    text("text-sys-xor", 1, 10, 3);
    text("text-sys-revtransparent", 1, 10, 4);
    expands("expand-modes");
    fonts = vst_load_fonts(handle, 0);
    for (i = 2; i <= fonts + 1 && !ttf; i++)
    {
        short id = vqt_name(handle, i, fname);
        if (id >= 5000)
            ttf = id;
    }
    if (ttf)
        text("text-ttf", ttf, 12, 2);
    blits("blit-aligned", 0, S_ONLY, 0);
    blits("blit-unaligned", 3, S_ONLY, 0);
    blits("blit-xor", 5, S_XOR_D, 0);
    blits("blit-scroll", 0, S_ONLY, 1);
    blit_sideways("blit-sideways");
    fills("fill-solid", 1, 0, 1);
    fills("fill-pattern", 2, 4, 1);
    fills("fill-solid-xor", 1, 0, 3);
    fills("fill-transparent", 2, 7, 2);
    fills("fill-revtransparent", 2, 5, 4);

    clear();
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);
    v_clsvwk(handle);
    appl_exit();
    say("FVDI VDIBENCH GUEST PASS");
    return 0;
}
