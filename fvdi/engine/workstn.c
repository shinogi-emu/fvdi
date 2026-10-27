/*
 * fVDI workstation functions
 *
 * Copyright 2000/2003, Johan Klockars
 * This software is licensed under the GNU General Public License.
 * Please, see LICENSE.TXT for further information.
 */

#include "fvdi.h"
#include "stdio.h"
#include "function.h"
#include "relocate.h"
#include "utility.h"
#include "globals.h"
#include "ctab.h"



Virtual **handle_link = 0;

int lib_vq_extnd(Virtual *vwk, long subfunction, long flag, short *intout, short *ptsout);


void linea_setup(Workstation *wk)
{
    unsigned short *linea, width, height, bitplanes;
    static unsigned short linea_orig[12];
    static short init = 1;

    linea = wk->screen.linea;

    if (init)
    {
        init = 0;
        linea_orig[0] = linea[-0x2b4 / 2];  /* DEV_TAB[0] */
        linea_orig[1] = linea[-0x2b2 / 2];  /* DEV_TAB[1] */
        linea_orig[2] = linea[-0x00c / 2];  /* V_REZ_HZ */
        linea_orig[3] = linea[-0x004 / 2];  /* V_REZ_VT */
        linea_orig[4] = linea[-0x306 / 2];  /* INQ_TAB[4] */
        linea_orig[5] = linea[0];           /* PLANES */
        linea_orig[6] = linea[1];           /* WIDTH */
        linea_orig[7] = linea[-1];          /* BYTES_LIN */
        linea_orig[8] = linea[-0x304 / 2];  /* INQ_TAB[5] */
        linea_orig[9] = linea[-0x266 / 2];  /* DEV_TAB[39] */
        linea_orig[10] = linea[-0x26e / 2]; /* DEV_TAB[35] */
        linea_orig[11] = linea[-0x29a / 2]; /* DEV_TAB[13] */
    }

    /* Copy a few things into the lineA variables */
    width = wk->screen.mfdb.width;
    height = wk->screen.mfdb.height;
    linea[-0x2b4 / 2] = width - 1;    /* DEV_TAB[0] */
    linea[-0x2b2 / 2] = height - 1;   /* DEV_TAB[1] */
    linea[-0x00c / 2] = width;        /* V_REZ_HZ */
    linea[-0x004 / 2] = height;       /* V_REZ_VT */
    if (lineafix)
    {
        /* Should cover more really */
        bitplanes = wk->screen.mfdb.bitplanes;
        linea[-0x306 / 2] = bitplanes;  /* INQ_TAB[4] */
        linea[0] = bitplanes;
        width = (width >> 3) * bitplanes;
        linea[1] = width;   /* WIDTH */
        linea[-1] = width;  /* BYTES_LIN */
    } else if (!init)
    {
        linea[-0x306 / 2] = linea_orig[4];  /* INQ_TAB[4] */
        linea[0] = linea_orig[5];   /* PLANES */
        linea[1] = linea_orig[6];   /* WIDTH */
        linea[-1] = linea_orig[7];  /* BYTES_LIN */
    }

    linea[-0x304 / 2] = wk->screen.look_up_table;          /* INQ_TAB[5] */
    linea[-0x266 / 2] = wk->screen.palette.possibilities;  /* DEV_TAB[39] */
    linea[-0x26e / 2] = wk->screen.colour;                 /* DEV_TAB[35] */
    linea[-0x29a / 2] = wk->screen.palette.size;           /* DEV_TAB[13] */
}


/* Find virtual workstation entry for a handle */
static Virtual **find_handle_entry(short hnd)
{
    short handles;
    Virtual **link;

    handles = HANDLES;
    if (hnd < handles)
        return &handle[hnd];

    link = handle_link;
    while (link)
    {
        hnd -= handles;
        if (debug)
        {
            PUTS("Looking for handle in extra table\n");
        }
        handles = (long) link[-2];
        if (hnd < handles)
            return &link[hnd];
        link = (Virtual **) link[-1];
    }

    return 0;
}


/* Find (or create, if necessary) a free handle */
static short find_free_handle(Virtual ***handle_entry)
{
    short hnd, handles;
    Virtual ***link, ***last, **handle_table;

    handles = HANDLES;
    for (hnd = 1; hnd < handles; hnd++)
    {
        if (handle[hnd] == non_fvdi_vwk)
        {
            *handle_entry = &handle[hnd];
            return hnd;
        }
    }

    link = &handle_link;
    last = link;
    while ((handle_table = *link) != NULL)
    {
        if (debug)
        {
            PUTS("Looking for free handle in extra table\n");
        }
        handles += (long)handle_table[-2];
        for (; hnd < handles; hnd++)
        {
            if (handle_table[hnd] == non_fvdi_vwk)
            {
                *handle_entry = &handle[hnd];
                return hnd;
            }
        }
        last = link;
        link = (Virtual ***)&handle_table[-1];
    }

    handle_table = (Virtual **)malloc(64 * sizeof(Virtual *));
    if (handle_table)
    {
        handles = *(long *)handle_table / sizeof(Virtual *) - 2;
        if (debug)
        {
            PRINTF(("Allocated space for %d extra handles\n", handles));
        }
        handle_table[0] = (Virtual *)((long)handles);
        handle_table[1] = 0;
        for (handles--; handles >= 2; handles--)
            handle_table[handles] = non_fvdi_vwk;
        *last = &handle_table[2];
        *handle_entry = &handle_table[2];
        return hnd;
    }

    return 0;
}


/* Needs to deal with virtuals on non-screen workstations! */

/*
 * Off-screen bitmaps
 * ==================
 *
 * A bitmap workstation is one allocation: its Virtual, 32 bytes of user
 * fill pattern, its own copy of the Workstation it was opened from, and,
 * unless the caller supplied the pixels, the pixels themselves.
 *
 * Two things can come to live outside that block, and are remembered here
 * so v_clsvwk can free them: the pixels of a bitmap v_resize_bm gave a new
 * size, and the colour table of a v_open_bm bitmap asked to have its own.
 */
typedef struct BitmapExtra {
    struct BitmapExtra *next;
    Virtual *owner;
    void *pixels;               /* from v_resize_bm, or NULL */
    Colour *palette;            /* own colour table, or NULL */
} BitmapExtra;

static BitmapExtra *bitmap_extras = 0;


static BitmapExtra *bitmap_extra(Virtual *vwk, int create)
{
    BitmapExtra *node;

    for (node = bitmap_extras; node; node = node->next)
    {
        if (node->owner == vwk)
            return node;
    }
    if (!create || (node = (BitmapExtra *)malloc(sizeof(BitmapExtra))) == NULL)
        return 0;
    node->owner = vwk;
    node->pixels = 0;
    node->palette = 0;
    node->next = bitmap_extras;
    bitmap_extras = node;

    return node;
}


static void bitmap_extra_close(Virtual *vwk)
{
    BitmapExtra **link = &bitmap_extras;

    while (*link)
    {
        BitmapExtra *node = *link;

        if (node->owner == vwk)
        {
            *link = node->next;
            if (node->pixels)
                free(node->pixels);
            if (node->palette)
                free(node->palette);
            free(node);
        } else
            link = &node->next;
    }
}


/* A bitmap workstation's Workstation follows its Virtual and fill pattern */
static int is_bitmap_vwk(Virtual *vwk)
{
    return vwk->real_address == (Workstation *)((long)vwk + sizeof(Virtual) + 32);
}


/*
 * Build an off-screen bitmap workstation modelled on wk: width x height
 * pixels (the width rounded up to 16), bitplanes deep.  'pixels' is the
 * caller's memory, or NULL for a cleared area allocated with the
 * workstation; 'wrap' is the bytes in a line, 0 for the least that fits.
 * The pixel size is in microns.  Returns the Virtual, or NULL.
 */
static Virtual *new_bitmap_vwk(Workstation *wk, short width, short height, short bitplanes,
                               short *pixels, long wrap, short pixel_width, short pixel_height)
{
    long extra_size = 32 + sizeof(Virtual) + sizeof(Workstation);
    long size;
    Virtual *new_vwk;
    Workstation *new_wk;

    width = (width + 15) & 0xfff0;
    if (!wrap)
        wrap = (long)(width >> 3) * bitplanes;	/* >> 4 in assembly file */
    size = wrap * height;

    if (!pixels)
        extra_size += size;

    /* New vwk, but it should really not always be for this driver! */
    if ((new_vwk = malloc(extra_size)) == NULL)
    {
        PRINTF(("v_opnbm: out of memory (%ld)\n", extra_size));
        return NULL;
    }

    new_wk = (Workstation *)((long)new_vwk + sizeof(Virtual) + 32);
    copymem(wk, new_wk, sizeof(Workstation));
    copymem(wk->driver->default_vwk, new_vwk, sizeof(Virtual));

    if (!pixels)
    {
        pixels = (short *)((long)new_wk + sizeof(Workstation));
        memset(pixels, bitplanes > 8 ? -1 : 0, size);
    }

    new_wk->screen.mfdb.address = pixels;
    new_wk->screen.mfdb.width = width;
    new_wk->screen.mfdb.height = height;
    new_wk->screen.mfdb.wdwidth = wrap / (2 * bitplanes);	/* width >> 4 when wrap is the least */
    new_wk->screen.mfdb.bitplanes = bitplanes;
    new_wk->screen.mfdb.reserved[0] = 0;
    new_wk->screen.mfdb.reserved[1] = 0;
    new_wk->screen.mfdb.reserved[2] = 0;
    new_wk->screen.mfdb.standard = 0;

    new_wk->screen.type = 0;
    new_wk->screen.wrap = wrap;
    new_wk->screen.shadow.buffer = 0;
    new_wk->screen.shadow.address = 0;
    new_wk->screen.shadow.wrap = 0;

    new_wk->screen.pixel.width = pixel_width;
    new_wk->screen.pixel.height = pixel_height;

    new_wk->screen.coordinates.course = 0;	/* ? */
    new_wk->screen.coordinates.min_x = 0;
    new_wk->screen.coordinates.min_y = 0;
    new_wk->screen.coordinates.max_x = width - 1;
    new_wk->screen.coordinates.max_y = height - 1;

    /* Probably OK to mark all these as unavailable */
    new_wk->various.input_type = 0;
    new_wk->various.inking = 0;
    new_wk->various.buttons = 0;
    new_wk->various.cursor_movement = 0;
    new_wk->various.number_entry = 0;
    new_wk->various.selection = 0;
    new_wk->various.typing = 0;
    new_wk->various.workstation_type = 0;
    new_wk->mouse.type = 0;				/* Enough? */

    new_vwk->real_address = new_wk;

    return new_vwk;
}


static void open_vwk_finish(Virtual *vwk, Workstation *wk, short hnd, Virtual **handle_entry, VDIpars *pars);


/*
 * v_open_bm (100, sub 3): NVDI's off-screen bitmap from a GCBITMAP.
 *
 * The bitmap is in the format of the screen, as NVDI's are; a GCBITMAP
 * asking for another pixel format or depth is refused.  With no GCBITMAP,
 * or an empty one, the bitmap is the size of base_handle's screen and
 * fVDI allocates it; an empty GCBITMAP is then filled in.
 *
 * intin[0] bit 0 set shares base_handle's colour table, clear gives the
 * bitmap a copy of its own; intin[1..3] are the pixel size and its unit.
 * The only result is the handle, in contrl[6] - intout is one word long,
 * so the workstation information v_opnvwk returns goes nowhere.
 */
static void v_open_bm(Virtual *vwk, VDIpars *pars)
{
    static short work_in[11] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2 };
    short intout[57], ptsout[12];
    Control control;
    VDIpars inner;
    GCBITMAP *bm;
    Workstation *wk = vwk->real_address;
    Virtual *new_vwk, **handle_entry;
    short bits, colour_flags, unit, hnd;
    long width, height, wrap = 0, size, pixel_width, pixel_height;
    unsigned long format;
    short *pixels = NULL;

    pars->control->handle = 0;          /* Assume failure */
    if (pars->control->l_intin < 4)
        return;
    bm = (GCBITMAP *)pars->control->addr1;
    colour_flags = pars->intin[0];
    if (colour_flags & ~1)              /* Only bit 0 is defined */
        return;

    bits = wk->screen.mfdb.bitplanes;
    format = ctab_pixel_format(vwk);
    width = wk->screen.mfdb.width;
    height = wk->screen.mfdb.height;
    if (bm)
    {
        if (bm->px_format && bm->px_format != format)
            return;
        if (bm->bits && bm->bits != bits)
            return;
        if (bm->addr)
        {
            width = bm->xmax - bm->xmin;
            height = bm->ymax - bm->ymin;
            if ((width <= 0) || (height <= 0) ||
                (bm->width < (((width + 15) & ~15L) >> 3) * bits) || (bm->width & 1))
                return;
            pixels = (short *)bm->addr;
            wrap = bm->width;
        } else if (bm->xmin || bm->ymin || bm->xmax || bm->ymax)
        {
            width = bm->xmax - bm->xmin;
            height = bm->ymax - bm->ymin;
            if ((width <= 0) || (height <= 0))
                return;
        }
    }
    if ((width > 0x7fff - 15) || (height > 0x7fff))
        return;

    /* The pixel size, in microns; 0 takes base_handle's */
    unit = pars->intin[1];
    pixel_width = pars->intin[2];
    pixel_height = pars->intin[3];
    if ((pixel_width > 0) && (pixel_height > 0))
    {
        switch (unit)
        {
        case -1:                        /* dpi */
            pixel_width = (25400L + pixel_width / 2) / pixel_width;
            pixel_height = (25400L + pixel_height / 2) / pixel_height;
            break;
        case 0:                         /* microns */
            break;
        case 1:                         /* 1/10 micron */
        case 2:                         /* 1/100 */
        case 3:                         /* 1/1000 */
            size = unit == 1 ? 10 : unit == 2 ? 100 : 1000;
            pixel_width = (pixel_width + size / 2) / size;
            pixel_height = (pixel_height + size / 2) / size;
            break;
        default:
            return;
        }
    }
    if ((pixel_width <= 0) || (pixel_height <= 0))
    {
        pixel_width = wk->screen.pixel.width;
        pixel_height = wk->screen.pixel.height;
    }

    if ((hnd = find_free_handle(&handle_entry)) == 0)
        return;
    new_vwk = new_bitmap_vwk(wk, (short)width, (short)height, bits, pixels, wrap,
                             (short)pixel_width, (short)pixel_height);
    if (!new_vwk)
        return;

    if (!(colour_flags & 1))
    {
        /* A colour table of its own: what the screen does later is not its business */
        Workstation *new_wk = new_vwk->real_address;
        BitmapExtra *extra = bitmap_extra(new_vwk, 1);
        long n = wk->screen.palette.size;
        Colour *palette = n > 0 ? (Colour *)malloc(n * sizeof(Colour)) : NULL;

        if (!extra || (n > 0 && !palette))
        {
            if (palette)
                free(palette);
            bitmap_extra_close(new_vwk);
            free(new_vwk);
            return;
        }
        if (palette)
        {
            copymem(wk->screen.palette.colours, palette, n * sizeof(Colour));
            new_wk->screen.palette.colours = palette;
            extra->palette = palette;
        }
    }

    control = *pars->control;
    control.l_intin = 11;
    inner.control = &control;
    inner.intin = work_in;
    inner.ptsin = pars->ptsin;
    inner.intout = intout;
    inner.ptsout = ptsout;
    open_vwk_finish(new_vwk, new_vwk->real_address, hnd, handle_entry, &inner);
    pars->control->handle = control.handle;

    if (bm && !bm->addr)
    {
        /* fVDI allocated it: describe what was made */
        Workstation *new_wk = new_vwk->real_address;

        bm->magic = 0x6362746dL;        /* 'cbtm' */
        bm->length = sizeof(GCBITMAP);
        bm->format = 0;
        bm->reserved = 0;
        bm->addr = (unsigned char *)new_wk->screen.mfdb.address;
        bm->width = new_wk->screen.wrap;
        bm->bits = bits;
        bm->px_format = format;
        bm->xmin = 0;
        bm->ymin = 0;
        bm->xmax = new_wk->screen.mfdb.width;
        bm->ymax = new_wk->screen.mfdb.height;
        bm->ctab = 0;
        bm->itab = 0;
        bm->reserved0 = 0;
        bm->reserved1 = 0;
    }
}


/*
 * v_resize_bm (100, sub 2): give a bitmap from v_opnbm or v_open_bm a new
 * size, in new pixels - fVDI's, or the caller's at intin[4..5] with
 * intin[2..3] bytes to a line.  intout[0] is 1 when done, 0 when not.
 *
 * NVDI releases the old pixels first, so a failure leaves the bitmap
 * without any.  fVDI only lets go of the old ones once the new ones are
 * in place, so a failure leaves the bitmap as it was.
 */
static void v_resize_bm(Virtual *vwk, VDIpars *pars)
{
    Workstation *wk = vwk->real_address;
    BitmapExtra *extra;
    short width, height, bitplanes;
    long wrap, least, size;
    short *pixels;
    void *block = NULL;

    pars->intout[0] = 0;
    if (!is_bitmap_vwk(vwk) || (pars->control->l_intin < 4))
        return;
    width = pars->intin[0];
    height = pars->intin[1];
    wrap = pars->control->l_intin >= 6 ? *(long *)&pars->intin[2] : 0;
    pixels = pars->control->l_intin >= 6 ? *(short **)&pars->intin[4] : NULL;
    if ((width <= 0) || (height <= 0) || (width > 0x7fff - 15))
        return;

    bitplanes = wk->screen.mfdb.bitplanes;
    width = (width + 15) & 0xfff0;
    least = (long)(width >> 3) * bitplanes;
    if (pixels)
    {
        if ((wrap < least) || (wrap & 1))
            return;
    } else
    {
        /* NVDI's rule for caller memory: a 16-byte boundary, 16 bytes spare each side */
        wrap = least;
        size = wrap * height;
        if ((block = malloc(size + 48)) == NULL)
            return;
        pixels = (short *)(((long)block + 16 + 15) & ~15L);
        memset(pixels, bitplanes > 8 ? -1 : 0, size);
    }

    if ((extra = bitmap_extra(vwk, 1)) == NULL)
    {
        if (block)
            free(block);
        return;
    }
    if (extra->pixels)
        free(extra->pixels);
    extra->pixels = block;

    wk->screen.mfdb.address = pixels;
    wk->screen.mfdb.width = width;
    wk->screen.mfdb.height = height;
    wk->screen.mfdb.wdwidth = wrap / (2 * bitplanes);
    wk->screen.wrap = wrap;
    wk->screen.coordinates.max_x = width - 1;
    wk->screen.coordinates.max_y = height - 1;
    lib_vs_clip(vwk, vwk->clip.on, &vwk->clip.rectangle.x1);   /* within the new size */

    pars->intout[0] = 1;
}


void CDECL v_opnvwk(Virtual *vwk, VDIpars *pars)
{
    short *intin;
    short hnd, width, height, bitplanes;
    long extra_size;
    Workstation *wk, *new_wk;
    Virtual *new_vwk, **handle_entry;
    MFDB *mfdb;
    long colors;
    short format;
    short bit_order;

    switch (pars->control->subfunction)
    {
    case 2:
        v_resize_bm(vwk, pars);
        return;
    case 3:
        v_open_bm(vwk, pars);
        return;
    }

    pars->control->handle = 0;	/* Assume failure */
    if ((hnd = find_free_handle(&handle_entry)) == 0)
    {
        PRINTF(("v_opnvwk: no free handle\n"));
        return;
    }

    wk = vwk->real_address;
    /* 32 - user fill pattern */
    extra_size = 32;
    extra_size += sizeof(*vwk);

    /* Check if really v_opnbm */
    if (pars->control->subfunction != 1 || pars->control->l_intin < 20)
    {
        if ((new_vwk = malloc(extra_size)) == NULL)
        {
            PRINTF(("v_opnbm[v_opnvwk]: out of memory\n"));
            return;
        }
        copymem(wk->driver->default_vwk, new_vwk, sizeof(Virtual));
        vwk = new_vwk;
    } else
    {
        mfdb = (MFDB *)pars->control->addr1;
        intin = pars->intin;

        if (!mfdb)
        {
            PRINTF(("v_opnbm: NULL mfdb\n"));
            return;
        }
        /* Doesn't allow the EdDI v1.1 variant yet */
        colors = *((long *)&intin[15]);
        if (colors != 0 && colors != 2 && colors != (1L << wk->screen.mfdb.bitplanes))
        {
            PRINTF(("v_opnbm: unsupported colors %ld\n", colors));
            return;
        }
        bitplanes = mfdb->bitplanes;
        if (bitplanes == 0)
            bitplanes = wk->screen.mfdb.bitplanes;
        /* Only same as physical or one allowed */
        if (bitplanes != 1 && bitplanes != wk->screen.mfdb.bitplanes)
        {
            PRINTF(("v_opnbm: unsupported planes %d\n", bitplanes));
            return;
        }
        format = intin[18];
        if (format != 0 && format != 1)
        {
            PRINTF(("v_opnbm: unsupported format %d\n", format));
            return;
        }
        bit_order = intin[19];
        if (bit_order != 0 && bit_order != 1)
        {
            PRINTF(("v_opnbm: unsupported bit order %d\n", bit_order));
            return;
        }
        
        if (mfdb->address || intin[11] || intin[12])
        {
            width = intin[11] ? (intin[11] + 1) : wk->screen.mfdb.width;
            height = intin[12] ? (intin[12] + 1) : wk->screen.mfdb.height;
        } else
        {
            width = wk->screen.mfdb.width;
            height = wk->screen.mfdb.height;
        }

        new_vwk = new_bitmap_vwk(wk, width, height, bitplanes, mfdb->address, 0, intin[13], intin[14]);
        if (!new_vwk)
            return;
        new_wk = new_vwk->real_address;

        if (!mfdb->address)
        {
            mfdb->standard = 0;
            mfdb->address = new_wk->screen.mfdb.address;
        }
        mfdb->width = new_wk->screen.mfdb.width;
        mfdb->height = new_wk->screen.mfdb.height;
        mfdb->wdwidth = new_wk->screen.mfdb.wdwidth;
        mfdb->bitplanes = bitplanes;
        mfdb->reserved[0] = 0;
        mfdb->reserved[1] = 0;
        mfdb->reserved[2] = 0;
        if (mfdb->standard)	/* Need to convert input MFDB to device dependent format? */
            lib_vdi_pp(lib_vr_trnfm, new_vwk, mfdb, mfdb);

        vwk = new_vwk;
        wk = new_wk;
    }

    open_vwk_finish(vwk, wk, hnd, handle_entry, pars);
}


/* What every new workstation gets: ordinary or bitmap, from work_in */
static void open_vwk_finish(Virtual *vwk, Workstation *wk, short hnd, Virtual **handle_entry, VDIpars *pars)
{
    unsigned short c;
    short dummy;

    vwk->fill.user.pattern.in_use = (short *)((long)vwk + sizeof(Virtual));
    vwk->fill.user.pattern.extra = 0;
    vwk->fill.user.multiplane = 0;
    x_colours_close(vwk);   /* nothing left from a vwk that had this address */

    /* Return information about workstation */
    lib_vq_extnd(vwk, 0, 0, pars->intout, pars->ptsout);

    pars->control->handle = vwk->standard_handle = hnd;
    *handle_entry = vwk;

    /* Call various setup functions (most with supplied data) */
    c = pars->intin[1];
    if (c < 1 || c > wk->drawing.line.types)
        c = 1;
    vwk->line.type = c;
    c = pars->intin[2];
    if (c >= wk->screen.palette.size)
        c = BLACK;
    vwk->line.colour.foreground = c;
    c = pars->intin[3];
    if (c < 1 || c > wk->drawing.marker.types)
        c = 3; /* Asterisk */
    vwk->marker.type = c;
    c = pars->intin[4];
    if (c >= wk->screen.palette.size)
        c = BLACK;
    vwk->marker.colour.foreground = c;
    lib_vst_font(vwk, pars->intin[5]);
    /* Default to 10 or 9 point font */
    lib_vst_point(vwk, wk->screen.mfdb.height >= 400 ? 10 : 9, &dummy, &dummy, &dummy, &dummy);
    c = pars->intin[6];
    if (c >= wk->screen.palette.size)
        c = BLACK;
    vwk->text.colour.foreground = c;
    c = pars->intin[7];
    if (c > 4)
        c = 0; /* Hollow */
    vwk->fill.interior = c;
    c = pars->intin[8];
    if (c < 1 || c > 24)
        c = 1;
    vwk->fill.style = c;
    c = pars->intin[9];
    if (c >= wk->screen.palette.size)
        c = BLACK;
    vwk->fill.colour.foreground = c;
    lib_vs_clip(vwk, 0, NULL);  /* No clipping (set to max size) */
    /* Should also take care of the coordinate values that come now */
}


void CDECL v_opnwk(VDIpars *pars)
{
    Driver *driver;
    Virtual *vwk, **handle_entry;
    Workstation *wk;
    unsigned short hnd, oldhnd;

    /* For now, just assume that any
     * workstation handle >10 is non-fVDI
     */
    if (pars->intin[0] > LAST_SCREEN_DEVICE)
    {
        int failed = 1;

        pars->control->handle = 0;	/* Assume failure */
        vwk = 0;
        if (old_gdos)
        {
            /* No pass-through without old GDOS */
            if ((hnd = find_free_handle(&handle_entry)) != 0)
            {
                if ((vwk = malloc(6)) != NULL)
                {
                    if ((oldhnd = call_other(pars, 0)) != 0)
                    {
                        /* Dummy handle for call */
                        failed = 0;
                        vwk->real_address = non_fvdi_wk;
                        /* Mark as pass-through handle */
                        vwk->standard_handle = oldhnd | 0x8000;
                        *handle_entry = vwk;
                        pars->control->handle = hnd;
                    } else
                    {
                        PRINTF(("call_other failed (%d)\n", pars->intin[0]));
                    }
                } else
                {
                    PUTS("malloc failed\n");
                }
            } else
            {
                PUTS("find_free_handle failed\n");
            }
        } else
        {
            PUTS("no old GDOS\n");
        }

        if (failed)
        {
            if (vwk)
                free(vwk);		/* Couldn't open */
        }

        return;
    }

#if 0
    /* Experimenting, 001217/010109 */
    if (!old_wk_handle && !stand_alone)
    {
        short intout[45], ptsout[12];

        old_wk_handle = scall_v_opnwk(1, intout, ptsout);
    }
#endif

    driver = (Driver *)driver_list->value;
    if ((vwk = driver->opnwk(default_virtual)) != NULL)
        ;				/* Should probably do something */
    else
        vwk = driver->default_vwk;

    /* To accomodate mouse drawing (only for one screen wk) */
    screen_wk = wk = vwk->real_address;

    linea_setup(wk);
    init_interrupts();

    v_opnvwk(vwk, pars);

    old_wk_handle = pars->control->handle;

    screen_vwk = *find_handle_entry(pars->control->handle);
}


void CDECL v_clsvwk(Virtual *vwk, VDIpars *pars)
{
    Virtual **handle_entry;
    short hnd;

    hnd = vwk->standard_handle;
    if (hnd == 0)	/* Check if default VDI structure */
        return;
    if (hnd < 0)
    {
        call_other(pars, hnd & 0x7fff);
        hnd = pars->control->handle;
    } else
    {
        if (vwk->fill.user.pattern.extra)
            free(vwk->fill.user.pattern.extra);
        if (vwk->palette != NULL)
            free((void *)(((long)vwk->palette & ~1) - NEG_PAL_N * sizeof(Colour)));
    }

    if (vwk->text.current_font)
        vwk->text.current_font->extra.ref_count--; /* Allow the font to be freed if appropriate */
    itab_close(vwk);
    ctab_close(vwk);
    x_colours_close(vwk);
    bitmap_extra_close(vwk);
    free(vwk);	/* This will work for off-screen bitmaps too, fortunately */

    /* Reset VDI structure address to default */
    handle_entry = find_handle_entry(hnd);
    if (!handle_entry)
    {
        PUTS("Handle could not be found in tables!\n");
    } else if (*handle_entry != vwk)
    {
        PUTS("Wrong handle entry!\n");
    } else
    {
        *handle_entry = non_fvdi_vwk;
    }
}


/* Needs to be able to deal with multiple fVDI workstations! */
void CDECL v_clswk(Virtual *vwk, VDIpars *pars)
{
    Driver *driver;
    Workstation *wk;

    wk = vwk->real_address;
    v_clsvwk(vwk, pars);

    if (wk != non_fvdi_wk && (driver = wk->driver) != NULL && driver->module.id <= LAST_SCREEN_DEVICE)
    {
        screen_wk = 0;

        driver->clswk(vwk);

#if 0
        if (old_wk_handle && !stand_alone)
            scall_v_clswk(old_wk_handle);
#endif
        reset_interrupts();
    }
}


/* Copy at most size - 1 characters and always terminate */
static void copy_limited(const char *from, char *to, long size)
{
    if (!to)
        return;
    while (from && *from && --size > 0)
        *to++ = *from++;
    *to = 0;
}


/*
 * vq_ext_devinfo (248 sub 4242): for a device number, whether a driver
 * exists (intout[0]) and is open (intout[1]), and the folder, file and
 * name of the driver, into the three buffers whose addresses follow the
 * device number in intin.
 *
 * fVDI answers for its own drivers, the screen devices: the folder it
 * loaded them from, the file named in FVDI.SYS and the name the driver
 * gives itself.  The buffers' sizes are not part of the call, so the
 * strings are kept to what NVDI's are: a 128-byte path, an 8.3 file name
 * and a 32-byte device name.  Other devices belong to the GDOS below, if
 * there is one.
 */
static void vq_ext_devinfo(VDIpars *pars)
{
    short device = pars->intin[0];
    char *file_path = *(char **)&pars->intin[1];
    char *file_name = *(char **)&pars->intin[3];
    char *name = *(char **)&pars->intin[5];
    List *element;
    Driver *driver = 0;

    pars->control->l_intout = 2;
    pars->intout[0] = 0;
    pars->intout[1] = 0;
    if (pars->control->l_intin < 7)
        return;

    if (device > LAST_SCREEN_DEVICE)
    {
        if (old_gdos)
            call_other(pars, 0);
        return;
    }

    for (element = driver_list; element; element = element->next)
    {
        if (((Driver *)element->value)->module.id == device)
        {
            driver = (Driver *)element->value;
            break;
        }
    }
    if (!driver)
        return;

    /*
     * module.file_name is the path fVDI opened the driver by, such as
     * gemsys\16_bit.sys: split it into folder and file.  Without a drive
     * the folder is relative to the root of the boot drive, where fVDI
     * looked from the AUTO folder, so name that drive.
     */
    {
        const char *full = driver->module.file_name;
        const char *file = full, *p;
        char dir[130];
        long n = 0, len;

        for (p = full; *p; p++)
        {
            if ((*p == '\\') || (*p == '/') || (*p == ':'))
                file = p + 1;
        }
        len = file - full;
        if (!len)
        {
            full = fvdi_driver_path();
            len = length(full);
        }
        if ((len < 2) || (full[1] != ':'))
        {
            dir[n++] = 'A' + *(volatile short *)0x446L;    /* _bootdev */
            dir[n++] = ':';
            if (!len || (full[0] != '\\'))
                dir[n++] = '\\';
        }
        while ((len-- > 0) && (n < (long)sizeof(dir) - 1))
            dir[n++] = *full++;
        dir[n] = 0;

        copy_limited(dir, file_path, 128);
        copy_limited(file, file_name, 13);
    }
    copy_limited(driver->module.name, name, 32);
    pars->intout[0] = 1;                /* exists */
    pars->intout[1] = 1;                /* screen drivers are open from boot */
}


void CDECL vq_devinfo(VDIpars *pars)
{
    if (pars->control->subfunction == 4242)
    {
        vq_ext_devinfo(pars);
        return;
    }

    /* For now, just assume that any
     * workstation handle >10 is non-fVDI
     */
    if (pars->intin[0] > LAST_SCREEN_DEVICE)
    {
        if (old_gdos)
        {
            /* No pass-through without old GDOS */
            call_other(pars, 0);	/* Dummy handle for call */
#ifdef FVDI_DEBUG
            {
                int i;
                char buf[2];
                
                PUTS("vq_devinfo:\n");
                display_output(pars);
                buf[1] = '\0';
                PUTS("filename: ");
                for (i = 0; i < pars->control->l_intout; i++)
                {
                    buf[0] = pars->intout[i];
                    PUTS(buf);
                }
                PUTS("\n");
                PUTS("device name: ");
                for (i = 1; i < pars->control->l_ptsout; i++)
                {
                    buf[0] = pars->ptsout[i];
                    PUTS(buf);
                }
                PUTS("\n");
            }
#endif
        } else
        {
            PUTS("no old GDOS (vq_devinfo)\n");
        }
        return;
    }

    pars->intout[0] = 0;
    pars->ptsout[0] = 1;
    pars->ptsout[1] = 0;
    pars->control->l_intout = 0;
    pars->control->l_ptsout = 1;
    PUTS("fVDI handles currently don't support vq_devinfo\n");
}


int lib_vq_extnd(Virtual *vwk, long subfunction, long flag, short *intout, short *ptsout)
{
    Workstation *wk = vwk->real_address;

    if (flag == 2 && subfunction == 1)
    {
        copymem_aligned(wk->driver->device, intout, sizeof(Device));
        ((Device *)intout)->address = wk->screen.mfdb.address;
        ((Device *)intout)->byte_width = wk->screen.wrap;
        return 1;
    } else if (flag)
    {
        *intout++ = wk->screen.type;
        *intout++ = wk->screen.bkg_colours;
        *intout++ = wk->writing.effects;
        *intout++ = wk->raster.scaling;
        *intout++ = wk->screen.mfdb.bitplanes;
        *intout++ = wk->screen.look_up_table;
        *intout++ = wk->raster.performance;
        *intout++ = wk->drawing.flood_fill;
        *intout++ = wk->writing.rotation.type;
        *intout++ = wk->drawing.writing_modes;
        *intout++ = wk->various.input_type;
        *intout++ = wk->writing.justification;
        *intout++ = wk->various.inking;
        *intout++ = wk->drawing.rubber_banding;
        *intout++ = wk->various.max_ptsin;
        *intout++ = wk->various.max_intin;
        *intout++ = wk->various.buttons;
        *intout++ = wk->drawing.line.wide.types_possible;
        *intout++ = wk->drawing.line.wide.writing_modes;
        *intout++ = vwk->clip.on;  /* 19 - originally reserved from here on */
        *intout++ = 0;   /* No pixel sizes below (1 - 0.1 um, 2 - 0.01, 3 - 0.001 */
        *intout++ = 0;   /* Pixel width */
        *intout++ = 0;   /* Pixel height */
        *intout++ = 0;   /* Horizontal dpi */
        *intout++ = 0;   /* Vertical dpi */
        *intout++ = 0;   /* Bit image rotation on printer (PC-GEM/3) */
        *intout++ = 0;   /* AES buffer address (PC-GEM/3) */
        *intout++ = 0;   /*         -    "    -           */
        *intout++ = 2;   /* Beziers! */
        *intout++ = 0;   /* Unused */
        *intout++ = 4;   /* 1 - bitmap scale, 2 - new raster functions, 4 - vr_clip_rects_xxx */
        *intout++ = 0;   /* 9 from here used to be reserved */
        *intout++ = 1;   /* New style colour routines (at least some of them) */
        *intout++ = 0;
        *intout++ = 0;
        *intout++ = 0;
        *intout++ = 0;
        *intout++ = 0;
        *intout++ = 0;
        *intout++ = 0;
        *intout++ = 0;   /* Unusable left border */
        *intout++ = 0;   /*          upper       */
        *intout++ = 0;   /*          right       */
        *intout++ = 0;   /*          lower       */
        *intout++ = 0;   /* Paper format (default/A3/A4/A5/B5/letter/half/legal/double/broad */

        *ptsout++ = vwk->clip.rectangle.x1;
        *ptsout++ = vwk->clip.rectangle.y1;
        *ptsout++ = vwk->clip.rectangle.x2;
        *ptsout++ = vwk->clip.rectangle.y2;
        *ptsout++ = 0;   /* Reserved from here on */
        *ptsout++ = 0;
        *ptsout++ = 0;
        *ptsout++ = 0;
        *ptsout++ = 0;
        *ptsout++ = 0;
        *ptsout++ = 0;
        *ptsout++ = 0;

        return 0;
    } else
    {
        long attributes;
        int i, n;

        *intout++ = wk->screen.coordinates.max_x;
        *intout++ = wk->screen.coordinates.max_y;
        *intout++ = wk->screen.coordinates.course;
        *intout++ = wk->screen.pixel.width;
        *intout++ = wk->screen.pixel.height;
        *intout++ = wk->writing.size.possibilities;
        *intout++ = wk->drawing.line.types;
        *intout++ = wk->drawing.line.wide.width.possibilities;
        *intout++ = wk->drawing.marker.types;
        *intout++ = wk->drawing.marker.size.possibilities;
        *intout++ = 1;     /* Fonts */
        *intout++ = wk->drawing.fill.patterns;
        *intout++ = wk->drawing.fill.hatches;
        *intout++ = wk->screen.palette.size;
        *intout++ = wk->drawing.primitives.supported;

        attributes = wk->drawing.primitives.attributes;
        n = 0;
        for (i = 9; i >= 0; i--)
        {
            if (attributes & 0x07)
            {
                n++;
                intout[i] = 10 - i;
                intout[i + 10] = (attributes & 0x07) - 1;
            }
            attributes >>= 3;
        }
        if (n < 10)
            intout[i + n] = -1;
        intout += 2 * 10;

        *intout++ = wk->screen.colour;
        *intout++ = wk->writing.rotation.possible;
        *intout++ = wk->drawing.fill.possible;
        *intout++ = wk->drawing.cellarray.available;
        *intout++ = wk->screen.palette.possibilities;
        *intout++ = wk->various.cursor_movement;
        *intout++ = wk->various.number_entry;
        *intout++ = wk->various.selection;
        *intout++ = wk->various.typing;
        *intout++ = wk->various.workstation_type;

        *ptsout++ = wk->writing.size.width.min;
        *ptsout++ = wk->writing.size.height.min;
        *ptsout++ = wk->writing.size.width.max;
        *ptsout++ = wk->writing.size.height.max;
        *ptsout++ = wk->drawing.line.wide.width.min;
        *ptsout++ = 0;
        *ptsout++ = wk->drawing.line.wide.width.max;
        *ptsout++ = 0;
        *ptsout++ = wk->drawing.marker.size.width.min;
        *ptsout++ = wk->drawing.marker.size.height.min;
        *ptsout++ = wk->drawing.marker.size.width.max;
        *ptsout++ = wk->drawing.marker.size.height.max;
        return 0;
    }
}


void lib_vs_clip(Virtual *vwk, short on, short *rect)
{
    Workstation *wk = vwk->real_address;

    vwk->clip.on = on;
    
    if (on)
    {
        short x;
        
        x = *rect++;
        if (x < 0)
            x = 0;
        vwk->clip.rectangle.x1 = x;
        x = *rect++;
        if (x < 0)
            x = 0;
        vwk->clip.rectangle.y1 = x;
        x = *rect++;
        if (x >= wk->screen.coordinates.max_x)
            x = wk->screen.coordinates.max_x;
        vwk->clip.rectangle.x2 = x;
        x = *rect++;
        if (x >= wk->screen.coordinates.max_y)
            x = wk->screen.coordinates.max_y;
        vwk->clip.rectangle.y2 = x;
    } else
    {
        vwk->clip.rectangle.x1 = vwk->clip.rectangle.y1 = 0;
        vwk->clip.rectangle.x2 = wk->screen.coordinates.max_x;
        vwk->clip.rectangle.y2 = wk->screen.coordinates.max_y;
    }
}
