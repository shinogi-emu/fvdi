/* Internal inverse palettes. Independent implementation; see LICENSE.TXT. */
#ifndef ITABLE_H
#define ITABLE_H

typedef struct InverseTable {
    struct InverseTable *next;
    Virtual *owner;
    unsigned long reference;
    unsigned short count, bits;
    /* Snapshot followed immediately by 1 << (3 * bits) byte indices. */
    COLOR_ENTRY colors[];
} InverseTable;

void *itab_create(Virtual *, const COLOR_TAB *, long);
int itab_valid_palette(const COLOR_TAB *);
int itab_delete(Virtual *, const void *);
void itab_close(Virtual *);
const InverseTable *itab_find(Virtual *, const void *, const COLOR_TAB *);

#define ITAB_PIXELS(t) ((const unsigned char *)((t)->colors + (t)->count))

#endif
