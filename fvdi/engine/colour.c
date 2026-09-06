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

#define neg_pal_n  9


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
        addr = malloc((wk->screen.palette.size + neg_pal_n) * sizeof(Colour));
        if (!addr)
        {
            PUTS("Could not allocate space for palette!\n");
            return 0;
        } else
        {
            if (!palette)
            {
                /* No palette allocated? */
                palette = vwk->palette = (Colour *)(addr + neg_pal_n * sizeof(Colour));   /* Point to index 0 */
                memset(addr, 0, neg_pal_n * sizeof(Colour));
            } else
            {                           /* Only negative palette allocated so far? */
                palette = (Colour *)((long)palette & ~1);  /* Copy the negative side first and free it */
                vwk->palette = (Colour *)(addr + neg_pal_n * sizeof(Colour));
                copymem_aligned(palette - neg_pal_n, addr, neg_pal_n * sizeof(Colour));
                free(palette - neg_pal_n);
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


static int fg_bg_index(Virtual *vwk, int subfunction, short **fg, short **bg)
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
        /* This will be for bitmaps */
        return 0;

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

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg))
        return -1;
    index = *fg = -subfunction * 2 - 2;      /* Index -2/-4... */

    palette = vwk->palette;
    if (!palette)
    {
        addr = malloc(neg_pal_n * sizeof(Colour));
        if (!addr)
        {
            PUTS("Could not allocate space for negative palette!\n");
            return -1;
        }
        palette = vwk->palette = (Colour *)(((long)addr + neg_pal_n * sizeof(Colour)) | 1);   /* Point to index 0 */
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

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg))
        return -1;
    index = *bg = -subfunction * 2 - 3;      /* Index -3/-5... */

    palette = vwk->palette;
    if (!palette)
    {
        addr = malloc(neg_pal_n * sizeof(Colour));
        if (!addr)
        {
            PUTS("Could not allocate space for negative palette!\n");
            return -1;
        }
        palette = vwk->palette = (Colour *)(((long)addr + neg_pal_n * sizeof(Colour)) | 1);   /* Point to index 0 */
    }
    palette = (Colour *)((long)palette & ~1);

    palette_pars.first_pen = index;
    palette_pars.count = 1;             /* One colour to set up */
    palette_pars.requested = (short *)((long)values | 1);    /* Odd for new style entries */
    palette_pars.palette = palette;

    set_palette(vwk, &palette_pars);

    return 1;
}


long CDECL lib_vq_fg_color(Virtual *vwk, long subfunction, COLOR_ENTRY *colour)
{
    short *fg, *bg, index;
    Colour *palette;

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg))
        return -1;
    index = *fg;

    palette = vwk->palette;
    if (!palette || (((long)palette & 1) && (index >= 0)))   /* No or only part local? */
        palette = vwk->real_address->screen.palette.colours;
    palette = (Colour *)((long)palette & ~1);

    colour->rgb.reserved = 0;
    colour->rgb.red = palette[index].vdi.red;
    colour->rgb.green = palette[index].vdi.green;
    colour->rgb.blue = palette[index].vdi.blue;

    return 1;    /* RGB_SPACE */
}


long CDECL lib_vq_bg_color(Virtual *vwk, long subfunction, COLOR_ENTRY *colour)
{
    short *fg, *bg, index;
    Colour *palette;

    if (!fg_bg_index(vwk, (int)subfunction, &fg, &bg))
        return -1;
    index = *bg;

    palette = vwk->palette;
    if (!palette || (((long)palette & 1) && (index >= 0)))   /* No or only part local? */
        palette = vwk->real_address->screen.palette.colours;
    palette = (Colour *)((long)palette & ~1);

    colour->rgb.reserved = 0;
    colour->rgb.red = palette[index].vdi.red;
    colour->rgb.green = palette[index].vdi.green;
    colour->rgb.blue = palette[index].vdi.blue;

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
