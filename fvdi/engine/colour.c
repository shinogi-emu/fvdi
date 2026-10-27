/*
 * fVDI colour handling
 *
 * Copyright 2005, Johan Klockars
 * This software is licensed under the GNU General Public License.
 * Please, see LICENSE.TXT for further information.
 */

#include "fvdi.h"
#include "stdio.h"
#include "function.h"
#include "relocate.h"
#include "utility.h"
#include "ctab.h"
#include "itable.h"



static Colour *get_clut(Virtual *vwk)
{
    Colour *palette;
    Workstation *wk;
    char *addr;

    wk = vwk->real_address;
    palette = vwk->palette;
    if (wk->driver->device->clut == 1)          /* Hardware CLUT? (used to test look_up_table) */
        palette = wk->screen.palette.colours;   /* Actually a common global */
    else if (!palette || ((long)palette & 1))
    {
        /* No or only negative palette allocated? */
        addr = malloc((wk->screen.palette.size + NEG_PAL_N) * sizeof(Colour));
        if (!addr)
        {
            PUTS("Could not allocate space for palette!\n");
            return 0;
        } else
        {
            if (!palette)
            {
                /* No palette allocated? */
                palette = vwk->palette = (Colour *)(addr + NEG_PAL_N * sizeof(Colour));   /* Point to index 0 */
                memset(addr, 0, NEG_PAL_N * sizeof(Colour));
            } else
            {                           /* Only negative palette allocated so far? */
                palette = (Colour *)((long)palette & ~1);  /* Copy the negative side first and free it */
                vwk->palette = (Colour *)(addr + NEG_PAL_N * sizeof(Colour));
                copymem_aligned(palette - NEG_PAL_N, addr, NEG_PAL_N * sizeof(Colour));
                free(palette - NEG_PAL_N);
                palette = vwk->palette;
            }
            copymem_aligned(wk->screen.palette.colours, palette, wk->screen.palette.size * sizeof(Colour));
        }
    }

    return palette;
}


void CDECL lib_vs_color(Virtual *vwk, long pen, RGB *values)
{
    Workstation *wk = vwk->real_address;
    Colour *palette;
    DrvPalette palette_pars;

    if (pen < 0 || pen >= wk->screen.palette.size)
        return;

    palette = get_clut(vwk);
    if (!palette)
        return;

    palette_pars.first_pen = pen;
    palette_pars.count = 1;             /* One colour to set up */
    palette_pars.requested = (short *)values;
    palette_pars.palette = palette;

    set_palette(vwk, &palette_pars);
}


static int idx2vdi(Workstation *wk, int index)
{
    static signed char vdi_colours[] = { 0, 2, 3, 6, 4, 7, 5, 8, 9, 10, 11, 14, 12, 15, 13, -1 };
    int ret;

    if ((unsigned int) index >= (unsigned int) wk->screen.palette.size)
        return -1;

    /* No VDI->TOS conversion for true colour */
    if (wk->driver->device->clut != 1)  /* Hardware CLUT? (used to test look_up_table) */
        ret = index;
    else if (index == wk->screen.palette.size - 1)
        ret = 1;
    else if ((index >= 16) && (index != 255))
        ret = index;
    else
    {
        ret = vdi_colours[index];
        if (ret < 0)
            ret = wk->screen.palette.size - 1;
    }

    return ret;
}


static int vdi2idx(Workstation *wk, int vdi_pen)
{
    static signed char tos_colours[] = { 0, -1, 1, 2, 4, 6, 3, 5, 7, 8, 9, 10, 12, 14, 11, 13 };
    int ret;

    if ((unsigned int)vdi_pen >= (unsigned int)wk->screen.palette.size)
        return -1;

    if (wk->driver->device->clut != 1)
        ret = vdi_pen;
    else if (vdi_pen == 255)
        ret = 15;
    else if (vdi_pen >= 16)
        ret = vdi_pen;
    else
    {
        ret = tos_colours[vdi_pen];
        if (ret < 0)
            ret = wk->screen.palette.size - 1;
    }

    return ret;
}

long CDECL ctab_index_to_vdi(Virtual *vwk, long index)
{
    return index < 0 || index > 255 ? -1 : idx2vdi(vwk->real_address, index);
}


int CDECL lib_vq_color(Virtual *vwk, long pen, long flag, RGB *colour)
{
    int index;
    Colour *palette;

    index = vdi2idx(vwk->real_address, (int)pen);
    if (index < 0)
        return -1;

    palette = vwk->palette;
    /* Negative indices are always in local palette, but this can't be one of those */
    if (!palette || ((long)palette & 1))
        palette = vwk->real_address->screen.palette.colours;

    if (flag == 0)
    {
        colour->red = palette[index].vdi.red;
        colour->green = palette[index].vdi.green;
        colour->blue = palette[index].vdi.blue;
    } else
    {
        colour->red = palette[index].hw.red;
        colour->green = palette[index].hw.green;
        colour->blue = palette[index].hw.blue;
    }

    return (int)pen;
}


/*
 * Colour state NVDI defines per workstation that fVDI keeps beside it, not
 * in it.  Virtual is allocated by the drivers as well as by the engine, so
 * growing it would tie every driver binary to this engine's layout.
 *
 * - The highlight, minimum, maximum and weight colours: vs_hilite_color,
 *   vs_min_color, vs_max_color and vs_weight_color (opcode 207,
 *   sub-functions 0 to 3) and their vq_ counterparts (209).  In NVDI they
 *   parameterise vr_transfer_bits' arithmetic modes (highlight, add,
 *   subtract, blend), which fVDI does not implement yet, so for now they
 *   are held and reported back - before, the set calls claimed success and
 *   kept nothing, and the queries always answered black.
 * - The raster foreground and background colours: vsr_fg_color,
 *   vsr_bg_color and their queries (200-203, sub-function 4).  These hold
 *   palette indices like the text, fill, line and marker ones in Virtual.
 */
#define X_COLOURS   4

typedef struct XColours {
    struct XColours *next;
    Virtual *owner;
    COLOR_ENTRY colour[X_COLOURS];
    short raster_fg, raster_bg;
} XColours;

static XColours *x_colours = 0;

/*
 * What a workstation answers before a program sets anything.  NVDI's own
 * defaults are not documented; these are fVDI's choice: black highlight,
 * black and white as the minimum and maximum, an even blend, and raster
 * colours 1 on 0 (black on white) as for every other kind of object.
 */
static const COLOR_RGB x_default[X_COLOURS] = {
    { 0, 0x0000, 0x0000, 0x0000 },   /* highlight */
    { 0, 0x0000, 0x0000, 0x0000 },   /* minimum */
    { 0, 0xffff, 0xffff, 0xffff },   /* maximum */
    { 0, 0x8000, 0x8000, 0x8000 }    /* weight */
};
static short raster_default_fg = 1, raster_default_bg = 0;   /* only ever read */

static XColours *x_colours_find(Virtual *vwk, int create)
{
    XColours *node;
    int i;

    for (node = x_colours; node; node = node->next)
    {
        if (node->owner == vwk)
            return node;
    }
    if (!create)
        return 0;

    if ((node = (XColours *)malloc(sizeof(XColours))) == NULL)
        return 0;
    for (i = 0; i < X_COLOURS; i++)
        node->colour[i].rgb = x_default[i];
    node->raster_fg = raster_default_fg;
    node->raster_bg = raster_default_bg;
    node->owner = vwk;
    node->next = x_colours;
    x_colours = node;

    return node;
}

/* Forget a workstation's colours: on close, and on open in case a program
 * ended without closing one that had the same address. */
void x_colours_close(Virtual *vwk)
{
    XColours **link = &x_colours;

    while (*link)
    {
        XColours *node = *link;

        if (node->owner == vwk)
        {
            *link = node->next;
            free(node);
        } else
            link = &node->next;
    }
}


static int fg_bg_index(Virtual *vwk, int subfunction, short **fg, short **bg, int create)
{
    switch (subfunction)
    {
    case 0:
        *fg = &vwk->text.colour.foreground;
        *bg = &vwk->text.colour.background;
        break;

    case 1:
        *fg = &vwk->fill.colour.foreground;
        *bg = &vwk->fill.colour.background;
        break;

    case 2:
        *fg = &vwk->line.colour.foreground;
        *bg = &vwk->line.colour.background;
        break;

    case 3:
        *fg = &vwk->marker.colour.foreground;
        *bg = &vwk->marker.colour.background;
        break;

    case 4:
        /* Raster operations: kept beside the workstation (see above) */
        {
            XColours *node = x_colours_find(vwk, create);

            if (node)
            {
                *fg = &node->raster_fg;
                *bg = &node->raster_bg;
            } else if (!create)
            {
                *fg = &raster_default_fg;
                *bg = &raster_default_bg;
            } else
                return 0;
        }
        break;

    default:
        return 0;
    }

    return 1;
}


int CDECL lib_vs_fg_color(Virtual *vwk, long subfunction, long colour_space, COLOR_ENTRY *values)
{
    short *fg, *bg, index;
    Colour *palette;
    void *addr;
    DrvPalette palette_pars;

    if ((unsigned int)colour_space > 1)    /* Only 0 or 1 allowed for now (current or RGB) */
        return 0;

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg, 1))
        return -1;
    index = *fg = -subfunction * 2 - 2;      /* Index -2/-4... */

    palette = vwk->palette;
    if (!palette)
    {
        addr = malloc(NEG_PAL_N * sizeof(Colour));
        if (!addr)
        {
            PUTS("Could not allocate space for negative palette!\n");
            return -1;
        }
        palette = vwk->palette = (Colour *)(((long)addr + NEG_PAL_N * sizeof(Colour)) | 1);   /* Point to index 0 */
    }
    palette = (Colour *)((long)palette & ~1);

    palette_pars.first_pen = index;
    palette_pars.count = 1;             /* One colour to set up */
    palette_pars.requested = (short *)((long)values | 1);    /* Odd for new style entries */
    palette_pars.palette = palette;

    set_palette(vwk, &palette_pars);

    return 1;
}


int CDECL lib_vs_bg_color(Virtual *vwk, long subfunction, long colour_space, COLOR_ENTRY *values)
{
    short *fg, *bg, index;
    Colour *palette;
    void *addr;
    DrvPalette palette_pars;

    if ((unsigned int)colour_space > 1)    /* Only 0 or 1 allowed for now (current or RGB) */
        return 0;

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg, 1))
        return -1;
    index = *bg = -subfunction * 2 - 3;      /* Index -3/-5... */

    palette = vwk->palette;
    if (!palette)
    {
        addr = malloc(NEG_PAL_N * sizeof(Colour));
        if (!addr)
        {
            PUTS("Could not allocate space for negative palette!\n");
            return -1;
        }
        palette = vwk->palette = (Colour *)(((long)addr + NEG_PAL_N * sizeof(Colour)) | 1);   /* Point to index 0 */
    }
    palette = (Colour *)((long)palette & ~1);

    palette_pars.first_pen = index;
    palette_pars.count = 1;             /* One colour to set up */
    palette_pars.requested = (short *)((long)values | 1);    /* Odd for new style entries */
    palette_pars.palette = palette;

    set_palette(vwk, &palette_pars);

    return 1;
}


/*
 * A COLOR_ENTRY component runs 0-65535; the palette keeps VDI's 0-1000.
 * Handing out the palette value as it stands made every fg/bg query read
 * full intensity as 1000, 1.5% of the scale the caller expects.
 */
static unsigned short vdi_to_rgb16(short value)
{
    if (value <= 0)
        return 0;
    if (value >= 1000)
        return 0xffff;
    return (unsigned short)(((long)value * 65535L + 500) / 1000);
}


long CDECL lib_vq_fg_color(Virtual *vwk, long subfunction, COLOR_ENTRY *colour)
{
    short *fg, *bg, index;
    Colour *palette;

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg, 0))
        return -1;
    index = *fg;

    palette = vwk->palette;
    if (!palette || (((long)palette & 1) && (index >= 0)))   /* No or only part local? */
        palette = vwk->real_address->screen.palette.colours;
    palette = (Colour *)((long)palette & ~1);

    colour->rgb.reserved = 0;
    colour->rgb.red = vdi_to_rgb16(palette[index].vdi.red);
    colour->rgb.green = vdi_to_rgb16(palette[index].vdi.green);
    colour->rgb.blue = vdi_to_rgb16(palette[index].vdi.blue);

    return 1;    /* RGB_SPACE */
}


long CDECL lib_vq_bg_color(Virtual *vwk, long subfunction, COLOR_ENTRY *colour)
{
    short *fg, *bg, index;
    Colour *palette;

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg, 0))
        return -1;
    index = *bg;

    palette = vwk->palette;
    if (!palette || (((long)palette & 1) && (index >= 0)))   /* No or only part local? */
        palette = vwk->real_address->screen.palette.colours;
    palette = (Colour *)((long)palette & ~1);

    colour->rgb.reserved = 0;
    colour->rgb.red = vdi_to_rgb16(palette[index].vdi.red);
    colour->rgb.green = vdi_to_rgb16(palette[index].vdi.green);
    colour->rgb.blue = vdi_to_rgb16(palette[index].vdi.blue);

    return 1;    /* RGB_SPACE */
}


int CDECL colour_entry(Virtual *vwk, long subfunction, short *intin, short *intout)
{
    (void) vwk;
    (void) intin;
    switch ((int)subfunction)
    {
    case 0:     /* v_color2value */
        PUTS("v_color2value not yet supported\n");
        return 2;

    case 1:     /* v_value2color */
        PUTS("v_value2color not yet supported\n");
        return 6;

    case 2:     /* v_color2nearest */
        PUTS("v_color2nearest not yet supported\n");
        return 6;

    case 3:     /* vq_px_format */
        {
            unsigned long format = ctab_pixel_format(vwk);
            long space = format ? 1 : 0;
            memcpy(intout, &space, sizeof(space));
            memcpy(intout + 2, &format, sizeof(format));
            return 4;
        }

    default:
        PUTS("Unknown colour entry operation\n");
        return 0;
    }
}


static int set_col_table(Virtual *vwk, long count, long start, COLOR_ENTRY *values)
{
    Workstation *wk = vwk->real_address;
    Colour *palette;
    DrvPalette palette_pars;

    if (start < 0 || count <= 0 || start >= wk->screen.palette.size)
        return 0;
    if (count > wk->screen.palette.size - start)
        count = wk->screen.palette.size - start;

    palette = get_clut(vwk);
    if (!palette)
        return 0;

    palette_pars.first_pen = start;
    palette_pars.count = count;
    palette_pars.requested = (short *)((long)values | 1);
    palette_pars.palette = palette;

    set_palette(vwk, &palette_pars);

    return (int)count;
}


int CDECL set_colour_table(Virtual *vwk, long subfunction, short *intin)
{
    COLOR_TAB *ctab;

    switch ((int)subfunction)
    {
    case 0:     /* vs_ctab */
        ctab = (COLOR_TAB *)intin;
        if (!itab_valid_palette(ctab))
            return 0;
        return set_col_table(vwk, ctab->no_colors, 0, ctab->colors);

    case 1:     /* vs_ctab_entry */
        {
            long space;
            memcpy(&space, intin + 1, sizeof(space));
            if (space != 0 && space != 1)
                return 0;
            return set_col_table(vwk, 1, intin[0], (COLOR_ENTRY *)(intin + 3));
        }

    case 2:     /* vs_dflt_ctab */
        {
            COLOR_TAB defaults;
            if (!ctab_default(&defaults, vwk->real_address->screen.mfdb.bitplanes))
                return 0;
            return set_col_table(vwk, defaults.no_colors, 0, defaults.colors);
        }

    default:
        PUTS("Unknown set colour table operation\n");
        return 0;
    }
}


int CDECL colour_table(Virtual *vwk, long subfunction, short *intin, short *intout)
{
    switch ((int)subfunction)
    {
    case 0:     /* vq_ctab */
        {
            long capacity, length = ctab_bytes(vwk->real_address->screen.palette.size);
            const COLOR_TAB *table;
            memcpy(&capacity, intin, sizeof(capacity));
            if (!length || capacity < length || !(table = ctab_current(vwk)))
                return 0;
            memcpy(intout, table, length);
            return length / 2;
        }

    case 1:     /* vq_ctab_entry */
        {
            const COLOR_TAB *table = ctab_current(vwk);
            long space = 0;
            memset(intout, 0, 12);
            if (table && intin[0] >= 0 && intin[0] < table->no_colors)
            {
                space = 1;
                memcpy(intout + 2, &table->colors[intin[0]], sizeof(COLOR_ENTRY));
            }
            memcpy(intout, &space, sizeof(space));
            return 6;
        }

    case 2:     /* vq_ctab_id */
        {
            const COLOR_TAB *table = ctab_current(vwk);
            long id = table ? table->map_id : 0;
            memcpy(intout, &id, sizeof(id));
            return 2;
        }

    case 3:     /* v_ctab_idx2vdi */
        intout[0] = idx2vdi(vwk->real_address, intin[0]);
        return 1;

    case 4:     /* v_ctab_vdi2idx */
        intout[0] = vdi2idx(vwk->real_address, intin[0]);
        return 1;

    case 5:     /* v_ctab_idx2value */
        {
            Workstation *wk = vwk->real_address;
            Colour *palette = vwk->palette;
            long index = intin[0];
            unsigned long value = 0;
            if (index < 0 || index >= wk->screen.palette.size)
                index = vdi2idx(wk, 1);
            if (wk->driver->device->clut == 1)
                value = index < 0 ? 0 : index;
            else
            {
                if (!palette || ((unsigned long)palette & 1)) palette = wk->screen.palette.colours;
                if (palette && index >= 0)
                {
                    if (wk->screen.mfdb.bitplanes <= 16)
                    {
                        unsigned short pixel;
                        memcpy(&pixel, &palette[index].real, sizeof(pixel));
                        value = pixel;
                    }
                    else value = palette[index].real;
                }
            }
            memcpy(intout, &value, sizeof(value));
            return 2;
        }

    case 6:     /* v_get_ctab_id */
        {
            long id = ctab_new_id();
            memcpy(intout, &id, sizeof(id));
            return 2;
        }

    case 7:     /* vq_dflt_ctab */
        {
            long capacity, bits = vwk->real_address->screen.mfdb.bitplanes;
            long length = ctab_bytes(bits > 8 ? 256 : bits > 0 ? 1L << bits : 0);
            memcpy(&capacity, intin, sizeof(capacity));
            if (!length || capacity < length || !ctab_default((COLOR_TAB *)intout, bits))
                return 0;
            return length / 2;
        }

    case 8:     /* v_create_ctab */
        {
            long space;
            unsigned long format;
            COLOR_TAB *table;
            memcpy(&space, intin, sizeof(space));
            memcpy(&format, intin + 2, sizeof(format));
            table = ctab_create(vwk, space, format);
            memcpy(intout, &table, sizeof(table));
            return 2;
        }

    case 9:     /* v_delete_ctab */
        {
            COLOR_TAB *table;
            memcpy(&table, intin, sizeof(table));
            intout[0] = ctab_delete(vwk, table);
            return 1;
        }

    default:
        PUTS("Unknown colour table operation\n");
        return 0;
    }
}


/* -1 wrong sub-function, 0 not done (colour space or memory), 1 done */
int CDECL lib_vs_x_color(Virtual *vwk, long subfunction, long colour_space, COLOR_ENTRY *values)
{
    XColours *node;

    if ((subfunction < 0) || (subfunction >= X_COLOURS))
        return -1;
    if ((colour_space != 0) && (colour_space != 1))     /* current or RGB */
        return 0;
    if ((node = x_colours_find(vwk, 1)) == NULL)
        return 0;

    node->colour[subfunction].rgb.reserved = 0;
    node->colour[subfunction].rgb.red = values->rgb.red;
    node->colour[subfunction].rgb.green = values->rgb.green;
    node->colour[subfunction].rgb.blue = values->rgb.blue;

    return 1;
}

/* Returns the colour space (1, RGB), or -1 for a wrong sub-function */
long CDECL lib_vq_x_color(Virtual *vwk, long subfunction, COLOR_ENTRY *colour)
{
    XColours *node;

    if ((subfunction < 0) || (subfunction >= X_COLOURS))
        return -1;

    node = x_colours_find(vwk, 0);
    if (node)
        colour->rgb = node->colour[subfunction].rgb;
    else
        colour->rgb = x_default[subfunction];

    return 1;
}
