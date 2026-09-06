/* NVDI's documented bit-15 raster scaling convention, independently written.
 * Integer nearest-neighbour MFDB scaling; see LICENSE.TXT.
 */
#include "fvdi.h"
#include "function.h"
#include "relocate.h"
#include "utility.h"

typedef struct {
    unsigned long pos, error, step, rem, den;
} RasterAxis;

static RasterAxis start_axis(unsigned long src, unsigned long dst, unsigned long skip)
{
    RasterAxis a;
    unsigned long n = src * skip;
    a.pos = n / dst; a.error = n % dst;
    a.step = src / dst; a.rem = src % dst; a.den = dst;
    return a;
}

static void next_axis(RasterAxis *a)
{
    a->pos += a->step;
    a->error += a->rem;
    if (a->error >= a->den) { a->error -= a->den; a->pos++; }
}

static unsigned long bitmap_size(const MFDB *b)
{
    unsigned long stride;
    if (!b || !b->address || ((unsigned long)b->address & 1) ||
        b->width <= 0 || b->height <= 0 || b->wdwidth < ((long)b->width + 15) / 16 ||
        (b->bitplanes != 1 && b->bitplanes != 2 && b->bitplanes != 4 && b->bitplanes != 8 &&
         b->bitplanes != 16 && b->bitplanes != 24 && b->bitplanes != 32) ||
        (b->standard != 0 && b->standard != 1))
        return 0;
    stride = (unsigned long)b->wdwidth * 2 * b->bitplanes;
    if (stride > 0x7fffffffUL / b->height)
        return 0;
    return stride * b->height;
}

static int overlap(const void *a, unsigned long na, const void *b, unsigned long nb)
{
    unsigned long aa = (unsigned long)a, bb = (unsigned long)b;
    return aa <= bb ? bb - aa < na : aa - bb < nb;
}

static unsigned long read_index(const MFDB *b, long x, long y, int packed)
{
    const unsigned char *base = (const unsigned char *)b->address;
    unsigned long pixel = 0;
    long p;
    if (packed && !b->standard && b->bitplanes > 1)
    {
        base += y * b->wdwidth * 2 * b->bitplanes + x * (b->bitplanes / 8);
        for (p = 0; p < b->bitplanes / 8; p++) pixel = (pixel << 8) | *base++;
    } else
        for (p = 0; p < b->bitplanes; p++)
        {
            long offset = b->standard ? ((p * b->height + y) * b->wdwidth + x / 16) * 2 :
                          ((y * b->wdwidth + x / 16) * b->bitplanes + p) * 2;
            unsigned short word = base[offset] * 256U + base[offset + 1];
            if (word & (0x8000U >> (x & 15))) pixel |= 1UL << p;
        }
    return pixel;
}

static void write_index(unsigned char *base, long x, long planes, int packed, unsigned long pixel)
{
    long p;
    if (packed && planes > 1)
    {
        base += x * (planes / 8);
        for (p = planes / 8; p--; ) { base[p] = pixel; pixel >>= 8; }
    } else
        for (p = 0; p < planes; p++)
            if (pixel & (1UL << p))
                base[(x / 16 * planes + p) * 2 + ((x & 15) >= 8)] |= 0x80U >> (x & 7);
}

static void scale(Virtual *vwk, const void *arguments, int expand)
{
    const unsigned char *args = arguments;
    const MFDB *src, *dst;
    const short *points, *pens = 0;
    Workstation *wk = vwk->real_address;
    short mode;
    long planes, xmin, ymin, xmax, ymax, sw, sh, dw, dh, x, row;
    unsigned long source_bytes, dest_bytes;
    unsigned char *buffer;
    long capacity;
    int screen, packed;

    memcpy(&mode, args, 2); mode &= 0x7fff;
    memcpy(&points, args + 2, sizeof(points));
    memcpy(&src, args + 2 + sizeof(points), sizeof(src));
    memcpy(&dst, args + 2 + sizeof(points) + sizeof(src), sizeof(dst));
    if (expand) memcpy(&pens, args + 2 + sizeof(points) + sizeof(src) + sizeof(dst), sizeof(pens));
    if (!points || !wk->driver || !wk->driver->device || (expand && !pens) ||
        (expand ? mode < 1 || mode > 4 : mode > 15))
        return;
    planes = wk->screen.mfdb.bitplanes;
    packed = wk->driver->device->format == 2;
    if ((!packed && wk->driver->device->format != 0) ||
        (packed && planes != 8 && planes != 16 && planes != 24 && planes != 32) ||
        (!packed && planes != 1 && planes != 2 && planes != 4 && planes != 8))
        return;
    source_bytes = bitmap_size(src);
    if (!source_bytes || src->bitplanes != (expand ? 1 : planes) ||
        points[0] < 0 || points[1] < 0 || points[2] >= src->width || points[3] >= src->height ||
        points[2] < points[0] || points[3] < points[1] ||
        points[6] < points[4] || points[7] < points[5])
        return;
    screen = !dst || !dst->address;
    if (screen)
    {
        if (!wk->screen.mfdb.address || wk->screen.mfdb.width <= 0 || wk->screen.mfdb.height <= 0 ||
            wk->screen.wrap < ((long)wk->screen.mfdb.width + 15) / 16 * 2 * planes)
            return;
        dest_bytes = (unsigned long)wk->screen.wrap * wk->screen.mfdb.height;
        if (overlap(src->address, source_bytes, wk->screen.mfdb.address, dest_bytes) ||
            (wk->screen.shadow.address && overlap(src->address, source_bytes, wk->screen.shadow.address, dest_bytes)))
            return;
        xmin = 0; ymin = 0;
        xmax = wk->screen.mfdb.width - 1; ymax = wk->screen.mfdb.height - 1;
        if (vwk->clip.on)
        {
            xmin = MAX(xmin, vwk->clip.rectangle.x1); ymin = MAX(ymin, vwk->clip.rectangle.y1);
            xmax = MIN(xmax, vwk->clip.rectangle.x2); ymax = MIN(ymax, vwk->clip.rectangle.y2);
        }
    } else
    {
        dest_bytes = bitmap_size(dst);
        if (!dest_bytes || dst->standard || dst->bitplanes != planes ||
            dst->address == wk->screen.mfdb.address || (void *)dst->address == wk->screen.shadow.address ||
            overlap(src->address, source_bytes, dst->address, dest_bytes) ||
            points[4] < 0 || points[5] < 0 || points[6] >= dst->width || points[7] >= dst->height)
            return;
        xmin = 0; ymin = 0; xmax = dst->width - 1; ymax = dst->height - 1;
    }
    xmin = MAX(xmin, points[4]); ymin = MAX(ymin, points[5]);
    xmax = MIN(xmax, points[6]); ymax = MIN(ymax, points[7]);
    if (xmin > xmax || ymin > ymax)
        return;
    sw = (long)points[2] - points[0] + 1; sh = (long)points[3] - points[1] + 1;
    dw = (long)points[6] - points[4] + 1; dh = (long)points[7] - points[5] + 1;
    planes = src->bitplanes;
    buffer = (unsigned char *)allocate_block(32);
    if (!buffer)
        return;
    capacity = *(long *)buffer;
    if (capacity < planes * 2)
    {
        free_block(buffer);
        return;
    }
    for (x = xmin; x <= xmax; )
    {
        long width = MIN(xmax - x + 1, MIN(capacity / (planes * 2) * 16, 32752L));
        long stride = (width + 15) / 16 * 2 * planes, previous = -1;
        MFDB staged;
        RasterAxis initial = start_axis(sw, dw, x - points[4]);
        RasterAxis y = start_axis(sh, dh, ymin - points[5]);
        memset(&staged, 0, sizeof(staged));
        staged.address = (short *)buffer; staged.width = width; staged.height = 1;
        staged.wdwidth = (width + 15) / 16; staged.bitplanes = planes;
        for (row = ymin; row <= ymax; row++)
        {
            short coords[8];
            if ((long)y.pos != previous)
            {
                long i;
                RasterAxis sx = initial;
                memset(buffer, 0, stride);
                for (i = 0; i < width; i++)
                {
                    unsigned long pixel = read_index(src, sx.pos + points[0], y.pos + points[1], packed);
                    write_index(buffer, i, planes, packed, pixel);
                    next_axis(&sx);
                }
            }
            coords[0] = coords[1] = coords[3] = 0; coords[2] = width - 1;
            coords[4] = x; coords[6] = x + width - 1; coords[5] = coords[7] = row;
            lib_vdi_spppp(expand ? (void *)lib_vrt_cpyfm : (void *)lib_vro_cpyfm, vwk, mode,
                          coords, &staged, (void *)dst, (void *)pens);
            previous = y.pos;
            next_axis(&y);
        }
        x += width;
    }
    free_block(buffer);
}

void CDECL legacy_vro_scale(Virtual *vwk, const void *args) { scale(vwk, args, 0); }
void CDECL legacy_vrt_scale(Virtual *vwk, const void *args) { scale(vwk, args, 1); }
