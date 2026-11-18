#ifndef BLEND_H
#define BLEND_H
/*
 * Blending of images with straight alpha onto 16-bit pixels, for
 * vr_transfer_bits with T_BLEND.  Shared by the engine (engine/transfer.c)
 * and the drivers, which are built with 16-bit ints: every value here is a
 * short or a long, never of the compiler's natural width.
 *
 * The arithmetic, per channel c of red, green and blue (all integers, >>
 * only on values that are not negative).  Wc is the weight colour of
 * vs_weight_color (0-65535), A the source alpha (0-255; 255 for sources
 * without alpha), n the bits of the destination field (5 or 6), d its
 * value and C the 8-bit source component:
 *
 *     Wc' = Wc + (Wc >> 15)                  once per call    0-65536
 *     wa  = A + (A >> 7)                     per pixel        0-256
 *     kc  = (wa * Wc' + 32768) >> 16         per channel      0-256
 *     s   = C >> (8 - n)                     the cut of modes 0 and 32
 *     out = (d * 256 + (s - d) * kc + 128) >> 8
 *
 * A pixel with A = 0 is not read or written.  Every other pixel is written,
 * with 0 in every bit outside the three fields.  See docs/raster.txt.
 *
 * This software is licensed under the GNU General Public License.
 * Please, see LICENSE.TXT for further information.
 */

#include "fvdi.h"

/* Source formats with straight alpha: the long 0xAARRGGBB (bytes A, R, G, B)
 * and bytes R, G, B, A.
 */
#define PX_ARGB32  0x03122020UL
#define PX_RGBA32  0x03022020UL

/* The vr_transfer_bits mode that mixes source and destination by the weight
 * colour, times the source alpha.
 */
#define T_BLEND    64

/* A 16-bit layout: blue in bits 0-4, red's five bits under red_mask, green's
 * five or six bits under green_mask.  A field of the colour 0x00rrggbb is
 * (rgb >> shift) & mask, which keeps the top bits of the component.
 * reversed: the two bytes of a pixel are swapped in memory.
 */
typedef struct {
    unsigned short red_mask, green_mask;
    short red_shift, green_shift, reversed;
} BlendPacking;

#define BLEND_PACKING_565 { 0xf800, 0x07e0, 8, 5, 0 }

#if defined(__GNUC__)
#define BLEND_INLINE static __inline__ __attribute__((always_inline))
#else
#define BLEND_INLINE static
#endif

/* Wc': a weight of 0-65535 as 0-65536, so that full weight multiplies by one */
BLEND_INLINE unsigned long blend_wprime(unsigned short w)
{
    return (unsigned long)w + (w >> 15);
}

/* kc (0-256) for alpha 0-255 and a weight Wc' from blend_wprime().  Below
 * 65536 both factors are words, so one 16 x 16 bit multiply does.
 */
BLEND_INLINE unsigned short blend_k(unsigned short alpha, unsigned long wprime)
{
    unsigned short wa = (unsigned short)(alpha + (alpha >> 7));

    if (wprime >= 65536UL)
        return wa;
    return (unsigned short)(((unsigned long)wa * (unsigned short)wprime + 32768UL) >> 16);
}

/* One field: d and s of the same width (5 or 6 bits), k of 0-256.  The sum
 * equals d * (256 - k) + s * k + 128, 0-16256, and each term fits a short.
 */
BLEND_INLINE unsigned short blend_channel(unsigned short d, unsigned short s, unsigned short k)
{
    short diff = (short)((short)s - (short)d);

    return (unsigned short)(((short)d * 256 + diff * (short)k + 128) >> 8);
}

/* The pixel d, as it is in memory (bytes swapped when p->reversed), blended
 * with the colour rgb (0x00rrggbb) by k[0], k[1], k[2] (red, green, blue).
 * The result is in the same byte order, with the bits outside the fields 0.
 */
BLEND_INLINE unsigned short blend_pixel16(unsigned short d, unsigned long rgb,
                                          const unsigned short k[3], const BlendPacking *p)
{
    short red_at = (short)(19 - p->red_shift);
    short green_top = (short)(16 - p->green_shift);      /* above green's highest bit */
    short green_bits = (short)((p->green_mask >> (green_top - 6)) == 0x3f ? 6 : 5);
    short green_at = (short)(green_top - green_bits);
    unsigned short pixel;

    if (p->reversed)
        d = (unsigned short)((d << 8) | (d >> 8));
    pixel = (unsigned short)(blend_channel((unsigned short)((d & p->red_mask) >> red_at),
                                           (unsigned short)((rgb >> 19) & 0x1f), k[0]) << red_at);
    pixel |= (unsigned short)(blend_channel((unsigned short)((d & p->green_mask) >> green_at),
                                            (unsigned short)((rgb >> (16 - green_bits)) &
                                                             ((1UL << green_bits) - 1)),
                                            k[1]) << green_at);
    pixel |= blend_channel((unsigned short)(d & 0x1f), (unsigned short)((rgb >> 3) & 0x1f), k[2]);
    if (p->reversed)
        pixel = (unsigned short)((pixel << 8) | (pixel >> 8));
    return pixel;
}

/* A driver's blend onto its own screen: w x h pixels of PX_ARGB32 rows,
 * src_pitch bytes apart, to (dst_x, dst_y), already clipped; wprime[] from
 * blend_wprime() for red, green and blue.  It returns 1 when it has drawn,
 * 0 when it leaves the call to the engine.  For the engine only.
 */
typedef long CDECL (*BlendHook)(Virtual *vwk, const unsigned char *src, long src_pitch,
                                long dst_x, long dst_y, long w, long h,
                                const unsigned long wprime[3]);

#endif
