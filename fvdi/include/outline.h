#ifndef OUTLINE_H
#define OUTLINE_H

/*
 * v_getoutline's working data, passed between the engine and the FreeType
 * module.  Longs only, so both sides agree on the layout whatever size an
 * int is where each is compiled.
 */
typedef struct {
    short *xy;                  /* caller's points */
    char *bez;                  /* caller's v_bez flags */
    long max;                   /* room for this many points */
    long count;                 /* points written */
    long advance;               /* the character's advance, pixels */
    long overflow;              /* ran out of room */
    long last_x, last_y;        /* FreeType's working state, 26.6 */
    long start_x, start_y;
    long open;
} OutlineArgs;

#endif
