/*
 * palette.c - Palette handling routines
 *
 * Copyright 2005, Johan Klockars
 *
 * This software is licensed under the GNU General Public License.
 * Please, see LICENSE.TXT for further information.
 */


#include "fvdi.h"
#include "driver.h"
#include "bitplane.h"
#include "os.h"
#ifdef __GNUC__
#include <mint/falcon.h>
#endif


long CDECL x_get_colour(Workstation *wk, long colour)
{
    static signed char tos_colours[] = { 0, -1, 1, 2, 4, 6, 3, 5, 7, 8, 9, 10, 12, 14, 11, 13 };
    int ret;

    colour &= 0x00ff;
    if (colour == 255)
        ret = 15;
    else if (colour >= 16)
        ret = colour;
    else
    {
        ret = tos_colours[colour];
        if (ret < 0)
            ret = wk->screen.palette.size - 1;
    }

    return ret;
}


long CDECL c_get_colour(Virtual *vwk, long colour)
{
    return x_get_colour(vwk->real_address, colour);
}


void CDECL c_get_colours(Virtual *vwk, long colour, unsigned long *foreground, unsigned long *background)
{
    *foreground = x_get_colour(vwk->real_address, colour & 0xffff);
    *background = x_get_colour(vwk->real_address, (colour >> 16) & 0xffff);
}


void CDECL x_get_colours(Workstation *wk, long colour, short *foreground, short *background)
{
    *foreground = x_get_colour(wk, colour & 0xffff);
    *background = x_get_colour(wk, (colour >> 16) & 0xffff);
}


void CDECL c_set_colours(Virtual *vwk, long start, long entries, unsigned short *requested, Colour palette[])
{
    Workstation *wk = vwk->real_address;
    long video = access->funcs.get_cookie("_VDO", 1);
    long i, count = wk->screen.palette.size;
    int extended = (unsigned long)requested & 1;

    if (start < 0 || entries <= 0 || start >= count)
        return;
    if (entries > count - start)
        entries = count - start;
    requested = (unsigned short *)((unsigned long)requested & ~1UL);
    video = video < 0 ? 0 : video >> 16;
    /* This fallback is for Atari planar video. Do not probe unrelated hardware. */
    if (video > 3)
        return;
    for (i = 0; i < entries; i++)
    {
        long index = extended ? start + i : x_get_colour(wk, start + i);
        long r, g, b, rr, gg, bb, levels = video == 0 ? 7 : video == 3 ? 255 : 15;
        unsigned short raw;
        if (extended)
        {
            requested++;
            r = ((unsigned long)*requested++ * 1000 + 32767) / 65535;
            g = ((unsigned long)*requested++ * 1000 + 32767) / 65535;
            b = ((unsigned long)*requested++ * 1000 + 32767) / 65535;
        } else
        {
            r = (short)*requested++; g = (short)*requested++; b = (short)*requested++;
            r = r < 0 ? 0 : r > 1000 ? 1000 : r;
            g = g < 0 ? 0 : g > 1000 ? 1000 : g;
            b = b < 0 ? 0 : b > 1000 ? 1000 : b;
        }
        rr = (r * levels + 500) / 1000;
        gg = (g * levels + 500) / 1000;
        bb = (b * levels + 500) / 1000;
        palette[index].vdi.red = r; palette[index].vdi.green = g; palette[index].vdi.blue = b;
        palette[index].hw.red = (rr * 1000 + levels / 2) / levels;
        palette[index].hw.green = (gg * 1000 + levels / 2) / levels;
        palette[index].hw.blue = (bb * 1000 + levels / 2) / levels;
        palette[index].real = index;
        if (wk->screen.mfdb.bitplanes == 1)
            continue;
        if (video == 3)
        {
            unsigned long color = (rr << 16) | (gg << 8) | bb;
            VsetRGB(index, 1, &color);
        } else if (video == 2)
            (void)EsetColor(index, (rr << 8) | (gg << 4) | bb);
        else
        {
            if (video == 1)
            {
                rr = (rr >> 1) | ((rr & 1) << 3);
                gg = (gg >> 1) | ((gg & 1) << 3);
                bb = (bb >> 1) | ((bb & 1) << 3);
            }
            raw = (rr << 8) | (gg << 4) | bb;
            (void)Setcolor(index, raw);
        }
    }
}
