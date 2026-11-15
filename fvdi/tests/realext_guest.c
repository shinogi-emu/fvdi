/*
 * realext_guest.c - vqt_real_extent (240 sub 4200) and vq_ext_devinfo
 * (248 sub 4242).
 *
 * vqt_real_extent is checked against what vqt_f_extent and vqt_fontinfo
 * say about the same string: the width must agree, and the box must sit
 * where the alignment puts the text.  It is run in the system font and in
 * a FreeType font when one is loaded.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI REALEXT GUEST PASS" or "FVDI REALEXT FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

void vqt_real_extent(short handle, short x, short y, const char *string, short extent[]);
short vq_ext_devinfo(short handle, short device, short *dev_exists,
                     char *file_path, char *file_name, char *name);
void vqt_f_extent(short handle, const char *str, short extent[]);

static int failed;
static short handle;
static short have_fext = 1;     /* vqt_f_extent is answered (0: use vqt_extent) */

/* ---- the fVDI cookie (the jar is in supervisor memory) ----------------- */

static long cookie_found;

static long jar_walk(void)
{
    long *jar = *(long **) 0x5A0L;

    cookie_found = 0;
    while (jar && jar[0])
    {
        if ((unsigned long) jar[0] == 0x66564449UL)    /* 'fVDI' */
        {
            cookie_found = 1;
            break;
        }
        jar += 2;
    }
    return 0;
}

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

    snprintf(line, sizeof(line), "REALEXT %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI REALEXT FAIL: %s", what);
        say(line);
    }
}

/*
 * One string, one alignment: the real extent at (x,y) must be the plain
 * extent's width, from the left edge the horizontal alignment gives, and
 * span the font's top to bottom line around the baseline the vertical
 * alignment gives.
 */
static void one(const char *label, short hor, short ver)
{
    static const char *text = "Hamburgefonts";
    short x = 100, y = 200;
    short ext[8], real[8], distances[5], effects[3], min, max, maxw, h, v;
    short width, left, baseline;
    char line[200], what[120];

    vst_alignment(handle, hor, ver, &h, &v);
    if (have_fext)
        vqt_f_extent(handle, text, ext);
    else
        vqt_extent(handle, text, ext);
    vqt_fontinfo(handle, &min, &max, distances, &maxw, effects);
    memset(real, 0x55, sizeof(real));
    vqt_real_extent(handle, x, y, text, real);

    width = ext[2] - ext[0];
    left = hor == 1 ? x - width / 2 : hor == 2 ? x - width : x;
    /* distances: bottom, descent, half, ascent, top */
    baseline = ver == 1 ? y + distances[2] : ver == 2 ? y + distances[3] :
               ver == 3 ? y - distances[0] : ver == 4 ? y - distances[1] :
               ver == 5 ? y + distances[4] : y;

    snprintf(line, sizeof(line),
             "REALEXT %s align %d,%d: extent w %d; real %d,%d %d,%d %d,%d %d,%d; top %d bottom %d",
             label, h, v, width, real[0], real[1], real[2], real[3], real[4], real[5],
             real[6], real[7], distances[4], distances[0]);
    say(line);

    snprintf(what, sizeof(what), "%s align %d,%d: corners in order and a rectangle", label, hor, ver);
    check(real[0] == real[6] && real[2] == real[4] && real[1] == real[3] && real[5] == real[7], what);
    snprintf(what, sizeof(what), "%s align %d,%d: width agrees with vqt_f_extent", label, hor, ver);
    check(real[2] - real[0] == width, what);
    snprintf(what, sizeof(what), "%s align %d,%d: left edge where the alignment puts it", label, hor, ver);
    check(real[0] == left, what);
    snprintf(what, sizeof(what), "%s align %d,%d: top and bottom lines around the baseline", label, hor, ver);
    check(real[5] == baseline - distances[4] && real[1] == baseline + distances[0], what);
}

static void alignments(const char *label)
{
    short h, v;

    one(label, 0, 0);
    one(label, 1, 5);
    one(label, 2, 3);
    one(label, 0, 2);
    vst_alignment(handle, 0, 0, &h, &v);   /* back to the default */
}

/*
 * Rotation: the cells v_gtext covers are the box vqt_real_extent gives.
 * Fill an area with pen 1, draw the string in replace mode in colour 1 (its
 * cells come out white on black), and find the box of pixels that are no
 * longer pen 1 by walking outward from the expected one.  As without
 * rotation the box ends at the far end of the text (its corners are one
 * past the last column), and covers both lines of the cells.
 */
static short pen_pixel, planes;

static short pixel_at(short x, short y)
{
    short pelw, idx;

    v_get_pixel(handle, x, y, &pelw, &idx);
    return idx;
}

static void rotation_one(short angle, short hor, short ver, short effects)
{
    static const char *text = "Hamburgefonts";
    const short px = 160, py = 200;
    short real[8], xy[4], h, v, dummy, x, y, i;
    short minx, maxx, miny, maxy, ok, top_ok, bottom_ok, left_ok, right_ok;
    char what[120];

    /*
     * the 16-bit text path clears the skew's extra columns across x whatever
     * the rotation, so rotated skewed text there covers more than its cells:
     * not checked in true colour
     */
    if (angle && (effects & 4) && planes >= 16)
    {
        say("REALEXT rotation with skew skipped in true colour");
        return;
    }
    vst_alignment(handle, hor, ver, &h, &v);
    vst_rotation(handle, angle);
    vst_effects(handle, effects);
    vswr_mode(handle, 1);
    vsf_interior(handle, 1);
    vsf_color(handle, 1);
    vsf_perimeter(handle, 0);
    xy[0] = 4; xy[1] = 10; xy[2] = 316; xy[3] = 390;
    vr_recfl(handle, xy);
    pen_pixel = pixel_at(6, 12);
    vst_color(handle, 1);
    memset(real, 0x55, sizeof(real));
    vqt_real_extent(handle, px, py, text, real);
    v_gtext(handle, px, py, (char *) text);

    minx = maxx = real[0];
    miny = maxy = real[1];
    for (i = 1; i < 4; i++)
    {
        if (real[2 * i] < minx) minx = real[2 * i];
        if (real[2 * i] > maxx) maxx = real[2 * i];
        if (real[2 * i + 1] < miny) miny = real[2 * i + 1];
        if (real[2 * i + 1] > maxy) maxy = real[2 * i + 1];
    }
    switch (angle)      /* the far end of the text is one past */
    {
    case 0:    maxx--; break;
    case 900:  miny++; break;
    case 1800: minx++; break;
    default:   maxy--; break;
    }

    ok = 1;
    top_ok = bottom_ok = left_ok = right_ok = 0;
    for (x = minx - 1; x <= maxx + 1; x++)
    {
        if (pixel_at(x, miny - 1) != pen_pixel || pixel_at(x, maxy + 1) != pen_pixel)
            ok = 0;
        if (x >= minx && x <= maxx)
        {
            top_ok |= pixel_at(x, miny) != pen_pixel;
            bottom_ok |= pixel_at(x, maxy) != pen_pixel;
        }
    }
    for (y = miny - 1; y <= maxy + 1; y++)
    {
        if (pixel_at(minx - 1, y) != pen_pixel || pixel_at(maxx + 1, y) != pen_pixel)
            ok = 0;
        if (y >= miny && y <= maxy)
        {
            left_ok |= pixel_at(minx, y) != pen_pixel;
            right_ok |= pixel_at(maxx, y) != pen_pixel;
        }
    }
    {
        char line[200];

        snprintf(line, sizeof(line), "REALEXT rotation %d effects %d align %d,%d: corners %d,%d %d,%d %d,%d %d,%d; box %d,%d-%d,%d",
                 angle, effects, hor, ver, real[0], real[1], real[2], real[3], real[4], real[5], real[6], real[7],
                 minx, miny, maxx, maxy);
        say(line);
    }
    snprintf(what, sizeof(what), "rotation %d effects %d align %d,%d: nothing drawn outside the box", angle, effects, hor, ver);
    check(ok, what);
    snprintf(what, sizeof(what), "rotation %d effects %d align %d,%d: drawing on every edge of the box", angle, effects, hor, ver);
    check(top_ok && bottom_ok && left_ok && right_ok, what);
    vst_rotation(handle, 0);
    vst_effects(handle, 0);
    vst_alignment(handle, 0, 0, &dummy, &dummy);
}

static void rotation(void)
{
    static const short angles[3] = { 900, 1800, 2700 };
    short a;

    vst_font(handle, 1);
    vst_effects(handle, 0);
    rotation_one(0, 0, 0, 0);          /* the unrotated box, the same way */
    for (a = 0; a < 3; a++)
    {
        rotation_one(angles[a], 0, 0, 0);
        rotation_one(angles[a], 1, 5, 0);
        rotation_one(angles[a], 2, 3, 0);
        rotation_one(angles[a], 0, 2, 0);
        rotation_one(angles[a], 0, 0, 4);       /* skew */
        rotation_one(angles[a], 1, 5, 4);
        rotation_one(angles[a], 2, 2, 4);
        rotation_one(angles[a], 0, 3, 16);      /* outline */
        rotation_one(angles[a], 1, 0, 16);
        rotation_one(angles[a], 2, 5, 4 | 16);  /* both */
    }
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, exists, open, fonts, i, id;
    char path[130], file[14], name[34], fname[40], line[300];
    short h, locked = 0, fvdi;

    say("REALEXT start");
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    h = handle;
    {
        short ext[57];

        vq_extnd(handle, 1, ext);
        planes = ext[4];
    }
    Supexec(jar_walk);
    fvdi = (short) cookie_found;
    /* without fVDI or a GDOS the system's own VDI has no 240 sub 0 */
    have_fext = fvdi || (short) vq_vgdos() != -2;
    /* the drawing checks read the screen back: no desktop repaint meanwhile */
    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    locked = 1;

    /* System font */
    vst_font(h, 1);
    alignments("system font");
    vst_effects(h, 1 | 4 | 16);         /* bold, italic, outlined */
    one("system font, effects", 0, 0);
    vst_effects(h, 0);

    /*
     * fVDI never rotates text; the system's own VDI does.  The rotation
     * checks fill y 10..390 around a string at y 200: they need a screen
     * of 400 lines or more (work_out[1] is the last line).
     */
    if (fvdi)
        say("REALEXT fVDI present - rotation checks skipped");
    else if (work_out[1] < 399)
        say("REALEXT screen under 400 lines - rotation checks skipped");
    else
        rotation();

    /* A FreeType font, if there is one */
    /* no font list to walk without a GDOS (the system's own VDI) */
    fonts = (short)vq_vgdos() != -2 ? vst_load_fonts(h, 0) : 0;
    id = 0;
    for (i = 2; i <= fonts + 1 && !id; i++)
    {
        short fid = vqt_name(h, i, fname);
        if (fid >= 5000)
            id = fid;
    }
    if (id)
    {
        snprintf(line, sizeof(line), "REALEXT outline font %d '%s'", id, fname);
        say(line);
        check(vst_font(h, id) == id, "outline font selected");
        vst_point(h, 14, &cw, &ch, &bw, &bh);
        alignments("outline font");
    } else
        say("REALEXT no outline font loaded - outline checks skipped");

    /* vq_ext_devinfo: the screen device, and one nobody has */
    memset(path, 0x55, sizeof(path));
    memset(file, 0x55, sizeof(file));
    memset(name, 0x55, sizeof(name));
    exists = 0;
    open = vq_ext_devinfo(h, 1, &exists, path, file, name);
    path[sizeof(path) - 1] = file[sizeof(file) - 1] = name[sizeof(name) - 1] = 0;
    snprintf(line, sizeof(line), "REALEXT device 1: exists %d open %d path '%s' file '%s' name '%s'",
             exists, open, path, file, name);
    say(line);
    check(exists != 0 && open != 0, "screen device exists and is open");
    if (fvdi)
    {
        check(strlen(file) > 0 && strlen(file) <= 12 && strchr(file, '.') != NULL &&
              strchr(file, '\\') == NULL, "driver file name, without its folder");
        check(strlen(path) > 2 && strlen(path) <= 127 && path[1] == ':' &&
              path[strlen(path) - 1] == '\\', "driver folder, absolute");
    } else
    {
        /* no driver files: the system's own VDI names none */
        check(file[0] == 0, "no driver file name");
        check(path[0] == 0, "no driver folder");
    }
    check(fvdi ? (strlen(name) > 0 && strlen(name) <= 31) : !strcmp(name, "EmuTOS VDI"), "driver name");
    exists = 1;
    open = vq_ext_devinfo(h, 9, &exists, path, file, name);
    check(exists == 0 && open == 0, "no driver for device 9");

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
        say("FVDI REALEXT GUEST PASS");
    return failed;
}
