/*
 * ctab_guest.c - the colour tables (205, 206) and inverse colour tables
 * (208) of EmuTOS's VDI, from fVDI's transfer_guest.c (colour_table_test,
 * the palette checks of planar_screen_test, the inverse tables of
 * indexed_test) made to run on any screen EmuTOS has.
 *
 * (a) vq_ctab: a short buffer is refused and left alone; the table lists the
 *     colours asked for the pens by pixel value (v_ctab_idx2vdi), its id is
 *     vq_ctab_id's and stays while the colours do
 * (b) v_ctab_idx2vdi / v_ctab_vdi2idx: the TOS order, both ways, -1 outside
 * (c) vs_ctab_entry: sets the pen of the value, a new id, refusals;
 *     v_ctab_idx2value: the pixel the screen gets for the value (read back
 *     on 16 bits), pen 1's for a value that is not one
 * (d) vq_dflt_ctab, vs_dflt_ctab and vs_ctab: the default table is what
 *     vs_dflt_ctab sets, vs_ctab puts the old colours back; bad tables are
 *     refused
 * (e) v_get_ctab_id, v_create_ctab and v_delete_ctab for every depth
 *     (tables the program reads and writes), refusals
 * (f) v_create_itab / v_delete_itab: numbers never given twice, refusals,
 *     another workstation cannot delete them
 * (g) the tables of a workstation go when it is closed: Malloc(-1) the same
 *     after 25 rounds of making tables and closing without deleting them
 *
 * Run under the AES.  Reports over BIOS device 1 (serial) and ends with
 * "FVDI CTAB GUEST PASS" or "FVDI CTAB FAIL: <check>".
 */

#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned short reserved, red, green, blue;
} Entry;

typedef struct {
    long magic, length, format, reserved, map_id, color_space, flags, no_colors;
    long reserved1, reserved2, reserved3, reserved4;
    Entry colors[256];
} CTab;

#define CTAB_MAGIC  0x63746162L
#define HEADER      48

static short contrl[12], intin[16], ptsin[16], intout[64], ptsout[16];
static int failed;
static short screen, planes, ncol;
static long checks;

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

    checks++;
    if (!ok)
    {
        snprintf(line, sizeof(line), "CTAB FAILED: %s", what);
        say(line);
        if (!failed)
        {
            failed = 1;
            snprintf(line, sizeof(line), "FVDI CTAB FAIL: %s", what);
            say(line);
        }
    }
}

/* a VDI call with intin and intout of the caller's choice (NULL: the arrays above) */
static void call(short h, short opcode, short sub, short nintin, short nptsin, void *in, void *out)
{
    short *pb[5];

    pb[0] = contrl;
    pb[1] = in ? (short *)in : intin;
    pb[2] = ptsin;
    pb[3] = out ? (short *)out : intout;
    pb[4] = ptsout;
    contrl[0] = opcode;
    contrl[1] = nptsin;
    contrl[2] = 0;
    contrl[3] = nintin;
    contrl[4] = 0;
    contrl[5] = sub;
    contrl[6] = h;
    __asm__ __volatile__(
        "move.l %0,%%d1\n\t"
        "move.w #115,%%d0\n\t"
        "trap #2"
        :
        : "g"(pb)
        : "d0", "d1", "d2", "a0", "a1", "a2", "memory", "cc");
}

static long get_long(const short *p)
{
    return ((long)p[0] << 16) | (unsigned short)p[1];
}

static void put_long(short *p, long v)
{
    p[0] = (short)(v >> 16);
    p[1] = (short)v;
}

/* the bindings */
static short vq_ctab(short h, long room, CTab *t)
{
    put_long(intin, room);
    call(h, 206, 0, 2, 0, NULL, t);
    return contrl[4];
}

static long vq_ctab_entry(short h, short index, Entry *e)
{
    intin[0] = index;
    call(h, 206, 1, 1, 0, NULL, NULL);
    memcpy(e, intout + 2, 8);
    return get_long(intout);
}

static long vq_ctab_id(short h)
{
    call(h, 206, 2, 0, 0, NULL, NULL);
    return get_long(intout);
}

static short idx2vdi(short h, short index)
{
    intin[0] = index;
    call(h, 206, 3, 1, 0, NULL, NULL);
    return intout[0];
}

static short vdi2idx(short h, short pen)
{
    intin[0] = pen;
    call(h, 206, 4, 1, 0, NULL, NULL);
    return intout[0];
}

static unsigned long idx2value(short h, short index)
{
    intin[0] = index;
    call(h, 206, 5, 1, 0, NULL, NULL);
    return (unsigned long)get_long(intout);
}

static long get_ctab_id(short h)
{
    call(h, 206, 6, 0, 0, NULL, NULL);
    return get_long(intout);
}

static short vq_dflt_ctab(short h, long room, CTab *t)
{
    put_long(intin, room);
    call(h, 206, 7, 2, 0, NULL, t);
    return contrl[4];
}

static CTab *create_ctab(short h, long space, unsigned long format)
{
    put_long(intin, space);
    put_long(intin + 2, (long)format);
    call(h, 206, 8, 4, 0, NULL, NULL);
    return (CTab *)get_long(intout);
}

static short delete_ctab(short h, void *t)
{
    put_long(intin, (long)t);
    call(h, 206, 9, 2, 0, NULL, NULL);
    return intout[0];
}

static short vs_ctab(short h, CTab *t, short nintin)
{
    call(h, 205, 0, nintin, 0, t, NULL);
    return intout[0];
}

static short vs_ctab_entry(short h, short index, long space, const Entry *e, short nintin)
{
    intin[0] = index;
    put_long(intin + 1, space);
    memcpy(intin + 3, e, 8);
    call(h, 205, 1, nintin, 0, NULL, NULL);
    return intout[0];
}

static short vs_dflt_ctab(short h)
{
    call(h, 205, 2, 0, 0, NULL, NULL);
    return intout[0];
}

static long create_itab(short h, void *t, short bits)
{
    put_long(intin, (long)t);
    intin[2] = bits;
    call(h, 208, 0, 3, 0, NULL, NULL);
    return get_long(intout);
}

static short delete_itab(short h, long id)
{
    put_long(intin, id);
    call(h, 208, 1, 2, 0, NULL, NULL);
    return intout[0];
}

/* a VDI component 0-1000 as one of 0-65535, and back (as the VDI converts) */
static unsigned short to16(short v)
{
    if (v <= 0)
        return 0;
    if (v >= 1000)
        return 0xffff;
    return (unsigned short)(((unsigned long)v * 65535UL + 500) / 1000);
}

static short to1000(unsigned short v)
{
    return (short)(((unsigned long)v * 1000UL + 32767) / 65535UL);
}

static short open_vwk(void)
{
    short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short work_out[57];
    short cw, chh, bw, bh, h;

    h = graf_handle(&cw, &chh, &bw, &bh);
    v_opnvwk(work_in, &h, work_out);
    return h;
}

/* the pixel drawn in a pen at (8, 8) of the screen, read back */
static unsigned long drawn_pixel(short pen)
{
    static unsigned short buf[16 * 16];
    short xy[8], bar[4] = { 8, 8, 8, 8 };
    MFDB scr, mem;
    unsigned long v = 0;
    int p;

    vsf_interior(screen, 1);
    vsf_color(screen, pen);
    vswr_mode(screen, 1);
    vsf_perimeter(screen, 0);
    vr_recfl(screen, bar);
    memset(&scr, 0, sizeof(scr));
    memset(&mem, 0, sizeof(mem));
    memset(buf, 0, sizeof(buf));
    mem.fd_addr = buf;
    mem.fd_w = 16;
    mem.fd_h = 1;
    mem.fd_wdwidth = 1;
    mem.fd_nplanes = planes;
    xy[0] = 8; xy[1] = 8; xy[2] = 8; xy[3] = 8;
    xy[4] = 0; xy[5] = 0; xy[6] = 0; xy[7] = 0;
    vro_cpyfm(screen, 3, xy, &scr, &mem);
    if (planes == 16)
        return buf[0];
    for (p = 0; p < planes; p++)
        if (buf[p] & 0x8000)
            v |= 1UL << p;
    return v;
}

static CTab saved, table, dflt, other;

/* TRUE under MiNT (the cookie jar has 'MiNT') */
static long mint_cookie(void)
{
    long *jar = *(long **)0x5a0;

    if (jar)
        for (; jar[0]; jar += 2)
            if (jar[0] == 0x4d694e54L)
                return 1;
    return 0;
}

/* (a), (b) */
static void test_query(void)
{
    char text[120];
    short rgb[3];
    Entry e;
    long id;
    int i;

    memset(&table, 0xa7, sizeof(table));
    memcpy(&other, &table, sizeof(table));
    check(vq_ctab(screen, HEADER + 8L * ncol - 1, &table) == 0, "a buffer one byte short is refused");
    check(memcmp(&table, &other, sizeof(table)) == 0, "a refused buffer is left alone");
    check(vq_ctab(screen, sizeof(table), &table) == (HEADER + 8 * ncol) / 2, "vq_ctab gives the table");
    check(table.magic == CTAB_MAGIC && table.length == HEADER + 8L * ncol && table.format == 0 &&
          table.color_space == 1 && table.no_colors == ncol && table.map_id != 0, "the header of the table");
    check(((unsigned char *)&table)[HEADER + 8 * ncol] == 0xa7 || ncol == 256, "nothing past the table");
    id = vq_ctab_id(screen);
    check(id == table.map_id, "vq_ctab_id is the id of the table");
    check(vq_ctab_id(screen) == id, "the id stays while the colours do");

    for (i = 0; i < ncol; i++)
    {
        short pen = idx2vdi(screen, i);

        if (pen < 0 || pen >= ncol)
        {
            check(0, "v_ctab_idx2vdi gives a pen");
            break;
        }
        check(vdi2idx(screen, pen) == i, "v_ctab_vdi2idx undoes v_ctab_idx2vdi");
        vq_color(screen, pen, 0, rgb);
        if (table.colors[i].red != to16(rgb[0]) || table.colors[i].green != to16(rgb[1]) ||
            table.colors[i].blue != to16(rgb[2]) || table.colors[i].reserved != 0)
        {
            snprintf(text, sizeof(text), "entry %d (pen %d) is the colour asked for the pen", i, pen);
            check(0, text);
        }
        if (vq_ctab_entry(screen, i, &e) != 1 || memcmp(&e, &table.colors[i], 8))
        {
            snprintf(text, sizeof(text), "vq_ctab_entry %d is entry %d of the table", i, i);
            check(0, text);
        }
    }
    check(idx2vdi(screen, 0) == 0 && idx2vdi(screen, ncol - 1) == 1, "TOS order: value 0 is pen 0, the last is pen 1");
    if (ncol >= 4)
        check(idx2vdi(screen, 1) == 2 && idx2vdi(screen, 2) == 3, "TOS order: values 1 and 2 are pens 2 and 3");
    if (ncol == 256)
        check(idx2vdi(screen, 15) == 255, "TOS order: value 15 is pen 255");
    check(idx2vdi(screen, -1) == -1 && idx2vdi(screen, ncol) == -1, "v_ctab_idx2vdi outside the table");
    check(vdi2idx(screen, -1) == -1 && vdi2idx(screen, ncol) == -1, "v_ctab_vdi2idx outside the pens");
    check(vq_ctab_entry(screen, ncol, &e) == 0 && e.red == 0 && e.green == 0 && e.blue == 0,
          "vq_ctab_entry outside the table: colour space 0, black");
    check(vq_ctab_entry(screen, -1, &e) == 0, "vq_ctab_entry of -1");
    memcpy(&saved, &table, sizeof(table));
}

/* (c) */
static void test_set_entry(void)
{
    short index = ncol >= 4 ? 2 : 1, pen = idx2vdi(screen, index), rgb[3];
    long id = vq_ctab_id(screen);
    Entry e = { 0, 12345, 23456, 34567 }, back;
    unsigned long value, want;

    check(vs_ctab_entry(screen, index, 1, &e, 7) == 1, "vs_ctab_entry sets one entry");
    check(vq_ctab_id(screen) != id && vq_ctab_id(screen) != 0, "a new id after the colours change");
    vq_color(screen, pen, 0, rgb);
    check(rgb[0] == to1000(12345) && rgb[1] == to1000(23456) && rgb[2] == to1000(34567),
          "the pen of the value is set (vq_color)");
    check(vq_ctab_entry(screen, index, &back) == 1 && back.red == to16(to1000(12345)) &&
          back.green == to16(to1000(23456)) && back.blue == to16(to1000(34567)),
          "vq_ctab_entry gives the colour back");
    value = idx2value(screen, index);
    want = planes == 16 ? drawn_pixel(pen) : (unsigned long)index;
    check(value == want, "v_ctab_idx2value is the pixel the screen gets");
    want = planes == 16 ? drawn_pixel(1) : (unsigned long)(ncol - 1);
    check(idx2value(screen, -1) == want && idx2value(screen, ncol) == want,
          "v_ctab_idx2value of a value that is not one: pen 1's pixel");
    if (planes != 16)
        check(idx2value(screen, ncol - 1) == (unsigned long)(ncol - 1), "v_ctab_idx2value of the last value");

    id = vq_ctab_id(screen);
    check(vs_ctab_entry(screen, -1, 1, &e, 7) == 0, "vs_ctab_entry of -1 is refused");
    check(vs_ctab_entry(screen, ncol, 1, &e, 7) == 0, "vs_ctab_entry past the table is refused");
    check(vs_ctab_entry(screen, index, 2, &e, 7) == 0, "vs_ctab_entry in another colour space is refused");
    check(vs_ctab_entry(screen, index, 1, &e, 6) == 0, "vs_ctab_entry with intin too short is refused");
    check(vq_ctab_id(screen) == id, "refused calls change nothing");

    check(vs_ctab_entry(screen, index, 0, &saved.colors[index], 7) == 1, "vs_ctab_entry puts the colour back");
    vq_ctab(screen, sizeof(table), &table);
    check(!memcmp(table.colors, saved.colors, 8L * ncol), "the table is as it was");
}

/* (d) */
static void test_default(void)
{
    short rgb[3];
    int i, ok = 1;

    memset(&dflt, 0xa7, sizeof(dflt));
    memcpy(&other, &dflt, sizeof(dflt));
    check(vq_dflt_ctab(screen, HEADER + 8L * ncol - 1, &dflt) == 0 && !memcmp(&dflt, &other, sizeof(dflt)),
          "vq_dflt_ctab refuses a short buffer and leaves it alone");
    check(vq_dflt_ctab(screen, sizeof(dflt), &dflt) == (HEADER + 8 * ncol) / 2, "vq_dflt_ctab gives the table");
    check(dflt.magic == CTAB_MAGIC && dflt.no_colors == ncol && dflt.length == HEADER + 8L * ncol &&
          dflt.map_id == (planes == 16 ? 16 : planes), "the default table's header (its id is its bits)");
    check(dflt.colors[0].red == 0xffff && dflt.colors[0].green == 0xffff && dflt.colors[0].blue == 0xffff,
          "the default value 0 is white");
    check(dflt.colors[ncol - 1].red == 0 && dflt.colors[ncol - 1].green == 0 && dflt.colors[ncol - 1].blue == 0,
          "the default last value is black");

    check(vs_dflt_ctab(screen) == ncol, "vs_dflt_ctab sets every value");
    vq_ctab(screen, sizeof(table), &table);
    check(!memcmp(table.colors, dflt.colors, 8L * ncol), "vs_dflt_ctab sets the default table");

    /* refusals: the table is not changed */
    memcpy(&other, &saved, sizeof(saved));
    other.magic ^= 1;
    check(vs_ctab(screen, &other, (HEADER + 8 * ncol) / 2) == 0, "vs_ctab refuses a table without the magic");
    memcpy(&other, &saved, sizeof(saved));
    other.no_colors = 257;
    check(vs_ctab(screen, &other, (HEADER + 8 * ncol) / 2) == 0, "vs_ctab refuses 257 colours");
    memcpy(&other, &saved, sizeof(saved));
    check(vs_ctab(screen, &other, (HEADER + 8 * ncol) / 2 - 1) == 0, "vs_ctab refuses intin shorter than the table");
    memcpy(&other, &saved, sizeof(saved));
    other.length = HEADER + 8 * ncol - 2;
    check(vs_ctab(screen, &other, (HEADER + 8 * ncol) / 2) == 0, "vs_ctab refuses a length short of the colours");
    vq_ctab(screen, sizeof(table), &table);
    check(!memcmp(table.colors, dflt.colors, 8L * ncol), "refused tables change nothing");

    check(vs_ctab(screen, &saved, (HEADER + 8 * ncol) / 2) == ncol, "vs_ctab sets the whole table");
    vq_ctab(screen, sizeof(table), &table);
    check(!memcmp(table.colors, saved.colors, 8L * ncol), "the colours are back");
    for (i = 0; i < ncol; i++)
    {
        vq_color(screen, idx2vdi(screen, i), 0, rgb);
        if (to16(rgb[0]) != saved.colors[i].red || to16(rgb[1]) != saved.colors[i].green ||
            to16(rgb[2]) != saved.colors[i].blue)
            ok = 0;
    }
    check(ok, "and so are the pens (vq_color)");

    /* a part of a table: the first two values */
    memcpy(&other, &dflt, sizeof(dflt));
    other.no_colors = 2;
    other.length = HEADER + 16;
    check(vs_ctab(screen, &other, (HEADER + 16) / 2) == 2, "vs_ctab of two colours sets two values");
    vq_ctab(screen, sizeof(table), &table);
    check(!memcmp(table.colors, dflt.colors, 16) &&
          (ncol == 2 || !memcmp(table.colors + 2, saved.colors + 2, 8L * (ncol - 2))),
          "the others stay");
    vs_ctab(screen, &saved, (HEADER + 8 * ncol) / 2);
}

/* (e) */
static void test_create(void)
{
    static const short depths[8] = { 1, 2, 4, 8, 15, 16, 24, 32 };
    long a = get_ctab_id(screen), b = get_ctab_id(screen), on_mint = Supexec(mint_cookie);
    CTab *t, *u;
    char text[120];
    int i;

    check(a > 32 && b > a, "v_get_ctab_id gives new ids");
    for (i = 0; i < 8; i++)
    {
        long n = depths[i] <= 8 ? 1L << depths[i] : 256;

        t = create_ctab(screen, i & 1, depths[i]);
        snprintf(text, sizeof(text), "v_create_ctab of %d bits", depths[i]);
        check(t != NULL, text);
        if (!t)
            continue;
        check(t->magic == CTAB_MAGIC && t->no_colors == n && t->length == HEADER + 8 * n &&
              t->color_space == 1 && t->map_id > b, "the header of a table made");
        check(t->colors[0].red == 0xffff && t->colors[0].blue == 0xffff, "its value 0 is white");
        if (depths[i] <= 8)
            check(t->colors[n - 1].red == 0 && t->colors[n - 1].green == 0 && t->colors[n - 1].blue == 0,
                  "its last value is black (pixel order)");
        t->colors[1].red = 0x1234;
        t->colors[n - 1].blue = 0x4321;
        check(t->colors[1].red == 0x1234 && t->colors[n - 1].blue == 0x4321, "the program writes the table");
        /*
         * The table is a block of its own: EmuTOS's Mshrink to its own size
         * succeeds only on the start of a block and changes nothing (MiNT
         * refuses blocks the program does not own: not asked there)
         */
        if (!on_mint)
            check(Mshrink(t, t->length) == 0, "the table starts a block of its own");
        u = create_ctab(screen, 1, depths[i]);
        check(u != NULL && u != t && u->map_id != t->map_id, "a second one is another table");
        if (u)
        {
            long k;
            int intact = 1;

            /* the program may write all of its table: the VDI keeps nothing of its own there */
            memset(u, 0x5a, u->length);
            check(delete_ctab(screen, t) == 1, "v_delete_ctab");
            for (k = 0; k < 48 + 8 * n; k++)
                if (((unsigned char *)u)[k] != 0x5a)
                    intact = 0;
            check(intact, "deleting a table leaves the other table's bytes alone");
            check(delete_ctab(screen, t) == 0, "a second v_delete_ctab is refused");
            check(delete_ctab(screen, u) == 1, "v_delete_ctab of a table the program overwrote");
        }
    }
    check(create_ctab(screen, 2, 8) == NULL, "v_create_ctab refuses colour space 2");
    check(create_ctab(screen, 1, 3) == NULL, "v_create_ctab refuses 3 bits");
    check(create_ctab(screen, 1, 0) == NULL, "v_create_ctab refuses 0 bits");
    check(delete_ctab(screen, &table) == 0, "v_delete_ctab refuses a table it did not make");
    check(delete_ctab(screen, NULL) == 0, "v_delete_ctab refuses NULL");
}

/* (f) */
static void test_itab(void)
{
    static CTab pal;
    long id, old, id3, id5;
    short h2;
    CTab *made;

    memset(&pal, 0, sizeof(pal));
    pal.magic = CTAB_MAGIC;
    pal.length = HEADER + 4 * 8;
    pal.color_space = 1;
    pal.no_colors = 4;
    pal.colors[0].red = pal.colors[1].green = pal.colors[2].blue = 0xffff;
    pal.colors[3].red = pal.colors[3].green = pal.colors[3].blue = 0xffff;

    id = create_itab(screen, &pal, 4);
    check(id != 0, "v_create_itab");
    check(create_itab(screen, &pal, 2) == 0 && create_itab(screen, &pal, 6) == 0, "bits 2 and 6 are refused");
    check(create_itab(screen, NULL, 4) == 0, "a NULL table is refused");
    check(create_itab(screen, (char *)&pal + 1, 4) == 0, "a table at an odd address is refused");
    pal.magic ^= 1;
    check(create_itab(screen, &pal, 4) == 0, "a table without the magic is refused");
    pal.magic ^= 1;
    pal.no_colors = 0;
    check(create_itab(screen, &pal, 4) == 0, "a table of no colours is refused");
    pal.no_colors = 4;
    pal.length = HEADER + 3 * 8;
    check(create_itab(screen, &pal, 4) == 0, "a table shorter than its colours is refused");
    pal.length = HEADER + 4 * 8;

    h2 = open_vwk();
    check(h2 > 0, "a second workstation");
    if (h2 > 0)
    {
        check(delete_itab(h2, id) == 0, "another workstation cannot delete the inverse table");
        made = create_ctab(h2, 1, 4);
        check(made && delete_ctab(screen, made) == 0, "nor a colour table another made");
        v_clsvwk(h2);
    }
    check(delete_itab(screen, id) == 1, "v_delete_itab");
    check(delete_itab(screen, id) == 0, "a second v_delete_itab is refused");
    check(delete_itab(screen, 0) == 0, "v_delete_itab of 0 is refused");
    old = id;
    id = create_itab(screen, &pal, 4);
    check(id != 0 && id != old, "numbers are not given again");
    id3 = create_itab(screen, &pal, 3);
    id5 = create_itab(screen, &pal, 5);
    check(id3 != 0 && id5 != 0 && id3 != id && id5 != id3, "3 and 5 bits");
    made = create_ctab(screen, 1, 8);
    check(made && create_itab(screen, made, 4) != 0, "an inverse table for a table the VDI made");
    check(delete_itab(screen, id) == 1 && delete_itab(screen, id3) == 1 && delete_itab(screen, id5) == 1,
          "deleting them");
    if (made)
        delete_ctab(screen, made);
}

/* (g) */
static void test_lifecycle(void)
{
    static CTab pal;
    long before, after, tt_before, tt_after;
    int round, i;
    char text[120];

    /* sixteen colours at most: the rounds are about memory, not about the time a map takes */
    memcpy(&pal, &saved, sizeof(saved));
    if (pal.no_colors > 16)
    {
        pal.no_colors = 16;
        pal.length = HEADER + 16 * 8;
    }
    before = (long)Malloc(-1L);
    tt_before = (long)Mxalloc(-1L, 1);         /* TT-RAM, where the VDI's blocks may go */
    for (round = 0; round < 25; round++)
    {
        short h = open_vwk();

        if (h <= 0)
        {
            check(0, "a workstation for each round");
            return;
        }
        for (i = 0; i < 3; i++)
        {
            CTab *m = create_ctab(h, 1, i == 0 ? 8 : i == 1 ? 4 : 32);

            /* the program writes all of it, header too, before the close frees it */
            if (m)
                memset(m, 0xff, m->length);
        }
        create_itab(h, &pal, 3);
        create_itab(h, &pal, 4);
        create_itab(h, &pal, 5);
        vq_ctab_id(h);              /* the VDI's own snapshot */
        v_clsvwk(h);
    }
    after = (long)Malloc(-1L);
    tt_after = (long)Mxalloc(-1L, 1);
    snprintf(text, sizeof(text), "CTAB Malloc(-1) %ld before, %ld after 25 rounds; TT-RAM %ld, %ld",
             before, after, tt_before, tt_after);
    say(text);
    check(after == before && tt_after == tt_before, "the tables of a closed workstation are freed");
}

int main(void)
{
    short work_out[57];
    char text[120];

    say("CTAB start");
    check(appl_init() >= 0, "AES application registration");
    screen = open_vwk();
    check(screen > 0, "virtual workstation");
    if (failed)
        goto out;
    vq_extnd(screen, 1, work_out);
    planes = work_out[4];
    vq_extnd(screen, 0, work_out);
    ncol = work_out[13] > 256 ? 256 : work_out[13];
    snprintf(text, sizeof(text), "CTAB %d planes, %d colours", planes, ncol);
    say(text);
    check(planes == 1 || planes == 2 || planes == 4 || planes == 8 || planes == 16,
          "a screen of 1, 2, 4, 8 or 16 planes");
    if (failed)
        goto out;

    graf_mouse(M_OFF, 0);
    wind_update(BEG_UPDATE);
    test_query();
    say("CTAB queries done");
    test_set_entry();
    say("CTAB entries done");
    test_default();
    say("CTAB default and whole tables done");
    test_create();
    say("CTAB made tables done");
    test_itab();
    say("CTAB inverse tables done");
    test_lifecycle();
    say("CTAB lifecycle done");
    vs_ctab(screen, &saved, (HEADER + 8 * ncol) / 2);
    wind_update(END_UPDATE);
    graf_mouse(M_ON, 0);

out:
    if (screen > 0)
        v_clsvwk(screen);
    appl_exit();
    if (!failed)
    {
        snprintf(text, sizeof(text), "CTAB %ld checks", checks);
        say(text);
        say("FVDI CTAB GUEST PASS");
    }
    return failed;
}
