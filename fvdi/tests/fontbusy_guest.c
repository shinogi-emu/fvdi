/*
 * fontbusy_guest.c - two programs size outline fonts at the same time.
 *
 * Under MiNT a program can sleep inside a VDI call that loads a font file,
 * with the outline font engine busy, while another program calls the VDI.
 * That one must not lose the face it selects (vst_point() and vst_height()
 * may not give the system font because the engine was busy for a moment),
 * and vst_load_fonts() must not give 0 because another program's call is
 * still scanning the fonts folder.
 *
 * FONTBUSY.PRG, started with no argument, starts itself twice at once
 * (Pexec mode 100) as "FONTBUSY.PRG 1 N" and "FONTBUSY.PRG 2 N" and waits
 * for both.  N, the number of faces each child sizes, is read from
 * C:\VTEST\FONTBUSY.CFG ("faces=N", default 12), and so is a delay in
 * seconds before the children start ("delay=S", default 0), for the
 * programs started at boot to be done.  Each child opens a
 * virtual workstation, loads the fonts, and sizes N faces, child 1 from
 * the first face on and child 2 from the last one back, so that they load
 * different font files; each face must be selected (vst_font() and
 * vqt_attributes() give its id) and measure text.  The children write
 * their reports to C:\VTEST\FONTBUS1.LOG and FONTBUS2.LOG, which the
 * parent copies to the serial port when both have ended (their output
 * would interleave on the port).
 *
 * Font files load too fast on a host folder for the calls to meet: run it
 * with a VDI whose font file reads sleep a while (a test build).
 *
 * Reports over BIOS device 1 (serial) and ends with
 * "FVDI FONTBUSY GUEST PASS" or "FVDI FONTBUSY FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <mint/mintbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static long logfh = -1;
static short handle;

static void say(const char *text)
{
    if (logfh >= 0)
    {
        Fwrite((short)logfh, strlen(text), text);
        Fwrite((short)logfh, 2, "\r\n");
        return;
    }
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

static void check(int ok, const char *what)
{
    char line[200];

    snprintf(line, sizeof(line), "FONTBUSY %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI FONTBUSY FAIL: %s", what);
        say(line);
    }
}

static long read_hz200(void)
{
    return *(volatile long *)0x4baL;
}

static long ticks(void)
{
    return Supexec(read_hz200);
}

/* FONTBUSY.CFG: faces=N, and delay=S, seconds to wait before starting the children */
static short config(const char *key, short value)
{
    char buf[100], *p;
    long fh, n;

    fh = Fopen("C:\\VTEST\\FONTBUSY.CFG", 0);
    if (fh < 0)
        return value;
    n = Fread((short)fh, sizeof(buf) - 1, buf);
    Fclose((short)fh);
    if (n <= 0)
        return value;
    buf[n] = 0;
    if ((p = strstr(buf, key)) != NULL)
        return (short)atoi(p + strlen(key));
    return value;
}

/* one of the two children: size the faces, one after the other */
static int child(int role, short faces)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 }, work_out[57];
    short cw, ch, bw, bh, fonts, sysfonts, k, i, id, got, attrib[10], ext[8];
    short wrong_font = 0, wrong_attrib = 0, no_extent = 0, sized = 0;
    char name[40], line[200], path[40];
    long t;

    snprintf(path, sizeof(path), "C:\\VTEST\\FONTBUS%d.LOG", role);
    logfh = Fcreate(path, 0);

    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    sysfonts = work_out[10];

    t = ticks();
    fonts = vst_load_fonts(handle, 0);
    snprintf(line, sizeof(line), "FONTBUSY child %d: vst_load_fonts gives %d in %ld ticks", role, fonts, ticks() - t);
    say(line);
    check(fonts > 0, "vst_load_fonts gives the outline faces, though the other child may be scanning");
    if (faces > fonts)
        faces = fonts;

    t = ticks();
    for (k = 0; k < faces; k++)
    {
        i = (role == 1) ? sysfonts + 1 + k : sysfonts + fonts - k;
        id = vqt_name(handle, i, name);
        got = vst_font(handle, id);
        if (k & 1)
            vst_height(handle, 10 + k % 7, &cw, &ch, &bw, &bh);
        else
            vst_point(handle, 9 + k % 5, &cw, &ch, &bw, &bh);
        vqt_attributes(handle, attrib);
        vqt_extent(handle, "Abg", ext);
        if (got != id)
            wrong_font++;
        if (attrib[0] != id)
            wrong_attrib++;
        if (ext[2] - ext[0] <= 0)
            no_extent++;
        if (got != id || attrib[0] != id)
        {
            snprintf(line, sizeof(line), "FONTBUSY child %d: face %d '%s' gave vst_font %d, then font %d",
                     role, id, name, got, attrib[0]);
            say(line);
        }
        else
            sized++;
    }
    snprintf(line, sizeof(line), "FONTBUSY child %d: %d of %d faces kept in %ld ticks", role, sized, faces,
             ticks() - t);
    say(line);
    check(wrong_font == 0, "vst_font gives each face selected, even with the engine busy");
    check(wrong_attrib == 0, "each face stays selected after vst_point or vst_height");
    check(no_extent == 0, "each face measures text");
    vst_font(handle, 1);
    v_clsvwk(handle);
out:
    appl_exit();
    if (!failed)
        say("FVDI FONTBUSY CHILD PASS");
    if (logfh >= 0)
        Fclose((short)logfh);
    return failed;
}

/* the reports of the children, to the serial port */
static void copy_log(int role)
{
    char path[40], buf[512], line[300];
    long fh, n;
    short len = 0;

    snprintf(path, sizeof(path), "C:\\VTEST\\FONTBUS%d.LOG", role);
    fh = Fopen(path, 0);
    if (fh < 0)
    {
        snprintf(line, sizeof(line), "FONTBUSY no report from child %d", role);
        say(line);
        return;
    }
    while ((n = Fread((short)fh, sizeof(buf), buf)) > 0)
    {
        long j;

        for (j = 0; j < n; j++)
        {
            if (buf[j] == '\r')
                continue;
            if (buf[j] == '\n' || len == (short)sizeof(line) - 1)
            {
                line[len] = 0;
                say(line);
                len = 0;
                continue;
            }
            line[len++] = buf[j];
        }
    }
    Fclose((short)fh);
}

int main(int argc, char **argv)
{
    char tail[16], line[200];
    long pid[2], r;
    short faces, delay, k, codes[2];

    if (argc > 2)
        return child(atoi(argv[1]), (short)atoi(argv[2]));

    say("FONTBUSY start");
    faces = config("faces=", 12);
    delay = config("delay=", 0);
    snprintf(line, sizeof(line), "FONTBUSY two children, %d faces each, after %d s", faces, delay);
    say(line);
    for (k = 0; k < delay; k++)
        Fselect(1000, NULL, NULL, NULL);    /* a second's sleep */
    for (k = 0; k < 2; k++)
    {
        snprintf(tail + 1, sizeof(tail) - 1, "%d %d", k + 1, faces);
        tail[0] = (char)strlen(tail + 1);
        pid[k] = Pexec(100, "C:\\VTEST\\FONTBUSY.PRG", tail, NULL);
        snprintf(line, sizeof(line), "FONTBUSY child %d is process %ld", k + 1, pid[k]);
        say(line);
        check(pid[k] > 0, "a child is started");
    }
    if (failed)
        goto out;
    codes[0] = codes[1] = -1;
    for (k = 0; k < 2; k++)
    {
        r = Pwaitpid((short)pid[k], 0, NULL);
        codes[k] = (short)(r & 0xffff);
    }
    copy_log(1);
    copy_log(2);
    snprintf(line, sizeof(line), "FONTBUSY children returned %d and %d", codes[0], codes[1]);
    say(line);
    check(codes[0] == 0 && codes[1] == 0, "both children pass");
out:
    if (!failed)
        say("FVDI FONTBUSY GUEST PASS");
    return failed;
}
