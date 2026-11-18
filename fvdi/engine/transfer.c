/*
 * Direct-colour conversion and indexed bitmap remapping.
 * This software is licensed under the GNU General Public License.
 * Please see LICENSE.TXT for further information.
 *
 * Independent implementation of the documented NVDI 5 raster interface.
 * RGB32/ARGB32/RGBA32/RGB16/indexed sources to RGB16/indexed destinations,
 * including native screens, and T_BLEND of direct sources into RGB16 memory
 * and onto RGB16 screens, through the driver's own blend where it has one.
 * Do not advertise complete new-raster support: other pairs are absent.
 */

#include <stdint.h>
#include "fvdi.h"
#include "function.h"
#include "relocate.h"
#include "utility.h"
#include "itable.h"
#include "ctab.h"
#include "blend.h"

#define RGB32       0x03421820UL
#define RGB565      0x03021010UL
#define RGB555      0x03420f10UL
#define REVERSED    0x00800000UL
#define INDEX8      0x01020808UL

static int source_size(const GCBITMAP *src)
{
    unsigned long format = src->px_format & ~REVERSED;
    if (src->px_format == INDEX8) return 1;
    if (src->px_format == RGB32 || src->px_format == PX_ARGB32 ||
        src->px_format == PX_RGBA32) return 4;
    if (format == RGB565 || format == RGB555) return 2;
    return 0;
}

typedef struct {
    long pos;
    unsigned long error, step, remainder, denominator;
} Axis;

/* The 16-bit layout, shared with the T_BLEND arithmetic of the drivers. */
typedef BlendPacking Packing;

#if defined(__GNUC__)
#define PIXEL_INLINE static __inline__ __attribute__((always_inline))
#else
#define PIXEL_INLINE static
#endif

/* Exact rational stepping: clipping does not restart the sampling phase.
 * Products are bounded by 65535 * 65536, even for signed RECT16 origins.
 * Divisions occur only when an axis is initialized, never in a pixel loop.
 */
static Axis axis_start(unsigned long source, unsigned long dest,
                       unsigned long skip, int filtered)
{
    Axis a;
    unsigned long n = skip * source;

    if (filtered)
    {
        n += source / 2;
        a.pos = (long)(n / dest) * 256 - 128;
        n = (n % dest) * 256 + (source & 1) * 128;
        a.pos += (long)(n / dest);
        source *= 256;
    } else
        a.pos = n / dest;
    a.error = n % dest;
    a.step = source / dest;
    a.remainder = source % dest;
    a.denominator = dest;
    return a;
}

PIXEL_INLINE void axis_next(Axis *a)
{
    a->pos += a->step;
    a->error += a->remainder;
    if (a->error >= a->denominator)
    {
        a->error -= a->denominator;
        a->pos++;
    }
}

/* Longs need only word alignment on m68k; all rows are checked beforehand.
 * The alias type permits reading caller-owned raw pixel storage directly.
 */
PIXEL_INLINE unsigned long read_rgb(const unsigned char *p)
{
#if defined(__m68k__)
    typedef uint32_t PixelWord __attribute__((may_alias));
    return *(const PixelWord *)p;
#else
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
#endif
}

/* A long of the pool block (even address): 0xAARRGGBB as bytes A, R, G, B. */
PIXEL_INLINE void write_rgb(unsigned char *p, unsigned long value)
{
#if defined(__m68k__)
    typedef uint32_t PixelWord __attribute__((may_alias));
    *(PixelWord *)p = value;
#else
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
#endif
}

/* The colour of a direct pixel, 0x..rrggbb: the top byte is not colour. */
PIXEL_INLINE unsigned long read_direct(const unsigned char *p, unsigned long format)
{
    unsigned long value, r, g, b;
    if (format == RGB32 || format == PX_ARGB32) return read_rgb(p);
    if (format == PX_RGBA32) return read_rgb(p) >> 8;
    value = (format & REVERSED) ? p[1] * 256U + p[0] : p[0] * 256U + p[1];
    b = value & 31;
    if ((format & ~REVERSED) == RGB565)
    {
        r = value >> 11; g = (value >> 5) & 63;
        g = (g << 2) | (g >> 4);
    } else
    {
        r = (value >> 10) & 31; g = (value >> 5) & 31;
        g = (g << 3) | (g >> 2);
    }
    return (((r << 3) | (r >> 2)) << 16) | (g << 8) | (b << 3) | (b >> 2);
}

PIXEL_INLINE unsigned short pack_rgb(unsigned long rgb, const Packing *p)
{
    unsigned short pixel = ((rgb >> p->red_shift) & p->red_mask) |
                           ((rgb >> p->green_shift) & p->green_mask) |
                           ((rgb >> 3) & 31);
    if (p->reversed)
        pixel = (pixel << 8) | (pixel >> 8);
    return pixel;
}

PIXEL_INLINE unsigned short pack_565(unsigned long rgb)
{
    return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 31);
}

PIXEL_INLINE void write_pixel(unsigned char *dst, unsigned short pixel)
{
#if defined(__m68k__)
    *(unsigned short *)dst = pixel;
#else
    dst[0] = pixel >> 8;
    dst[1] = pixel;
#endif
}

PIXEL_INLINE unsigned short read_pixel(const unsigned char *src)
{
#if defined(__m68k__)
    return *(const unsigned short *)src;
#else
    return (unsigned short)((src[0] << 8) | src[1]);
#endif
}

/* Bilinear interpolation at pixel centres for reductions. Eight fractional
 * bits keep all channel arithmetic within signed 32 bits. This is a bounded
 * four-sample filter, not an area filter for photographic thumbnails.
 */
PIXEL_INLINE unsigned long blend_rgb(unsigned long a, unsigned long b,
                                     unsigned long c, unsigned long d,
                                     long fx, unsigned long fy)
{
    unsigned long rgb = 0;
    long top, bottom, channel;
    int shift;

    for (shift = 16; shift >= 0; shift -= 8)
    {
        channel = (a >> shift) & 255;
        top = channel * 256 + ((long)((b >> shift) & 255) - channel) * fx;
        channel = (c >> shift) & 255;
        bottom = channel * 256 + ((long)((d >> shift) & 255) - channel) * fx;
        channel = (top * 256 + (bottom - top) * (long)fy + 32768) >> 16;
        rgb |= (unsigned long)channel << shift;
    }
    return rgb;
}

static unsigned long interpolate(const unsigned char *row0,
                                  const unsigned char *row1,
                                  long x, long width, unsigned long fy,
                                  unsigned long format, long pixel_bytes)
{
    unsigned long a, b, c, d;
    long fx;

    if (x < 0)
        x = 0;
    if (x > (width - 1) * 256)
        x = (width - 1) * 256;
    fx = x & 255;
    x = (x >> 8) * pixel_bytes;
    a = read_direct(row0 + x, format);
    c = read_direct(row1 + x, format);
    if (fx)
    {
        b = read_direct(row0 + x + pixel_bytes, format);
        d = read_direct(row1 + x + pixel_bytes, format);
    } else
    {
        b = a;
        d = c;
    }
    return blend_rgb(a, b, c, d, fx, fy);
}

static void render(const GCBITMAP *src, const RECT16 *sr, const RECT16 *dr,
                   const RECT16 *draw, unsigned char *dst, long stride,
                   const Packing *packing, const unsigned short *index_palette)
{
    /* Keep packing constants independent of stores to caller-owned memory.
     * Older GCC otherwise reloads the short fields after every pixel store.
     */
    Packing local_packing = *packing;
    unsigned long sw = (long)sr->x2 - sr->x1 + 1;
    unsigned long sh = (long)sr->y2 - sr->y1 + 1;
    unsigned long dw = (long)dr->x2 - dr->x1 + 1;
    unsigned long dh = (long)dr->y2 - dr->y1 + 1;
    int filtered = !index_palette && (sw > dw || sh > dh);
    int native565 = !packing->reversed && packing->red_mask == 0xf800 &&
                    packing->green_mask == 0x07e0;
    int raw565 = (src->px_format & ~REVERSED) == RGB565 &&
                 packing->red_mask == 0xf800 && packing->green_mask == 0x07e0 &&
                 !!packing->reversed == !!(src->px_format & REVERSED);
    Axis initial_x = axis_start(sw, dw, (long)draw->x1 - dr->x1, filtered);
    Axis y = axis_start(sh, dh, (long)draw->y1 - dr->y1, filtered);
    long count = (long)draw->x2 - draw->x1 + 1;
    long rows = (long)draw->y2 - draw->y1 + 1;
    long previous_y = -1;
    long pixel_bytes = src->bits / 8;
    const unsigned char *base = src->addr + ((long)sr->y1 - src->ymin) * src->width +
                                ((long)sr->x1 - src->xmin) * pixel_bytes;

    while (rows--)
    {
        unsigned char *out = dst;
        Axis x = initial_x;
        long n = count;
        const unsigned char *row;

        if (!filtered && y.pos == previous_y)
        {
            /* Enlargement often repeats rows. Reuse this strip's preceding
             * output, without rereading or converting the source pixels.
             */
            memcpy(dst, dst - stride, count * 2);
        } else if (index_palette)
        {
            row = base + y.pos * src->width;
            if (sw == dw)
            {
                row += x.pos;
                while (n--)
                {
                    write_pixel(out, index_palette[*row++]);
                    out += 2;
                }
            } else
                while (n--)
                {
                    write_pixel(out, index_palette[row[x.pos]]);
                    out += 2;
                    axis_next(&x);
                }
        } else if (filtered)
        {
            const unsigned char *next;
            long sy = y.pos;
            unsigned long fy;

            if (sy < 0)
                sy = 0;
            if (sy > (long)(sh - 1) * 256)
                sy = (sh - 1) * 256;
            fy = sy & 255;
            row = base + (sy >> 8) * src->width;
            next = fy ? row + src->width : row;
            while (n--)
            {
                write_pixel(out, pack_rgb(interpolate(row, next, x.pos, sw, fy, src->px_format, pixel_bytes), &local_packing));
                out += 2;
                axis_next(&x);
            }
        } else if (raw565)
        {
            /* Identical RGB565 packing needs no unpack/repack round trip. */
            row = base + y.pos * src->width;
            if (sw == dw)
                memcpy(out, row + x.pos * 2, count * 2);
            else
                while (n--)
                {
                    memcpy(out, row + x.pos * 2, 2);
                    out += 2;
                    axis_next(&x);
                }
        } else if (native565 && src->px_format == RGB32)
        {
            /* Constant shifts/masks keep the common Atari RGB565 loop small
             * and avoid register pressure from the generic packing fields.
             */
            row = base + y.pos * src->width;
            if (sw == dw)
            {
                row += x.pos * 4;
                while (n--)
                {
                    write_pixel(out, pack_565(read_rgb(row)));
                    row += 4;
                    out += 2;
                }
            } else
                while (n--)
                {
                    write_pixel(out, pack_565(read_rgb(row + x.pos * 4)));
                    out += 2;
                    axis_next(&x);
                }
        } else if (sw == dw)
        {
            /* Native-size conversion: no coordinate stepping per pixel. */
            row = base + y.pos * src->width + x.pos * pixel_bytes;
            while (n--)
            {
                write_pixel(out, pack_rgb(read_direct(row, src->px_format), &local_packing));
                row += pixel_bytes;
                out += 2;
            }
        } else
        {
            row = base + y.pos * src->width;
            while (n--)
            {
                write_pixel(out, pack_rgb(read_direct(row + x.pos * pixel_bytes, src->px_format), &local_packing));
                out += 2;
                axis_next(&x);
            }
        }
        dst += stride;
        previous_y = y.pos;
        axis_next(&y);
    }
}

/* T_BLEND: blend the part draw of dr in place, row by row from dst (stride
 * bytes apart), with the nearest source pixel.  The sampling phase is that of
 * the whole of dr, as for clipped copies; enlarged rows are not reused,
 * since each one blends with what it covers.  Alpha 0 pixels are skipped.
 */
static void blend_render(const GCBITMAP *src, const RECT16 *sr, const RECT16 *dr,
                         const RECT16 *draw, unsigned char *dst, long stride,
                         const Packing *packing, const unsigned long wprime[3])
{
    Packing local_packing = *packing;
    unsigned long format = src->px_format;
    unsigned long sw = (long)sr->x2 - sr->x1 + 1;
    unsigned long sh = (long)sr->y2 - sr->y1 + 1;
    unsigned long dw = (long)dr->x2 - dr->x1 + 1;
    unsigned long dh = (long)dr->y2 - dr->y1 + 1;
    Axis initial_x = axis_start(sw, dw, (long)draw->x1 - dr->x1, 0);
    Axis y = axis_start(sh, dh, (long)draw->y1 - dr->y1, 0);
    long count = (long)draw->x2 - draw->x1 + 1;
    long rows = (long)draw->y2 - draw->y1 + 1;
    long pixel_bytes = src->bits / 8;
    const unsigned char *base = src->addr + ((long)sr->y1 - src->ymin) * src->width +
                                ((long)sr->x1 - src->xmin) * pixel_bytes;
    unsigned short k[3] = { 0, 0, 0 }, alpha, last_alpha = 256;    /* k of no alpha yet */
    unsigned long rgb;

    while (rows--)
    {
        const unsigned char *row = base + y.pos * src->width;
        unsigned char *out = dst;
        Axis x = initial_x;
        long n = count;

        while (n--)
        {
            const unsigned char *p = row + x.pos * pixel_bytes;

            if (format == PX_ARGB32)
            {
                rgb = read_rgb(p);
                alpha = rgb >> 24;
            } else if (format == PX_RGBA32)
            {
                rgb = read_rgb(p);
                alpha = rgb & 255;
                rgb >>= 8;
            } else
            {
                rgb = read_direct(p, format);
                alpha = 255;
            }
            if (alpha)
            {
                if (alpha != last_alpha)
                {
                    last_alpha = alpha;
                    k[0] = blend_k(alpha, wprime[0]);
                    k[1] = blend_k(alpha, wprime[1]);
                    k[2] = blend_k(alpha, wprime[2]);
                }
                write_pixel(out, blend_pixel16(read_pixel(out), rgb, k, &local_packing));
            }
            out += 2;
            axis_next(&x);
        }
        dst += stride;
        axis_next(&y);
    }
}

/* T_BLEND through the driver (a BlendHook): the part draw of dr as rows of
 * PX_ARGB32 from dst, stride bytes apart, sampled as blend_render() samples
 * (the nearest source pixel, the phase of the whole of dr).  Sources
 * without alpha get alpha 255.  A row that samples the same source row as
 * the one before is copied: the rows are not blended here.
 */
static void argb_render(const GCBITMAP *src, const RECT16 *sr, const RECT16 *dr,
                        const RECT16 *draw, unsigned char *dst, long stride)
{
    unsigned long format = src->px_format;
    unsigned long sw = (long)sr->x2 - sr->x1 + 1;
    unsigned long sh = (long)sr->y2 - sr->y1 + 1;
    unsigned long dw = (long)dr->x2 - dr->x1 + 1;
    unsigned long dh = (long)dr->y2 - dr->y1 + 1;
    Axis initial_x = axis_start(sw, dw, (long)draw->x1 - dr->x1, 0);
    Axis y = axis_start(sh, dh, (long)draw->y1 - dr->y1, 0);
    long count = (long)draw->x2 - draw->x1 + 1;
    long rows = (long)draw->y2 - draw->y1 + 1;
    long previous_y = -1;
    long pixel_bytes = src->bits / 8;
    const unsigned char *base = src->addr + ((long)sr->y1 - src->ymin) * src->width +
                                ((long)sr->x1 - src->xmin) * pixel_bytes;
    unsigned long value;

    while (rows--)
    {
        const unsigned char *row = base + y.pos * src->width;
        unsigned char *out = dst;
        Axis x = initial_x;
        long n = count;

        if (y.pos == previous_y)
            memcpy(dst, dst - stride, count * 4);
        else
            while (n--)
            {
                const unsigned char *p = row + x.pos * pixel_bytes;

                if (format == PX_ARGB32)
                    value = read_rgb(p);
                else if (format == PX_RGBA32)
                {
                    value = read_rgb(p);
                    value = (value >> 8) | ((value & 255) << 24);
                } else
                    value = 0xff000000UL | (read_direct(p, format) & 0xffffffUL);
                write_rgb(out, value);
                out += 4;
                axis_next(&x);
            }
        dst += stride;
        previous_y = y.pos;
        axis_next(&y);
    }
}

/* Limit descriptions to the coordinate space RECT16 can address, and check
 * row sizes/products before forming pointers. The caller owns the storage.
 */
static unsigned long bitmap_bytes(const GCBITMAP *bm, long bytes_per_pixel)
{
    long w, h;

    if (!bm->addr || bm->xmin < -32768L || bm->ymin < -32768L ||
        bm->xmax > 32768L || bm->ymax > 32768L ||
        bm->xmax <= bm->xmin || bm->ymax <= bm->ymin ||
        (bytes_per_pixel > 1 && (((unsigned long)bm->addr | bm->width) & 1)))
        return 0;
    w = bm->xmax - bm->xmin;
    h = bm->ymax - bm->ymin;
    if (bm->width < w * bytes_per_pixel || bm->width > 0x7fffffffL / h)
        return 0;
    return bm->width * h;
}

static int overlaps(const void *a, unsigned long alen, const void *b, unsigned long blen)
{
    unsigned long aa = (unsigned long)a, bb = (unsigned long)b;
    return aa <= bb ? bb - aa < alen : aa - bb < blen;
}

/* A memory destination in RGB565 or RGB555, either byte order. */
static int memory_packing(unsigned long px_format, Packing *p)
{
    unsigned long format = px_format & ~REVERSED;

    if (format != RGB565 && format != RGB555)
        return 0;
    p->red_mask = format == RGB565 ? 0xf800 : 0x7c00;
    p->green_mask = format == RGB565 ? 0x07e0 : 0x03e0;
    p->red_shift = format == RGB565 ? 8 : 9;
    p->green_shift = format == RGB565 ? 5 : 6;
    p->reversed = (px_format & REVERSED) != 0;
    return 1;
}

static int screen_packing(const Device *dev, Packing *p)
{
    int i, red_start, green_start;

    if (!dev || dev->format != 2 || dev->bits.red != 5 || dev->bits.blue != 5 ||
        (dev->bits.green != 5 && dev->bits.green != 6))
        return 0;
    red_start = dev->scrmap.bitnumber.red[0];
    green_start = dev->scrmap.bitnumber.green[0];
    if (!((red_start == 11 && (green_start == 5 || green_start == 6)) ||
          (red_start == 10 && green_start == 5 && dev->bits.green == 5)))
        return 0;
    if (green_start + dev->bits.green > red_start)
        return 0;
    for (i = 0; i < 5; i++)
        if (dev->scrmap.bitnumber.red[i] != red_start + i ||
            dev->scrmap.bitnumber.blue[i] != i)
            return 0;
    for (i = 0; i < dev->bits.green; i++)
        if (dev->scrmap.bitnumber.green[i] != green_start + i)
            return 0;
    p->red_mask = 31U << red_start;
    p->green_mask = ((1U << dev->bits.green) - 1) << green_start;
    p->red_shift = 19 - red_start;
    p->green_shift = 16 - dev->bits.green - green_start;
    p->reversed = 0;
    return 1;
}

/* Whether the screen of wk is an RGB16 destination, and its layout: 16
 * bitplanes in a layout screen_packing() knows, of a size that can be
 * drawn.  The one test for drawing (screen_area()) and for advertising
 * T_BLEND (transfer_blend_ok()), so that the two cannot drift apart.
 */
static int screen_rgb16(const Workstation *wk, Packing *packing)
{
    return wk->screen.mfdb.bitplanes == 16 && wk->driver &&
           screen_packing(wk->driver->device, packing) &&
           wk->screen.mfdb.width > 0 && wk->screen.mfdb.height > 0 &&
           wk->screen.wrap >= (long)wk->screen.mfdb.width * 2;
}

/* The screen as an RGB16 destination: its layout, and the part of it that
 * may be drawn, all of it or, when clipping is on, its cut with the vs_clip
 * rectangle.  The source must not lie in the screen or its shadow.  Returns
 * 0 to refuse the call.
 */
static int screen_area(Virtual *vwk, const GCBITMAP *src, unsigned long src_bytes,
                       Packing *packing, RECT16 *area)
{
    Workstation *wk = vwk->real_address;
    unsigned long screen_bytes;

    if (!screen_rgb16(wk, packing))
        return 0;
    screen_bytes = (unsigned long)wk->screen.wrap * wk->screen.mfdb.height;
    /* Screen output is staged through the driver, so VRAM need not be mapped. */
    if ((wk->screen.mfdb.address &&
         overlaps(src->addr, src_bytes, wk->screen.mfdb.address, screen_bytes)) ||
        (wk->screen.shadow.address &&
         overlaps(src->addr, src_bytes, wk->screen.shadow.address, screen_bytes)))
        return 0;
    area->x1 = 0;
    area->y1 = 0;
    area->x2 = wk->screen.mfdb.width - 1;
    area->y2 = wk->screen.mfdb.height - 1;
    if (vwk->clip.on)
    {
        area->x1 = MAX(area->x1, vwk->clip.rectangle.x1);
        area->y1 = MAX(area->y1, vwk->clip.rectangle.y1);
        area->x2 = MIN(area->x2, vwk->clip.rectangle.x2);
        area->y2 = MIN(area->y2, vwk->clip.rectangle.y2);
    }
    return 1;
}

/* Draw the part area of dr onto the screen through one borrowed pool block.
 * Batch complete rows when they fit; split wide rows otherwise. No
 * frame-sized allocation or new cache. The driver blit preserves its
 * screen/shadow-buffer conventions, and the engine never touches VRAM.
 *
 * With wprime (T_BLEND), each strip is first read back from the screen
 * through the same blit, which reads the shadow when there is one, then
 * the source is blended into it and the strip written back.  Each pixel of
 * area is read and written once, and nothing outside it.  The sampling
 * phase is that of the whole of dr in every strip.  Without a pool block
 * nothing is drawn.
 *
 * With the driver's blend (hook, T_BLEND only), each strip is instead
 * sampled into the block as PX_ARGB32 rows, twice the bytes of a pixel,
 * and handed to the driver, which blends it onto the screen.  Should the
 * driver decline a strip, that strip and the rest go the read-back way;
 * they fit, at half the size.  A block too small for 16 such pixels is
 * read back from the start.
 */
static void screen_strips(Virtual *vwk, const GCBITMAP *src, const RECT16 *sr,
                          const RECT16 *dr, const RECT16 *area, const Packing *packing,
                          const unsigned short *index_palette, const unsigned long *wprime,
                          BlendHook hook)
{
    RECT16 strip;
    MFDB block, screen;
    char *buffer;
    long capacity, unit, x, y;

    buffer = allocate_block(32);
    if (!buffer)
        return;
    capacity = *(long *)buffer;
    if (capacity < 32)
    {
        free_block(buffer);
        return;
    }
    if (capacity < 64)
        hook = 0;
    unit = hook ? 2 : 1;                    /* bytes a pixel takes in the block, over 2 */
    memset(&screen, 0, sizeof(screen));     /* address 0: the screen */
    for (x = area->x1; x <= area->x2; x = (long)strip.x2 + 1)
    {
        long width = MIN(area->x2 - x + 1, MIN((capacity / (32 * unit)) * 16, 32752L));
        long stride = ((width + 15) & ~15L) * 2;
        long height = MIN(capacity / (stride * unit), 32767L);

        strip.x1 = x;
        strip.x2 = x + width - 1;
        memset(&block, 0, sizeof(block));
        block.address = (short *)buffer;
        block.width = width;
        block.wdwidth = stride / 32;
        block.bitplanes = 16;
        for (y = area->y1; y <= area->y2; y = (long)strip.y2 + 1)
        {
            short coords[8];

            strip.y1 = y;
            strip.y2 = MIN(area->y2, y + height - 1);
            block.height = (long)strip.y2 - y + 1;
            if (hook)
            {
                argb_render(src, sr, dr, &strip, (unsigned char *)buffer, stride * 2);
                if (hook(vwk, (unsigned char *)buffer, stride * 2, strip.x1, strip.y1,
                         width, block.height, wprime))
                    continue;
                hook = 0;
            }
            if (wprime)
            {
                /* A memory destination is not clipped: the read is exact. */
                coords[0] = strip.x1;
                coords[1] = strip.y1;
                coords[2] = strip.x2;
                coords[3] = strip.y2;
                coords[4] = coords[5] = 0;
                coords[6] = width - 1;
                coords[7] = block.height - 1;
                lib_vdi_spppp(lib_vro_cpyfm, vwk, 3, coords, &screen, &block, 0);
                blend_render(src, sr, dr, &strip, (unsigned char *)buffer, stride, packing, wprime);
            } else
                render(src, sr, dr, &strip, (unsigned char *)buffer, stride, packing, index_palette);
            coords[0] = coords[1] = 0;
            coords[2] = width - 1;
            coords[3] = block.height - 1;
            coords[4] = strip.x1;
            coords[5] = strip.y1;
            coords[6] = strip.x2;
            coords[7] = strip.y2;
            lib_vdi_spppp(lib_vro_cpyfm, vwk, 3, coords, &block, 0, 0);
        }
    }
    free_block(buffer);
}

/* Return zero only for pairs owned by the older implementation. A refused
 * A supported-source -> 16-bit refusal must not reach its assumed-32-bit path.
 */
static int transfer_rgb16_common(Virtual *vwk, const GCBITMAP *src, GCBITMAP *dst,
                                 const RECT16 *sr, const RECT16 *dr, long mode,
                                 unsigned short *index_palette)
{
    Workstation *wk = vwk->real_address;
    Packing packing;
    RECT16 draw;
    unsigned long src_bytes, dst_bytes;
    long xmin, ymin, xmax, ymax;
    long x, y;
    int indexed;
    GCBITMAP source;

    if (!src || !source_size(src) ||
        (dst ? dst->bits != 16 : wk->screen.mfdb.bitplanes != 16))
        return 0;
    indexed = src->px_format == INDEX8;
    /* Dithering has no effect on a destination with more than 256 colours. */
    mode &= ~128L;
    if ((mode != 0 && mode != 32) || src->bits != source_size(src) * 8 || !sr || !dr ||
        sr->x2 < sr->x1 || sr->y2 < sr->y1 || dr->x2 < dr->x1 || dr->y2 < dr->y1)
        return 1;
    src_bytes = bitmap_bytes(src, source_size(src));
    if (!src_bytes || sr->x1 < src->xmin || sr->y1 < src->ymin ||
        sr->x2 >= src->xmax || sr->y2 >= src->ymax)
        return 1;

    if (indexed)
    {
        if (!src->ctab)
        {
            source = *src;
            source.ctab = (COLOR_TAB *)ctab_for_bitmap(vwk, 8);
            src = &source;
        }
        if (!itab_valid_palette(src->ctab))
            return 1;
        if (src->ctab->no_colors < 256)
            for (y = sr->y1; y <= sr->y2; y++)
            {
                const unsigned char *row = src->addr + (y - src->ymin) * src->width;
                for (x = sr->x1; x <= sr->x2; x++)
                    if (row[x - src->xmin] >= src->ctab->no_colors)
                        return 1;
            }
    }

    if (dst)
    {
        dst_bytes = bitmap_bytes(dst, 2);
        if (!dst_bytes || !memory_packing(dst->px_format, &packing) ||
            overlaps(src->addr, src_bytes, dst->addr, dst_bytes) ||
            (void *)dst->addr == wk->screen.mfdb.address || dst->addr == wk->screen.shadow.address)
            return 1;
        xmin = dst->xmin;
        ymin = dst->ymin;
        xmax = dst->xmax - 1;
        ymax = dst->ymax - 1;
        /* Offscreen callers must supply valid destination coordinates. */
        if (dr->x1 < xmin || dr->y1 < ymin || dr->x2 > xmax || dr->y2 > ymax)
            return 1;
    } else
    {
        if (!screen_area(vwk, src, src_bytes, &packing, &draw))
            return 1;
        xmin = draw.x1;
        ymin = draw.y1;
        xmax = draw.x2;
        ymax = draw.y2;
    }
    xmin = MAX(xmin, dr->x1);
    ymin = MAX(ymin, dr->y1);
    xmax = MIN(xmax, dr->x2);
    ymax = MIN(ymax, dr->y2);
    if (xmin > xmax || ymin > ymax)
        return 1;
    if (indexed)
        for (x = 0; x < src->ctab->no_colors; x++)
        {
            const COLOR_RGB *rgb = &src->ctab->colors[x].rgb;
            unsigned long value = ((unsigned long)(rgb->red >> 8) << 16) |
                                  ((unsigned long)(rgb->green >> 8) << 8) | (rgb->blue >> 8);
            index_palette[x] = pack_rgb(value, &packing);
        }
    draw.x1 = xmin;
    draw.y1 = ymin;
    draw.x2 = xmax;
    draw.y2 = ymax;
    if (dst)
    {
        render(src, sr, dr, &draw,
               dst->addr + (ymin - dst->ymin) * dst->width + (xmin - dst->xmin) * 2,
               dst->width, &packing, indexed ? index_palette : 0);
        return 1;
    }
    screen_strips(vwk, src, sr, dr, &draw, &packing, indexed ? index_palette : 0, 0, 0);
    return 1;
}

/* Keep the 512-byte prepacked indexed palette off the direct-colour stack. */
#ifdef __GNUC__
__attribute__((noinline))
#endif
static int indexed_to_rgb16(Virtual *vwk, const GCBITMAP *src, GCBITMAP *dst,
                            const RECT16 *sr, const RECT16 *dr, long mode)
{
    unsigned short palette[256];
    return transfer_rgb16_common(vwk, src, dst, sr, dr, mode, palette);
}

int CDECL transfer_rgb16(Virtual *vwk, const GCBITMAP *src, GCBITMAP *dst,
                         const RECT16 *sr, const RECT16 *dr, long mode)
{
    if (src && src->px_format == INDEX8)
        return indexed_to_rgb16(vwk, src, dst, sr, dr, mode);
    return transfer_rgb16_common(vwk, src, dst, sr, dr, mode, 0);
}

/* The driver's own T_BLEND onto its screen, if it offers one: the answer of
 * its setup(Q_BLEND_ARGB, 0), asked once and kept for that driver (one
 * driver at a time; another one is asked when it comes).  -1 or 0 is no
 * blend.  Built with FVDI_NO_BLEND_HOOK, no driver is asked.
 */
static BlendHook blend_hook(const Driver *driver)
{
#ifdef FVDI_NO_BLEND_HOOK
    (void)driver;
    return 0;
#else
    static const Driver *hook_driver;
    static BlendHook hook_fn;
    long answer;

    if (driver != hook_driver)
    {
        answer = driver->module.setup ? driver->module.setup(Q_BLEND_ARGB, 0) : -1;
        hook_fn = answer == -1 || answer == 0 ? 0 : (BlendHook)answer;
        hook_driver = driver;
    }
    return hook_fn;
#endif
}

/* vr_transfer_bits with an arithmetic mode (64-127, with or without bit 7).
 * Only T_BLEND is drawn: a direct-colour source (alpha 255 unless it is
 * PX_ARGB32 or PX_RGBA32) times the workstation's weight colour, blended in
 * place into memory RGB565/RGB555, either byte order, or onto a 16-bit
 * screen whose layout screen_packing() knows.  A memory destination is not
 * clipped: it must hold all of dr.  On the screen, the part of dr inside
 * the screen and the vs_clip rectangle is drawn.  When the driver offers
 * its own blend (blend_hook()), it gets the caller's rows for PX_ARGB32 at
 * 1:1, else strips sampled and converted to PX_ARGB32 in a pool block
 * (screen_strips()).  Otherwise, or when the driver declines, the part is
 * read back through the driver's blit, blended and written back.  Every
 * other mode, format or destination, and every call that is not
 * understood, draws nothing: the older paths would copy regardless of the
 * mode.
 */
void CDECL transfer_blend(Virtual *vwk, const GCBITMAP *src, GCBITMAP *dst,
                          const RECT16 *sr, const RECT16 *dr, long mode)
{
    Workstation *wk = vwk->real_address;
    Packing packing;
    RECT16 area;
    unsigned long src_bytes, dst_bytes, wprime[3];
    unsigned short weight[3];
    long size;

    if ((mode & ~128L) != T_BLEND || !src || !sr || !dr)
        return;
    size = source_size(src);
    if (!size || src->px_format == INDEX8 || src->bits != size * 8 ||
        sr->x2 < sr->x1 || sr->y2 < sr->y1 || dr->x2 < dr->x1 || dr->y2 < dr->y1)
        return;
    src_bytes = bitmap_bytes(src, size);
    if (!src_bytes || sr->x1 < src->xmin || sr->y1 < src->ymin ||
        sr->x2 >= src->xmax || sr->y2 >= src->ymax)
        return;
    x_weight(vwk, weight);
    wprime[0] = blend_wprime(weight[0]);
    wprime[1] = blend_wprime(weight[1]);
    wprime[2] = blend_wprime(weight[2]);
    if (!dst)
    {
        BlendHook hook;

        if (!screen_area(vwk, src, src_bytes, &packing, &area))
            return;
        area.x1 = MAX(area.x1, dr->x1);
        area.y1 = MAX(area.y1, dr->y1);
        area.x2 = MIN(area.x2, dr->x2);
        area.y2 = MIN(area.y2, dr->y2);
        if (area.x1 > area.x2 || area.y1 > area.y2)
            return;
        hook = blend_hook(wk->driver);
        if (hook && src->px_format == PX_ARGB32 &&
            (long)sr->x2 - sr->x1 == (long)dr->x2 - dr->x1 &&
            (long)sr->y2 - sr->y1 == (long)dr->y2 - dr->y1)
        {
            /* At 1:1 the rows are the caller's, from the source pixel of area's corner */
            const unsigned char *rows = src->addr +
                ((long)sr->y1 - src->ymin + area.y1 - dr->y1) * src->width +
                ((long)sr->x1 - src->xmin + area.x1 - dr->x1) * 4;

            if (hook(vwk, rows, src->width, area.x1, area.y1,
                     (long)area.x2 - area.x1 + 1, (long)area.y2 - area.y1 + 1, wprime))
                return;
            hook = 0;
        }
        screen_strips(vwk, src, sr, dr, &area, &packing, 0, wprime, hook);
        return;
    }
    if (dst->bits != 16 || !memory_packing(dst->px_format, &packing))
        return;
    dst_bytes = bitmap_bytes(dst, 2);
    if (!dst_bytes || overlaps(src->addr, src_bytes, dst->addr, dst_bytes) ||
        (void *)dst->addr == wk->screen.mfdb.address || dst->addr == wk->screen.shadow.address ||
        dr->x1 < dst->xmin || dr->y1 < dst->ymin || dr->x2 >= dst->xmax || dr->y2 >= dst->ymax)
        return;
    blend_render(src, sr, dr, dr,
                 dst->addr + ((long)dr->y1 - dst->ymin) * dst->width + ((long)dr->x1 - dst->xmin) * 2,
                 dst->width, &packing, wprime);
}

/* Bit 3 of vq_extnd's work_out[30]: T_BLEND draws on the screen of vwk
 * (screen_rgb16(), the drawing's own test) and vq_px_format names its
 * layout, RGB565 or RGB555.  The 5-5-5 layouts with a gap blend as well,
 * but vq_px_format has no name for them, so they are not advertised.
 */
int transfer_blend_ok(Virtual *vwk)
{
    Packing packing;
    unsigned long format;

    if (!screen_rgb16(vwk->real_address, &packing))
        return 0;
    format = ctab_pixel_format(vwk);
    return format == RGB565 || format == RGB555;
}

/* The table quantizes RGB8 at the requested 3-5 bit resolution per channel.
 * All shifts/masks are independent of the caller's destination stores.
 */
PIXEL_INLINE unsigned char index_rgb(unsigned long rgb, const unsigned char *map,
                                    unsigned long bits)
{
    unsigned long mask = (1UL << bits) - 1;
    unsigned long r = (rgb >> (24 - 3 * bits)) & (mask << (2 * bits));
    unsigned long g = (rgb >> (16 - 2 * bits)) & (mask << bits);
    unsigned long b = (rgb >> (8 - bits)) & mask;
    return map[r | g | b];
}

static unsigned long interpolate_index8(const unsigned char *row0,
                                         const unsigned char *row1,
                                         long x, long width, unsigned long fy,
                                         const uint32_t *palette)
{
    long fx, right;
    x = MAX(0, MIN(x, (width - 1) * 256));
    fx = x & 255;
    x >>= 8;
    right = x + (fx != 0);
    return blend_rgb(palette[row0[x]], palette[row0[right]],
                     palette[row1[x]], palette[row1[right]], fx, fy);
}

typedef struct {
    Virtual *vwk;
    RECT16 clip;
    unsigned char *buffer;
    long capacity, planes;
} IndexedScreen;

static void present_index8(const IndexedScreen *screen, const unsigned char *row,
                            const RECT16 *dr, long y)
{
    long x, limit = MIN((screen->capacity / (2 * screen->planes)) * 16, 32752L);
    if (y < screen->clip.y1 || y > screen->clip.y2)
        return;
    for (x = screen->clip.x1; x <= screen->clip.x2; )
    {
        long width = MIN((long)screen->clip.x2 - x + 1, limit), group, p;
        MFDB mfdb;
        short coords[8];
        for (group = 0; group < width; group += 16)
        {
            unsigned short words[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
            long i, count = MIN(16, width - group);
            for (i = 0; i < count; i++)
            {
                unsigned char index = row[x - dr->x1 + group + i];
                for (p = 0; p < screen->planes; p++)
                    if (index & (1U << p)) words[p] |= 0x8000U >> i;
            }
            for (p = 0; p < screen->planes; p++)
                write_pixel(screen->buffer + (group / 16 * screen->planes + p) * 2, words[p]);
        }
        memset(&mfdb, 0, sizeof(mfdb));
        mfdb.address = (short *)screen->buffer;
        mfdb.width = width; mfdb.height = 1; mfdb.wdwidth = (width + 15) / 16;
        mfdb.bitplanes = screen->planes;
        coords[0] = coords[1] = coords[3] = 0;
        coords[2] = width - 1;
        coords[4] = x; coords[6] = x + width - 1;
        coords[5] = coords[7] = y;
        lib_vdi_spppp(lib_vro_cpyfm, screen->vwk, 3, coords, &mfdb, 0, 0);
        x += width;
    }
}

static void render_index8(const GCBITMAP *src, GCBITMAP *dst,
                           const RECT16 *sr, const RECT16 *dr,
                           const InverseTable *table, int dither,
                           const uint32_t *palette, const unsigned char *remap,
                           int identity, const IndexedScreen *screen)
{
    const unsigned char *map, *base;
    unsigned char *dest;
    unsigned long sw, sh, dw, dh, bits;
    Axis initial_x, y;
    long row_number, x_number, source_stride, dest_stride, pixel_bytes = src->bits / 8;
    long previous_y = -1;
    short *errors = 0;
    int filtered;
    sw = (long)sr->x2 - sr->x1 + 1;
    sh = (long)sr->y2 - sr->y1 + 1;
    dw = (long)dr->x2 - dr->x1 + 1;
    dh = (long)dr->y2 - dr->y1 + 1;
    filtered = (!palette || dither) && (sw > dw || sh > dh);
    if (dither)
    {
        /* One in-place error row, not two. Each component stores weighted
         * errors with denominator 16, bounded to +/-4080, hence shorts.
         */
        errors = calloc(dw * 3, sizeof(*errors));
        if (!errors)
            return;
    }
    bits = table->bits;
    map = ITAB_PIXELS(table);
    source_stride = src->width;
    dest_stride = dst->width;
    base = src->addr + ((long)sr->y1 - src->ymin) * source_stride +
                       ((long)sr->x1 - src->xmin) * pixel_bytes;
    dest = dst->addr + ((long)dr->y1 - dst->ymin) * dest_stride + ((long)dr->x1 - dst->xmin);
    initial_x = axis_start(sw, dw, 0, filtered);
    y = axis_start(sh, dh, 0, filtered);
    for (row_number = 0; row_number < (long)dh; row_number++)
    {
        Axis x = initial_x;
        const unsigned char *row, *next;
        long carry[3] = { 0, 0, 0 }, diagonal[3] = { 0, 0, 0 };
        long sy = y.pos;
        unsigned long fy = 0;
        short *err = errors;

        if (filtered)
        {
            sy = MAX(0, MIN(sy, (long)(sh - 1) * 256));
            fy = sy & 255;
            sy >>= 8;
        }
        row = base + sy * source_stride;
        next = fy ? row + source_stride : row;
        if (!screen && !dither && !filtered && sy == previous_y)
        {
            memcpy(dest, dest - dest_stride, dw);
        } else if (palette && !dither)
        {
            if (identity && sw == dw)
                memcpy(dest, row, dw);
            else if (sw == dw)
                for (x_number = 0; x_number < (long)dw; x_number++)
                    dest[x_number] = remap[row[x_number]];
            else
                for (x_number = 0; x_number < (long)dw; x_number++)
                {
                    dest[x_number] = remap[row[x.pos]];
                    axis_next(&x);
                }
        } else if (!dither)
        {
            /* Keep the lookup-only loop separate from diffusion's live
             * channel/error state, especially for older m68k compilers.
             */
            for (x_number = 0; x_number < (long)dw; x_number++)
            {
                unsigned long rgb = filtered ? interpolate(row, next, x.pos, sw, fy, src->px_format, pixel_bytes) :
                                               read_direct(row + x.pos * pixel_bytes, src->px_format);
                dest[x_number] = index_rgb(rgb, map, bits);
                axis_next(&x);
            }
        } else
            for (x_number = 0; x_number < (long)dw; x_number++)
            {
                unsigned long rgb;
                unsigned char pixel;
                long channels[3], k, shift;
                unsigned long adjusted = 0;
                const COLOR_RGB *chosen;

                if (palette)
                    rgb = filtered ? interpolate_index8(row, next, x.pos, sw, fy, palette) :
                                     palette[row[x.pos]];
                else
                    rgb = filtered ? interpolate(row, next, x.pos, sw, fy, src->px_format, pixel_bytes) :
                                     read_direct(row + x.pos * pixel_bytes, src->px_format);
                for (k = 0, shift = 16; k < 3; k++, shift -= 8)
                {
                    long value = (long)((rgb >> shift) & 255) +
                                 ((err[k] + carry[k] + 8) >> 4);
                    channels[k] = MAX(0, MIN(value, 255));
                    adjusted |= (unsigned long)channels[k] << shift;
                }
                pixel = index_rgb(adjusted, map, bits);
                chosen = &table->colors[pixel].rgb;
                channels[0] -= chosen->red >> 8;
                channels[1] -= chosen->green >> 8;
                channels[2] -= chosen->blue >> 8;
                /* Floyd-Steinberg, left to right. The old error at x is
                 * consumed before this slot becomes the next row's x.
                 * Keep the previous pixel's down-right term in a register
                 * instead of overwriting an unconsumed old-row entry.
                 */
                for (k = 0; k < 3; k++)
                {
                    long error = channels[k];
                    if (x_number)
                        err[k - 3] += error * 3;
                    err[k] = error * 5 + diagonal[k];
                    diagonal[k] = error;
                    carry[k] = error * 7;
                }
                err += 3;
                dest[x_number] = pixel;
                axis_next(&x);
            }
        previous_y = sy;
        if (screen)
            present_index8(screen, dest, dr, row_number + dr->y1);
        else
            dest += dest_stride;
        axis_next(&y);
    }
    if (errors)
        free(errors);
}

static int same_rgb(const COLOR_RGB *a, const COLOR_RGB *b)
{
    return a->red == b->red && a->green == b->green && a->blue == b->blue;
}

/* Keep the fixed 1,280-byte palette workspace off the RGB32 call path.
 * No full-image conversion, persistent remap cache or per-pixel search.
 */
static void remap_index8(const GCBITMAP *src, GCBITMAP *dst,
                          const RECT16 *sr, const RECT16 *dr,
                          const InverseTable *table, int dither, const IndexedScreen *screen)
{
    uint32_t palette[256];
    unsigned char remap[256];
    unsigned short i, j, count = src->ctab->no_colors;
    int exact = 1, identity = 1;
    long x, y, width = (long)sr->x2 - sr->x1 + 1;
    const unsigned char *row;

    /* Validate the whole requested source before the first destination write.
     * A complete 256-entry palette makes every byte index valid: skip the scan.
     * Padding and pixels outside the source rectangle are not palette indices.
     */
    if (count < 256)
    {
        row = src->addr + ((long)sr->y1 - src->ymin) * src->width +
                          ((long)sr->x1 - src->xmin);
        for (y = sr->y1; y <= sr->y2; y++, row += src->width)
            for (x = 0; x < width; x++)
                if (row[x] >= count)
                    return;
    }
    for (i = 0; i < count; i++)
    {
        const COLOR_RGB *rgb = &src->ctab->colors[i].rgb;
        palette[i] = ((uint32_t)(rgb->red >> 8) << 16) |
                     ((uint32_t)(rgb->green >> 8) << 8) | (rgb->blue >> 8);
        /* Preserve matching index positions, including duplicate colours.
         * Otherwise choose the first exact destination RGB16 match.
         * Reserved entry words are not colour components.
         */
        if (i < table->count && same_rgb(rgb, &table->colors[i].rgb))
            j = i;
        else
            for (j = 0; j < table->count; j++)
                if (same_rgb(rgb, &table->colors[j].rgb))
                    break;
        if (j == table->count)
        {
            exact = 0;
            j = index_rgb(palette[i], ITAB_PIXELS(table), table->bits);
        }
        remap[i] = j;
        if (j != i)
            identity = 0;
    }
    /* Generalize the documented equal/system-subset bypass to an explicitly
     * supplied destination palette containing every source colour exactly.
     * Indexed reductions without diffusion sample indices, never blend them.
     */
    render_index8(src, dst, sr, dr, table, dither && !exact, palette, remap, identity, screen);
}

/* Native indexed screen output uses pixel-ordered device palettes and the
 * existing planar driver blit. Other screen layouts retain their old handler.
 */
static int transfer_index_screen(Virtual *vwk, const GCBITMAP *src,
                                  const RECT16 *sr, const RECT16 *dr, long mode)
{
    Workstation *wk = vwk->real_address;
    IndexedScreen screen;
    GCBITMAP source, dest;
    const COLOR_TAB *palette;
    const InverseTable *table;
    unsigned long bytes, screen_bytes;
    long width, xmin, ymin, xmax, ymax, row_offset;
    unsigned char *row;
    int allocated_row = 0;
    int indexed = src->px_format == INDEX8;

    screen.planes = wk->screen.mfdb.bitplanes;
    if (screen.planes > 8)
        return 0;
    if ((screen.planes != 1 && screen.planes != 2 && screen.planes != 4 && screen.planes != 8) ||
        !wk->driver || !wk->driver->device || wk->driver->device->format != 0 ||
        wk->driver->device->clut != 1 || !wk->screen.mfdb.address ||
        wk->screen.mfdb.width <= 0 || wk->screen.mfdb.height <= 0 ||
        wk->screen.wrap < ((long)wk->screen.mfdb.width + 15) / 16 * 2 * screen.planes ||
        ((mode & ~128L) != 0 && (mode & ~128L) != 32) ||
        src->bits != source_size(src) * 8 || !sr || !dr ||
        sr->x1 > sr->x2 || sr->y1 > sr->y2 || dr->x1 > dr->x2 || dr->y1 > dr->y2)
        return 1;
    bytes = bitmap_bytes(src, source_size(src));
    screen_bytes = (unsigned long)wk->screen.wrap * wk->screen.mfdb.height;
    if (!bytes || sr->x1 < src->xmin || sr->y1 < src->ymin ||
        sr->x2 >= src->xmax || sr->y2 >= src->ymax ||
        overlaps(src->addr, bytes, wk->screen.mfdb.address, screen_bytes) ||
        (wk->screen.shadow.address && overlaps(src->addr, bytes, wk->screen.shadow.address, screen_bytes)))
        return 1;
    xmin = MAX(0, dr->x1); ymin = MAX(0, dr->y1);
    xmax = MIN(wk->screen.mfdb.width - 1, dr->x2);
    ymax = MIN(wk->screen.mfdb.height - 1, dr->y2);
    if (vwk->clip.on)
    {
        xmin = MAX(xmin, vwk->clip.rectangle.x1); ymin = MAX(ymin, vwk->clip.rectangle.y1);
        xmax = MIN(xmax, vwk->clip.rectangle.x2); ymax = MIN(ymax, vwk->clip.rectangle.y2);
    }
    if (xmin > xmax || ymin > ymax)
        return 1;
    if (indexed && !src->ctab)
    {
        source = *src;
        source.ctab = (COLOR_TAB *)ctab_for_bitmap(vwk, 8);
        src = &source;
    }
    if (indexed && !itab_valid_palette(src->ctab))
        return 1;
    palette = ctab_current(vwk);
    if (!palette || palette->no_colors != (1L << screen.planes))
        return 1;
    table = ctab_inverse(vwk, palette);
    if (!table)
        return 1;
    width = (long)dr->x2 - dr->x1 + 1;
    screen.buffer = (unsigned char *)allocate_block(32);
    if (!screen.buffer)
        return 1;
    screen.capacity = *(long *)screen.buffer;
    if (screen.capacity < 2 * screen.planes)
    {
        free_block(screen.buffer);
        return 1;
    }
    /* Share the pool block with the byte row when possible. Leave at least
     * one aligned planar group; unusually wide rows retain the heap fallback.
     */
    row_offset = (screen.capacity - width) & ~1L;
    if (row_offset >= 2 * screen.planes)
    {
        row = screen.buffer + row_offset;
        screen.capacity = row_offset;
    } else
    {
        row = malloc(width);
        if (!row)
        {
            free_block(screen.buffer);
            return 1;
        }
        allocated_row = 1;
    }
    screen.vwk = vwk;
    screen.clip.x1 = xmin; screen.clip.y1 = ymin;
    screen.clip.x2 = xmax; screen.clip.y2 = ymax;
    memset(&dest, 0, sizeof(dest));
    dest.addr = row; dest.width = width;
    dest.xmin = dr->x1; dest.ymin = dr->y1;
    if (screen.capacity >= 2 * screen.planes)
    {
        /* Render the original rectangle's diffusion stream, not a restarted
         * clipped stream. Only one byte row plus one error row is retained.
         */
        if (indexed)
            remap_index8(src, &dest, sr, dr, table, (mode & 128) != 0, &screen);
        else
            render_index8(src, &dest, sr, dr, table, (mode & 128) != 0, 0, 0, 0, &screen);
    }
    free_block(screen.buffer);
    if (allocated_row) free(row);
    return 1;
}

int CDECL transfer_index8(Virtual *vwk, const GCBITMAP *src, GCBITMAP *dst,
                          const RECT16 *sr, const RECT16 *dr, long mode)
{
    const InverseTable *table;
    unsigned long src_bytes, dst_bytes;
    int indexed, dither = (mode & 128) != 0;
    GCBITMAP source, destination;

    if (!src || !source_size(src))
        return 0;
    if (!dst)
        return transfer_index_screen(vwk, src, sr, dr, mode);
    if (dst->bits != 8)
        return 0;
    indexed = src->px_format == INDEX8;
    mode &= ~128L;
    if ((mode != 0 && mode != 32) || src->bits != source_size(src) * 8 ||
        dst->px_format != INDEX8 || !sr || !dr ||
        sr->x2 < sr->x1 || sr->y2 < sr->y1 || dr->x2 < dr->x1 || dr->y2 < dr->y1)
        return 1;
    src_bytes = bitmap_bytes(src, source_size(src));
    dst_bytes = bitmap_bytes(dst, 1);
    if (!src_bytes || !dst_bytes ||
        sr->x1 < src->xmin || sr->y1 < src->ymin || sr->x2 >= src->xmax || sr->y2 >= src->ymax ||
        dr->x1 < dst->xmin || dr->y1 < dst->ymin || dr->x2 >= dst->xmax || dr->y2 >= dst->ymax ||
        overlaps(src->addr, src_bytes, dst->addr, dst_bytes) ||
        (void *)dst->addr == vwk->real_address->screen.mfdb.address ||
        dst->addr == vwk->real_address->screen.shadow.address)
        return 1;
    /* Preserve the old unpaletted native-size byte copy without pretending to
     * provide device/default colour conversion. The bounds/overlap/mode checks
     * above still apply. Supplying any palette requires the mapped path below.
     */
    if (indexed && !src->ctab && !dst->ctab && !src->itab && !dst->itab && !dither &&
        (long)sr->x2 - sr->x1 == (long)dr->x2 - dr->x1 &&
        (long)sr->y2 - sr->y1 == (long)dr->y2 - dr->y1)
    {
        long row, width = (long)sr->x2 - sr->x1 + 1;
        for (row = 0; row <= (long)sr->y2 - sr->y1; row++)
            memcpy(dst->addr + ((long)dr->y1 - dst->ymin + row) * dst->width +
                              ((long)dr->x1 - dst->xmin),
                   src->addr + ((long)sr->y1 - src->ymin + row) * src->width +
                              ((long)sr->x1 - src->xmin), width);
        return 1;
    }
    if (indexed && !src->ctab)
    {
        source = *src;
        source.ctab = (COLOR_TAB *)ctab_for_bitmap(vwk, 8);
        src = &source;
    }
    if (!dst->ctab)
    {
        destination = *dst;
        destination.ctab = (COLOR_TAB *)ctab_for_bitmap(vwk, 8);
        dst = &destination;
        table = dst->itab ? itab_find(vwk, dst->itab, dst->ctab) : ctab_inverse(vwk, dst->ctab);
    } else
        table = itab_find(vwk, dst->itab, dst->ctab);
    if (!table || (indexed && !itab_valid_palette(src->ctab)))
        return 1;
    if (indexed)
        remap_index8(src, dst, sr, dr, table, dither, 0);
    else
        render_index8(src, dst, sr, dr, table, dither, 0, 0, 0, 0);
    return 1;
}
