/* Colour-table snapshots and ownership. Independent implementation of the
 * documented NVDI interface; GNU GPL, see LICENSE.TXT.
 */
#include <stddef.h>
#include "fvdi.h"
#include "function.h"
#include "relocate.h"
#include "utility.h"
#include "ctab.h"
#include "itable.h"

/* Reuse fVDI's existing public-domain TOS default colours, not another VDI's
 * implementation. Driver modules have their own copy of this data.
 */
#include "../drivers/common/colours.c"

typedef struct ColourTable {
    struct ColourTable *next;
    Virtual *owner;
    int current;
    void *inverse;
    /* A variable-length COLOR_TAB follows this header. */
} ColourTable;

static ColourTable *tables;
/* Values 1..32 identify the immutable default palettes by bit depth. */
static unsigned long next_id = 33;

unsigned long ctab_pixel_format(Virtual *vwk)
{
    Workstation *wk = vwk->real_address;
    const Device *dev = wk->driver ? wk->driver->device : 0;
    long bits = wk->screen.mfdb.bitplanes, i, r, g;
    if (!dev) return 0;
    if (dev->clut == 1 && (bits == 1 || bits == 2 || bits == 4 || bits == 8))
    {
        if (dev->format != 0 && dev->format != 1 && dev->format != 2) return 0;
        return 0x01000000UL | ((dev->format == 2 || bits == 1) ? 0x20000UL :
                              dev->format == 1 ? 0x10000UL : 0) | (bits << 8) | bits;
    }
    if (dev->format != 2) return 0;
    r = dev->scrmap.bitnumber.red[0]; g = dev->scrmap.bitnumber.green[0];
    for (i = 0; i < dev->bits.red; i++)
        if (i >= 8 || dev->scrmap.bitnumber.red[i] != r + i) return 0;
    for (i = 0; i < dev->bits.green; i++)
        if (i >= 8 || dev->scrmap.bitnumber.green[i] != g + i) return 0;
    for (i = 0; i < dev->bits.blue; i++)
        if (i >= 8 || dev->scrmap.bitnumber.blue[i] != i) return 0;
    if (bits == 16 && dev->bits.red == 5 && dev->bits.blue == 5)
    {
        if (r == 11 && g == 5 && dev->bits.green == 6) return 0x03021010UL;
        if (r == 10 && g == 5 && dev->bits.green == 5) return 0x03420f10UL;
    }
    if (r == 16 && g == 8 && dev->bits.red == 8 && dev->bits.green == 8 && dev->bits.blue == 8)
    {
        if (bits == 24) return 0x03021818UL;
        if (bits == 32) return 0x03421820UL;
    }
    /* Do not describe an unrecognized component layout as xRGB32. */
    return 0;
}

long ctab_new_id(void)
{
    if (!next_id || next_id > 0x7fffffffUL)
        return 0;
    return next_id++;
}

long ctab_bytes(long count)
{
    return count > 0 && count <= 256 ?
           offsetof(COLOR_TAB, colors) + count * sizeof(COLOR_ENTRY) : 0;
}

static void header(COLOR_TAB *table, long count, long id)
{
    memset(table, 0, offsetof(COLOR_TAB, colors));
    table->magic = 0x63746162L;
    table->length = ctab_bytes(count);
    table->map_id = id;
    table->color_space = 1;
    table->no_colors = count;
}

static unsigned short component(short value)
{
    long clamped = value < 0 ? 0 : value > 1000 ? 1000 : value;
    return (clamped * 65535UL + 500) / 1000;
}

static COLOR_ENTRY entry(const RGB *rgb)
{
    COLOR_ENTRY result;
    result.rgb.reserved = 0;
    result.rgb.red = component(rgb->red);
    result.rgb.green = component(rgb->green);
    result.rgb.blue = component(rgb->blue);
    return result;
}

int ctab_default(COLOR_TAB *table, long bits)
{
    static const signed char pens[16] = { 0, 2, 3, 6, 4, 7, 5, 8, 9, 10, 11, 14, 12, 15, 13, -1 };
    long count, i;
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 &&
        bits != 15 && bits != 16 && bits != 24 && bits != 32)
        return 0;
    count = bits <= 8 ? 1L << bits : 256;
    header(table, count, bits);
    for (i = 0; i < count; i++)
    {
        long pen = i;
        RGB rgb;
        if (bits <= 8)
        {
            if (i == count - 1) pen = 1;
            else if (i < 16) pen = pens[i] < 0 ? count - 1 : pens[i];
        }
        rgb.red = default_vdi_colors[pen][0];
        rgb.green = default_vdi_colors[pen][1];
        rgb.blue = default_vdi_colors[pen][2];
        table->colors[i] = entry(&rgb);
    }
    return 1;
}

static ColourTable *allocate_table(Virtual *vwk, long count, int current)
{
    ColourTable *node;
    long id = ctab_new_id(), bytes = ctab_bytes(count);
    if (!vwk || !id || !bytes)
        return 0;
    node = malloc(sizeof(*node) + bytes);
    if (!node)
        return 0;
    node->owner = vwk;
    node->current = current;
    node->inverse = 0;
    header((COLOR_TAB *)(node + 1), count, id);
    node->next = tables;
    tables = node;
    return node;
}

const COLOR_TAB *ctab_current(Virtual *vwk)
{
    Workstation *wk = vwk->real_address;
    Colour *palette = vwk->palette;
    ColourTable *node;
    COLOR_TAB *table;
    long count = wk->screen.palette.size, i;
    int changed = 0, fresh = 0;

    if (!ctab_bytes(count) || !wk->driver || !wk->driver->device)
        return 0;
    if (wk->driver->device->clut == 1 || !palette || ((unsigned long)palette & 1))
        palette = wk->screen.palette.colours;
    if (!palette)
        return 0;
    for (node = tables; node; node = node->next)
        if (node->owner == vwk && node->current == 1)
            break;
    if (node && ((COLOR_TAB *)(node + 1))->no_colors != count)
    {
        ColourTable *old = node, **link;
        /* Allocate before discarding the old snapshot, including on failure. */
        node = allocate_table(vwk, count, 1);
        if (!node)
            return 0;
        for (link = &tables; *link != old; link = &(*link)->next)
            ;
        *link = old->next;
        if (old->inverse) itab_delete(vwk, old->inverse);
        free(old);
        fresh = 1;
    }
    if (!node)
    {
        node = allocate_table(vwk, count, 1);
        if (!node)
            return 0;
        fresh = 1;
    }
    table = (COLOR_TAB *)(node + 1);
    for (i = 0; i < count; i++)
    {
        COLOR_ENTRY color = entry(&palette[i].vdi);
        if (fresh || memcmp(&color, &table->colors[i], sizeof(color)))
        {
            changed = 1;
            table->colors[i] = color;
        }
    }
    if (changed && !fresh)
        table->map_id = ctab_new_id();
    return table->map_id ? table : 0;
}

const COLOR_TAB *ctab_for_bitmap(Virtual *vwk, long bits)
{
    ColourTable *node;
    if (bits == vwk->real_address->screen.mfdb.bitplanes)
        return ctab_current(vwk);
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8)
        return 0;
    for (node = tables; node; node = node->next)
        if (node->owner == vwk && node->current == 64 + bits)
            return (const COLOR_TAB *)(node + 1);
    node = allocate_table(vwk, 1L << bits, 64 + bits);
    if (!node)
        return 0;
    ctab_default((COLOR_TAB *)(node + 1), bits);
    return (const COLOR_TAB *)(node + 1);
}

const InverseTable *ctab_inverse(Virtual *vwk, const COLOR_TAB *table)
{
    ColourTable *node;
    for (node = tables; node; node = node->next)
        if (node->owner == vwk && node->current &&
            (const void *)(node + 1) == (const void *)table)
        {
            const InverseTable *inverse = itab_find(vwk, node->inverse, table);
            void *replacement;
            if (inverse)
                return inverse;
            replacement = itab_create(vwk, table, 4);
            if (!replacement)
                return 0;
            if (node->inverse) itab_delete(vwk, node->inverse);
            node->inverse = replacement;
            return itab_find(vwk, replacement, table);
        }
    return 0;
}

COLOR_TAB *ctab_create(Virtual *vwk, long space, unsigned long format)
{
    long bits = format & 255;
    ColourTable *node;
    COLOR_TAB *table;
    long id;
    if ((space != 0 && space != 1) ||
        (bits != 1 && bits != 2 && bits != 4 && bits != 8 &&
         bits != 15 && bits != 16 && bits != 24 && bits != 32))
        return 0;
    node = allocate_table(vwk, bits <= 8 ? 1L << bits : 256, 0);
    if (!node)
        return 0;
    table = (COLOR_TAB *)(node + 1);
    id = table->map_id;
    ctab_default(table, bits);
    table->map_id = id;
    return table;
}

int ctab_delete(Virtual *vwk, const COLOR_TAB *table)
{
    ColourTable **link;
    for (link = &tables; *link; link = &(*link)->next)
        if ((*link)->owner == vwk && !(*link)->current &&
            (const void *)((*link) + 1) == (const void *)table)
        {
            ColourTable *node = *link;
            *link = node->next;
            free(node);
            return 1;
        }
    return 0;
}

void ctab_close(Virtual *vwk)
{
    ColourTable **link = &tables;
    while (*link)
    {
        ColourTable *node = *link;
        if (node->owner == vwk)
        {
            *link = node->next;
            if (node->inverse) itab_delete(vwk, node->inverse);
            free(node);
        } else
            link = &node->next;
    }
}
