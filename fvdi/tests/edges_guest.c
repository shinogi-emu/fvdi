/*
 * edges_guest.c - calls an old VDI got wrong at the edges: v_cellarray
 * (opcode 10) and v_contourfill (103) must return, vq_chcells must give
 * rows and columns of a landscape screen that fit it, and v_curleft must
 * move the text cursor one column to the left.
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI EDGES GUEST PASS" or "FVDI EDGES FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <string.h>

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

    snprintf(line, sizeof(line), "EDGES %s: %s", ok ? "ok" : "FAILED", what);
    say(line);
    if (!ok && !failed)
    {
        failed = 1;
        snprintf(line, sizeof(line), "FVDI EDGES FAIL: %s", what);
        say(line);
    }
}

int main(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, ch, bw, bh;
    short screen_w, screen_h;
    short rows, cols;
    short row0, col0, row1, col1, row2, col2;
    short cells[16];
    short pxy[4];
    char line[200];
    int i;

    say("EDGES start");
    check(appl_init() >= 0, "AES application registration");
    handle = graf_handle(&cw, &ch, &bw, &bh);
    v_opnvwk(work_in, &handle, work_out);
    check(handle > 0, "virtual workstation");
    if (failed)
        goto out;
    screen_w = work_out[0] + 1;
    screen_h = work_out[1] + 1;
    snprintf(line, sizeof(line), "EDGES screen %dx%d, character cell %dx%d",
             screen_w, screen_h, cw, ch);
    say(line);

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, 0);

    /* v_cellarray and v_contourfill: that they return is the check */
    for (i = 0; i < 16; i++)
        cells[i] = i & 1;
    pxy[0] = 20; pxy[1] = 20; pxy[2] = 27; pxy[3] = 27;
    v_cellarray(handle, pxy, 4, 4, 4, 1, cells);
    check(1, "v_cellarray returns");
    vsf_color(handle, 1);
    v_contourfill(handle, 22, 22, 1);
    check(1, "v_contourfill returns");

    /* the character cells of the screen */
    rows = cols = 0;
    vq_chcells(handle, &rows, &cols);
    snprintf(line, sizeof(line), "EDGES vq_chcells rows %d, columns %d", rows, cols);
    say(line);
    check(rows > 0 && cols > 0, "vq_chcells gives rows and columns");
    if (screen_w > screen_h)
        check(rows < cols, "a landscape screen has fewer rows than columns");
    check((long)rows * ch <= screen_h, "the rows fit the screen height");

    /* v_curleft moves one column to the left */
    v_enter_cur(handle);
    v_curaddress(handle, 5, 10);
    row0 = col0 = 0;
    vq_curaddress(handle, &row0, &col0);
    v_curleft(handle);
    row1 = col1 = 0;
    vq_curaddress(handle, &row1, &col1);
    v_curleft(handle);
    row2 = col2 = 0;
    vq_curaddress(handle, &row2, &col2);
    v_exit_cur(handle);
    snprintf(line, sizeof(line), "EDGES cursor %d,%d -> %d,%d -> %d,%d",
             row0, col0, row1, col1, row2, col2);
    say(line);
    check(row1 == row0 && col1 == col0 - 1, "v_curleft moves the cursor one column left");
    check(row2 == row0 && col2 == col0 - 2, "and again");

    graf_mouse(M_ON, 0);
    wind_update(END_UPDATE);
    form_dial(FMD_FINISH, 0, 0, 0, 0, 0, 0, screen_w, screen_h);

out:
    if (handle > 0)
        v_clsvwk(handle);
    appl_exit();
    if (!failed)
        say("FVDI EDGES GUEST PASS");
    return failed;
}
