/*
 * fontscan_guest.c - add a font at run time through the NVDI cookie,
 * the way HighWire does it, and check it can be used without a reboot.
 *
 * NVDI 5 publishes two private vectors in its cookie: get_font_dir (+$B0)
 * lists the font folders and scan_fonts (+$AC) re-walks them.  HighWire's
 * web fonts depend on them: it writes a downloaded font into an HWFONTS\
 * folder beside the installed families, rescans, and looks the new family
 * up by name.  This program repeats that sequence step by step.
 *
 * Run it under the AES (it opens a virtual workstation).  Before boot, put
 * a TrueType font that is in no font folder at C:\FONTSCAN\TESTFONT.TTF (or
 * C:\VTEST\TESTFONT.TTF) and
 * pass its family name as the first argument (default "DejaVu Sans").  It
 * copies the file into <font folder>\HWFONTS\ itself, so the font cannot
 * have been there when fVDI loaded its fonts.
 *
 * Every check is reported over BIOS device 1, the serial port, and the run
 * ends with "FVDI FONTSCAN GUEST PASS" or "FVDI FONTSCAN FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

#define SOURCE_FONT "C:\\FONTSCAN\\TESTFONT.TTF"
#define SOURCE_FONT2 "C:\\VTEST\\TESTFONT.TTF"   /* where the test runner puts it */
#define NVDI_COOKIE 0x4E564449UL    /* 'NVDI' */
#define FVDI_COOKIE 0x66564449UL    /* 'fVDI' */
#define NVDI_FONTSCAN 0x0100        /* fVDI cookie flag: the vectors are real */
#define MAX_DIRS 16
#define DIR_SIZE 256

/* NVDI's extended font information, as gemlib lays it out */
typedef struct {
    long  size;
    short format;
    short id;
    short index;
    char  font_name[50];
    char  family_name[50];
    char  style_name[50];
    char  file_name1[200];
    char  file_name2[200];
    char  file_name3[200];
    short pt_cnt;
    short pt_sizes[64];
} XFNT_INFO;

short vqt_xfntinfo(short handle, short flags, short id, short index, XFNT_INFO *info);
short vqt_name_and_id(short handle, short font_format, char *font_name, char *ret_name);

static short handle;
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
    char line[200];

    snprintf(line, sizeof(line), "FONTSCAN %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI FONTSCAN FAIL: %s", what);
        say(line);
    }
}

/* ---- cookies (the jar is in supervisor memory) ------------------------ */

static unsigned long cookie_wanted;
static long cookie_found;

static long jar_walk(void)
{
    long *jar = *(long **) 0x5A0L;

    cookie_found = 0;
    while (jar && jar[0])
    {
        if ((unsigned long) jar[0] == cookie_wanted)
        {
            cookie_found = jar[1];
            break;
        }
        jar += 2;
    }
    return 0;
}

static void *get_cookie(unsigned long id)
{
    cookie_wanted = id;
    Supexec(jar_walk);
    return (void *) cookie_found;
}

/* ---- the two vectors, called as HighWire calls them ------------------- */

static void *scan_fn, *getdir_fn;
static char dirs[MAX_DIRS][DIR_SIZE];
static char *dir_ptrs[MAX_DIRS];
static short dir_count;
static char extra_dir[DIR_SIZE];

static short call_getdir(short index, char *buf)
{
    register long r_d0 __asm__("d0") = index;
    register char *r_a0 __asm__("a0") = buf;
    register void *r_a1 __asm__("a1") = getdir_fn;

    __asm__ __volatile__("jsr (%[fn])"
        : "+d"(r_d0), "+a"(r_a0), [fn] "+a"(r_a1)
        :
        : "d1", "d2", "a2", "memory", "cc");
    return (short) r_d0;
}

static void call_scan(short count, long flags, char **list)
{
    register long r_d0 __asm__("d0") = count;
    register long r_d1 __asm__("d1") = flags;
    register char **r_a0 __asm__("a0") = list;
    register void *r_a1 __asm__("a1") = scan_fn;

    __asm__ __volatile__("jsr (%[fn])"
        : "+d"(r_d0), "+d"(r_d1), "+a"(r_a0), [fn] "+a"(r_a1)
        :
        : "d2", "a2", "memory", "cc");
}

/* Enumerate the folders, add the extra one, rescan - in supervisor mode */
static long rescan_super(void)
{
    short n = 0, i;

    for (i = 0; i < MAX_DIRS; i++)
    {
        dirs[i][0] = 0;
        if (!call_getdir(i + 1, dirs[i]))
            break;
        dir_ptrs[n++] = dirs[i];
    }
    if (extra_dir[0] && n < MAX_DIRS)
    {
        strcpy(dirs[n], extra_dir);
        dir_ptrs[n] = dirs[n];
        n++;
    }
    if (n > 0)
        call_scan(n, 0L, dir_ptrs);
    dir_count = n;
    return 0;
}

/* ---- helpers ----------------------------------------------------------- */

static short font_count(void)
{
    return vst_load_fonts(handle, 0);
}

/* HighWire's webfont_family_id(): the regular face of a family, or any face */
static short family_id(const char *family, char *full_name)
{
    XFNT_INFO xi;
    short idx, any = 0;

    for (idx = 1; idx <= 400; idx++)
    {
        if (vqt_xfntinfo(handle, 0x0003, 0, idx, &xi) == 0)
            break;
        if (xi.family_name[0] && !strcasecmp(xi.family_name, family))
        {
            if (!any)
            {
                any = xi.id;
                strcpy(full_name, xi.font_name);
            }
            if (!strstr(xi.font_name, "Bold") && !strstr(xi.font_name, "Italic") &&
                !strstr(xi.font_name, "Oblique"))
            {
                strcpy(full_name, xi.font_name);
                return xi.id;
            }
        }
    }
    return any;
}

/* HighWire's nvdi_font_dir(): a loaded font's folder, minus its family folder */
static int font_folder(char *out)
{
    XFNT_INFO xi;
    short idx;
    char *p;

    for (idx = 1; idx <= 400; idx++)
    {
        if (vqt_xfntinfo(handle, 0x0008, 0, idx, &xi) == 0)
            break;
        if (xi.file_name1[0] && xi.format != 1)
        {
            strcpy(out, xi.file_name1);
            if ((p = strrchr(out, '\\')) == NULL)
                continue;
            *p = 0;                          /* the family folder */
            if ((p = strrchr(out, '\\')) == NULL)
                continue;
            p[1] = 0;                        /* the font folder itself */
            return 1;
        }
    }
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    static char buf[16384];
    long in, out, n;
    int ok = 1;

    if ((in = Fopen(from, 0)) < 0)
        return 0;
    if ((out = Fcreate(to, 0)) < 0)
    {
        Fclose((short) in);
        return 0;
    }
    while ((n = Fread((short) in, sizeof(buf), buf)) > 0)
    {
        if (Fwrite((short) out, n, buf) != n)
        {
            ok = 0;
            break;
        }
    }
    if (n < 0)
        ok = 0;
    Fclose((short) in);
    Fclose((short) out);
    return ok;
}

int main(int argc, char **argv)
{
    const char *family = argc > 1 ? argv[1] : "DejaVu Sans";
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh;
    char folder[DIR_SIZE], target[DIR_SIZE + 32], name[64], line[300];
    unsigned short *nvdi, *fvdi;
    short before, after, again, id, id2, extent[8];
    long gdos;

    say("FONTSCAN start");
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;

    gdos = vq_vgdos();
    check(!memcmp(&gdos, "fVDI", 4), "vq_vgdos says fVDI");

    nvdi = get_cookie(NVDI_COOKIE);
    fvdi = get_cookie(FVDI_COOKIE);
    check(nvdi != NULL, "NVDI cookie present");
    check(fvdi != NULL, "fVDI cookie present");
    if (failed)
        goto out;
    snprintf(line, sizeof(line), "FONTSCAN NVDI version %04x, fVDI flags %04x", nvdi[0], fvdi[1]);
    say(line);
    check(nvdi[0] >= 0x0500 && nvdi[0] <= 0x0599, "NVDI version is 5.x");
    check((fvdi[1] & NVDI_FONTSCAN) != 0, "fVDI cookie flags the font vectors as real");
    scan_fn = *(void **) ((char *) nvdi + 0xAC);
    getdir_fn = *(void **) ((char *) nvdi + 0xB0);
    check(scan_fn && !((long) scan_fn & 1), "scan_fonts vector usable");
    check(getdir_fn && !((long) getdir_fn & 1), "get_font_dir vector usable");
    if (failed)
        goto out;

    check(font_folder(folder), "font folder found from a loaded font's path");
    if (failed)
        goto out;
    snprintf(line, sizeof(line), "FONTSCAN font folder %s", folder);
    say(line);

    before = font_count();
    snprintf(line, sizeof(line), "FONTSCAN fonts before: %d", before);
    say(line);
    check(family_id(family, name) == 0, "test family not loaded at boot");
    if (failed)
        goto out;

    /* What a web font download does: a new file in HWFONTS\ */
    snprintf(extra_dir, sizeof(extra_dir), "%sHWFONTS\\", folder);
    snprintf(target, sizeof(target), "%sHWFONTS", folder);
    (void) Dcreate(target);              /* may exist already */
    snprintf(target, sizeof(target), "%sHWFONTS\\TESTFONT.TTF", folder);
    check(copy_file(SOURCE_FONT, target) || copy_file(SOURCE_FONT2, target),
          "font copied into HWFONTS");
    if (failed)
        goto out;

    Supexec(rescan_super);
    snprintf(line, sizeof(line), "FONTSCAN rescanned %d folder(s), first %s", dir_count, dirs[0]);
    say(line);
    check(dir_count >= 2, "get_font_dir listed the configured folder");

    after = font_count();
    snprintf(line, sizeof(line), "FONTSCAN fonts after: %d", after);
    say(line);
    check(after == before + 1, "exactly one font added");

    id = family_id(family, name);
    snprintf(line, sizeof(line), "FONTSCAN family '%s' -> id %d, name '%s'", family, id, name);
    say(line);
    check(id != 0, "new family found by name");
    if (failed)
        goto out;

    id2 = vqt_name_and_id(handle, 0, name, line);
    check(id2 == id, "vqt_name_and_id finds the same id");
    check(vst_font(handle, id) == id, "vst_font selects it");
    vst_point(handle, 12, &cw, &ch, &bw, &bh);
    vqt_extent(handle, "Hamburgefonts", extent);
    snprintf(line, sizeof(line), "FONTSCAN extent width %d", extent[2] - extent[0]);
    say(line);
    check(extent[2] - extent[0] > 0, "text in the new font has a width");

    Supexec(rescan_super);
    again = font_count();
    check(again == after, "a second rescan adds nothing");

out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI FONTSCAN GUEST PASS");
    return failed;
}
