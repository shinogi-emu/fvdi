/*
 * T_BLEND straight onto the screen of the 16-bit driver and its shadow.
 *
 * vr_transfer_bits with T_BLEND (64) mixes images with straight alpha into
 * the screen (see docs/raster.txt).  Without help from the driver the
 * engine reads each strip back through the driver's blit, blends it and
 * writes it back.  This driver offers the engine its own blend instead,
 * through setup(Q_BLEND_ARGB, 0) (16b_spec.c): rows of PX_ARGB32 pixels
 * that the engine has clipped, mixed into the screen in place.
 *
 * The arithmetic is include/blend.h's, the same as the engine's.  A pixel
 * with alpha 0 is not touched, on the screen or in the shadow.  Every other
 * pixel is read from the shadow when there is one, else from the screen,
 * and written to both.  The screen is RGB565, this driver's only layout.
 *
 * Built with 16-bit ints as part of 16_bit.c, and on its own by the host
 * checks (tests/alpha.c), which run on machines of either byte order: there
 * the pixels and the source longs are taken byte by byte, big-endian, as
 * the 68k stores them.  Shorts and longs only (the checks' noint target).
 *
 * This software is licensed under the GNU General Public License.
 * Please, see LICENSE.TXT for further information.
 */

#include "fvdi.h"
#include "driver.h"
#include "blend.h"

#if defined(__m68k__)
typedef unsigned long BlendLong __attribute__((may_alias));
typedef unsigned short BlendWord __attribute__((may_alias));
#define ARGB_AT(p)        (*(const BlendLong *)(p))
#define PIXEL_AT(p)       (*(const BlendWord *)(p))
#define PIXEL_SET(p, v)   (*(BlendWord *)(p) = (v))
#else
#define ARGB_AT(p)        (((unsigned long)(p)[0] << 24) | ((unsigned long)(p)[1] << 16) | \
                           ((unsigned long)(p)[2] << 8) | (p)[3])
#define PIXEL_AT(p)       ((unsigned short)(((p)[0] << 8) | (p)[1]))
#define PIXEL_SET(p, v)   ((p)[0] = (unsigned char)((v) >> 8), (p)[1] = (unsigned char)(v))
#endif

long CDECL c_blend_argb(Virtual *vwk, const unsigned char *src, long src_pitch,
                        long dst_x, long dst_y, long w, long h, const unsigned long wprime[3]);

/*
 * w x h pixels of PX_ARGB32 (the long 0xAARRGGBB), rows src_pitch bytes
 * apart from src, blended at (dst_x, dst_y) of the screen of vwk with the
 * weights wprime[] of blend_wprime() (red, green, blue).  The rectangle is
 * already clipped to the screen; it is not checked again.  Returns 1 when
 * drawn, 0 when left to the engine: a screen without an address, or not of
 * 16 bits.  The engine's BlendHook, not a call for programs.
 */
long CDECL c_blend_argb(Virtual *vwk, const unsigned char *src, long src_pitch,
                        long dst_x, long dst_y, long w, long h, const unsigned long wprime[3])
{
    static const BlendPacking packing = BLEND_PACKING_565;
    Workstation *wk = vwk->real_address;
    unsigned char *screen_row, *shadow_row;
    unsigned long argb, w0 = wprime[0], w1 = wprime[1], w2 = wprime[2];
    unsigned short k[3], alpha, last = 256, opaque = 0, pixel;
    long wrap, offset, n;

    if (!wk->screen.mfdb.address || wk->screen.mfdb.bitplanes != 16)
        return 0;
    if (w <= 0 || h <= 0)
        return 1;
    wrap = wk->screen.wrap;
    offset = dst_y * wrap + dst_x * 2;
    screen_row = (unsigned char *)wk->screen.mfdb.address + offset;
    shadow_row = wk->screen.shadow.address ? (unsigned char *)wk->screen.shadow.address + offset : 0;
    k[0] = k[1] = k[2] = 0;
    while (h-- > 0)
    {
        const unsigned char *s = src;
        unsigned char *d = screen_row, *f = shadow_row;

        for (n = w; n > 0; n--)
        {
            argb = ARGB_AT(s);
            s += 4;
            alpha = (unsigned short)(argb >> 24);
            if (alpha)
            {
                if (alpha != last)
                {
                    last = alpha;
                    k[0] = blend_k(alpha, w0);
                    k[1] = blend_k(alpha, w1);
                    k[2] = blend_k(alpha, w2);
                    /* k = 256 in every channel: the result is the source's cut */
                    opaque = (k[0] & k[1] & k[2]) == 256;
                }
                if (opaque)
                    pixel = (unsigned short)(((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) | ((argb >> 3) & 0x1f));
                else
                    pixel = blend_pixel16(f ? PIXEL_AT(f) : PIXEL_AT(d), argb, k, &packing);
                PIXEL_SET(d, pixel);
                if (f)
                    PIXEL_SET(f, pixel);
            }
            d += 2;
            if (f)
                f += 2;
        }
        src += src_pitch;
        screen_row += wrap;
        if (shadow_row)
            shadow_row += wrap;
    }
    return 1;
}
