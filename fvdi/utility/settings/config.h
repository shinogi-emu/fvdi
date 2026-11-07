/* Portable fVDI settings document. See LICENSE.TXT. */
#ifndef FVSET_CONFIG_H
#define FVSET_CONFIG_H
#include <stddef.h>
#define FVSET_PATH 128
#define FVSET_FONT 160
#define FVSET_MAX_FILE 65536L
#define FVSET_BOOT 0
#define FVSET_ARANYM 1
#define FVSET_SAGA 2
#define FVSET_SHINOGI 3
typedef struct {
    char path[FVSET_PATH],driver[FVSET_PATH],fonts[FVSET_FONT];
    char old_fonts[FVSET_FONT];
    char *original;
    size_t length;
    int antialias,nvdi,old_antialias,old_nvdi;
    int platform,width,height,depth,refresh,mode_changed;
} FvSettings;
void fvset_dispose(FvSettings *);
int fvset_load(FvSettings *,const char *,char *,size_t);
char *fvset_render(const FvSettings *,size_t *,char *,size_t);
int fvset_save(FvSettings *,char *,size_t,char *,size_t);
int fvset_resolution(const char *,int,int,char *,size_t,char *,size_t);
int fvset_mode_valid(int,int,int,int,int);
#endif
