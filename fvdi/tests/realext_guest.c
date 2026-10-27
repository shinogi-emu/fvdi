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
    vqt_f_extent(handle, text, ext);
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

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh, exists, open, fonts, i, id;
    char path[130], file[14], name[34], fname[40], line[300];
    short h;

    say("REALEXT start");
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    h = handle;

    /* System font */
    vst_font(h, 1);
    alignments("system font");
    vst_effects(h, 1 | 4 | 16);         /* bold, italic, outlined */
    one("system font, effects", 0, 0);
    vst_effects(h, 0);

    /* A FreeType font, if there is one */
    fonts = vst_load_fonts(h, 0);
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
    check(strlen(file) > 0 && strlen(file) <= 12 && strchr(file, '.') != NULL &&
          strchr(file, '\\') == NULL, "driver file name, without its folder");
    check(strlen(path) > 2 && strlen(path) <= 127 && path[1] == ':' &&
          path[strlen(path) - 1] == '\\', "driver folder, absolute");
    check(strlen(name) > 0 && strlen(name) <= 31, "driver name");
    exists = 1;
    open = vq_ext_devinfo(h, 9, &exists, path, file, name);
    check(exists == 0 && open == 0, "no driver for device 9");

out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI REALEXT GUEST PASS");
    return failed;
}
