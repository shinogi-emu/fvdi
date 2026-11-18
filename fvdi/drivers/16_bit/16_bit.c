#if 1
#define FAST		/* Write in FastRAM buffer */
#define BOTH		/* Write in both FastRAM and on screen */
#else
#undef FAST
#undef BOTH
#endif

#include "../16_bit/16b_exp.c"
#include "../16_bit/16b_blit.c"
#include "../16_bit/16b_line.c"
#include "../16_bit/16b_fill.c"
#include "../16_bit/16b_scr.c"
/* before 16b_pal.c, whose green_bits macro would rename a variable of blend.h */
#include "../16_bit/16b_blend.c"
#include "../16_bit/16b_pal.c"
#include "../16_bit/16b_spec.c"
#include "../16_bit/16b_mouse.c"
