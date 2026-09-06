/* Internal colour-table lifecycle. See LICENSE.TXT. */
#ifndef CTAB_H
#define CTAB_H

long ctab_new_id(void);
unsigned long ctab_pixel_format(Virtual *vwk);
long ctab_bytes(long);
const COLOR_TAB *ctab_current(Virtual *);
const COLOR_TAB *ctab_for_bitmap(Virtual *, long);
const struct InverseTable *ctab_inverse(Virtual *, const COLOR_TAB *);
int ctab_default(COLOR_TAB *, long);
COLOR_TAB *ctab_create(Virtual *, long, unsigned long);
int ctab_delete(Virtual *, const COLOR_TAB *);
void ctab_close(Virtual *);

#endif
