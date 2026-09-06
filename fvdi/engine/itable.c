/*
 * Inverse colour tables for the documented NVDI 5 raster interface.
 * Independent implementation, licensed under the GNU General Public License.
 * Please see LICENSE.TXT for further information.
 */
#include <stddef.h>
#include "fvdi.h"
#include "function.h"
#include "relocate.h"
#include "utility.h"
#include "itable.h"

static InverseTable *tables;
static unsigned long next_reference = 1;

int itab_valid_palette(const COLOR_TAB *ctab)
{
    return ctab && !((unsigned long)ctab & 1) &&
           ctab->magic == 0x63746162L && ctab->format == 0 &&
           ctab->color_space == 1 && ctab->no_colors > 0 &&
           ctab->no_colors <= 256 && ctab->length >=
           (long)(offsetof(COLOR_TAB, colors) + ctab->no_colors * sizeof(COLOR_ENTRY));
}

void *itab_create(Virtual *vwk, const COLOR_TAB *ctab, long bits)
{
    InverseTable *table;
    unsigned char *map;
    unsigned long cells, cell;
    long shift, mask, half;

    /* Never recycle references, including after close/delete. A stale ID
     * cannot accidentally identify a later allocation at the same address.
     */
    if (!vwk || !itab_valid_palette(ctab) || bits < 3 || bits > 5 ||
        !next_reference || next_reference > 0xffffffffUL)
        return 0;
    cells = 1UL << (bits * 3);
    table = malloc(sizeof(*table) + ctab->no_colors * sizeof(COLOR_ENTRY) + cells);
    if (!table)
        return 0;
    table->owner = vwk;
    table->count = ctab->no_colors;
    table->bits = bits;
    memcpy(table->colors, ctab->colors, table->count * sizeof(COLOR_ENTRY));
    map = (unsigned char *)(table->colors + table->count);
    shift = 8 - bits;
    mask = (1L << bits) - 1;
    half = 1L << (shift - 1);
    /* RGB8 cell-centre nearest neighbour. Squared distances fit in 32 bits.
     * Resolution is explicit, construction is paid once, and lookup needs
     * neither multiplication nor a palette search in the transfer loop.
     * Equal-distance ties select the first palette entry deterministically.
     */
    for (cell = 0; cell < cells; cell++)
    {
        long r = ((cell >> (2 * bits)) << shift) + half;
        long g = (((cell >> bits) & mask) << shift) + half;
        long b = ((cell & mask) << shift) + half;
        unsigned long best = 0xffffffffUL;
        unsigned short i, index = 0;

        for (i = 0; i < table->count; i++)
        {
            long dr = r - (table->colors[i].rgb.red >> 8);
            long dg = g - (table->colors[i].rgb.green >> 8);
            long db = b - (table->colors[i].rgb.blue >> 8);
            unsigned long distance = dr * dr + dg * dg + db * db;
            if (distance < best)
            {
                best = distance;
                index = i;
            }
        }
        map[cell] = index;
    }
    table->reference = next_reference++;
    table->next = tables;
    tables = table;
    return (void *)table->reference;
}

const InverseTable *itab_find(Virtual *vwk, const void *reference, const COLOR_TAB *ctab)
{
    const InverseTable *table;
    for (table = tables; table; table = table->next)
        if (table->reference == (unsigned long)reference && table->owner == vwk)
        {
            /* The inverse map is a snapshot, not a cache of a mutable
             * caller pointer. Changed palettes require a new inverse table.
             */
            if (!itab_valid_palette(ctab) || ctab->no_colors != table->count ||
                memcmp(ctab->colors, table->colors, table->count * sizeof(COLOR_ENTRY)))
                return 0;
            return table;
        }
    return 0;
}

int itab_delete(Virtual *vwk, const void *reference)
{
    InverseTable **link;
    for (link = &tables; *link; link = &(*link)->next)
        if ((*link)->reference == (unsigned long)reference && (*link)->owner == vwk)
        {
            InverseTable *table = *link;
            *link = table->next;
            free(table);
            return 1;
        }
    return 0;
}

void itab_close(Virtual *vwk)
{
    InverseTable **link = &tables;
    while (*link)
    {
        InverseTable *table = *link;
        if (table->owner == vwk)
        {
            *link = table->next;
            free(table);
        } else
            link = &table->next;
    }
}

int CDECL inverse_table(Virtual *vwk, long subfunction, short *intin, short *intout)
{
    void *reference;
    switch ((int)subfunction)
    {
    case 0:
        memcpy(&reference, intin, sizeof(reference));
        reference = itab_create(vwk, reference, intin[2]);
        memcpy(intout, &reference, sizeof(reference));
        return 2;
    case 1:
        memcpy(&reference, intin, sizeof(reference));
        intout[0] = itab_delete(vwk, reference);
        return 1;
    default:
        return 0;
    }
}
