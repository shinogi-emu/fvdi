/* Preserve unrelated settings and replace files only after a verified write.
 * Uses GEMDOS Frename directly on Atari, so an existing backup is not replaced.
 * See LICENSE.TXT.
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifdef __m68k__
#include <mint/osbind.h>
#endif

static int fail(char *error,size_t n,const char *text)
{
    if(n)snprintf(error,n,"%s",text);
    return 0;
}
static int same(const char *a,const char *b)
{
    while(*a&&*b){if(tolower((unsigned char)*a++)!=tolower((unsigned char)*b++))return 0;}
    return !*a&&!*b;
}
static const char *base_name(const char *p)
{
    const char *base=p;
    while(*p){if(*p=='/'||*p=='\\'||*p==':')base=p+1;p++;}
    return base;
}
/* Token boundaries match fVDI's quoted strings and separate '=' token. */
static const char *token(const char *p,char *out,size_t n)
{
    size_t i=0;
    int quoted=0;
    while(*p==' '||*p=='\t')p++;
    if(!*p||*p=='#'){out[0]=0;return p;}
    if(*p=='='){if(n>1){out[0]='=';out[1]=0;}return p+1;}
    if(*p=='"'){quoted=1;p++;}
    while(*p&&(quoted?*p!='"':!isspace((unsigned char)*p)&&*p!='=')){
        if(i+1>=n)return NULL;
        out[i++]=*p++;
    }
    if(quoted){if(*p!='"')return NULL;p++;}
    out[i]=0;return p;
}
static int read_file(const char *path,char **data,size_t *length)
{
    FILE *f=fopen(path,"rb");long n;char *p;
    if(!f)return 0;
    if(fseek(f,0,SEEK_END)||((n=ftell(f))<0)||n>FVSET_MAX_FILE||fseek(f,0,SEEK_SET)){fclose(f);return 0;}
    p=malloc(n+1);if(!p){fclose(f);return 0;}
    if(fread(p,1,n,f)!=(size_t)n||ferror(f)){free(p);fclose(f);return 0;}
    if(fclose(f)){free(p);return 0;}
    if(memchr(p,0,n)){free(p);return 0;}
    p[n]=0;*data=p;*length=n;return 1;
}
void fvset_dispose(FvSettings *s)
{
    free(s->original);memset(s,0,sizeof(*s));
}
static int line_text(const char **next,char *line,size_t cap,const char **start,size_t *raw)
{
    const char *p=*next,*e=p;
    size_t n;
    if(!*p)return 0;
    while(*e&&*e!='\r'&&*e!='\n')e++;
    n=e-p;if(n>=cap)return -1;
    memcpy(line,p,n);line[n]=0;*start=p;
    if(*e=='\r')e++;
    if(*e=='\n')e++;
    *raw=e-p;*next=e;return 1;
}
int fvset_mode_valid(int platform,int w,int h,int depth,int hz)
{
    if(platform==FVSET_ARANYM)
        return w>=320&&w<=1920&&h>=200&&h<=1200&&w%8==0&&
               (depth==8||depth==16||depth==32)&&hz>=50&&hz<=85;
    if(platform==FVSET_SAGA)
        return depth==16&&hz==60&&((w==640&&h==480)||(w==800&&h==600)||
               (w==1024&&h==768)||(w==1280&&h==720));
    if(platform==FVSET_SHINOGI)
        return depth==16&&w>=320&&w<=1920&&h>=200&&h<=1080&&w%8==0;
    return 0;
}
int fvset_load(FvSettings *s,const char *path,char *error,size_t en)
{
    FvSettings t;
    const char *p,*start,*v;
    char line[1024],a[160],b[160];size_t raw;
    int rc,driver=0,fonts=0;
    memset(&t,0,sizeof(t));t.width=640;t.height=480;t.depth=16;t.refresh=60;
    if(strlen(path)>=sizeof(t.path))return fail(error,en,"Configuration path is too long.");
    if(!read_file(path,&t.original,&t.length))return fail(error,en,"Cannot read configuration (limit 64 KB).");
    strcpy(t.path,path);p=t.original;
    while((rc=line_text(&p,line,sizeof(line),&start,&raw))>0){
        v=token(line,a,sizeof(a));if(!v)goto bad;
        if(same(a,"fonts")){
            if(!driver)goto bad;
            if(fonts++)continue;
            v=token(v,b,sizeof(b));if(!v||strcmp(b,"="))goto bad;
            v=token(v,t.fonts,sizeof(t.fonts));if(!v||!t.fonts[0])goto bad;
        }else if(same(a,"antialias"))t.antialias=1;
        else if(same(a,"cookie")){
            if(!token(v,b,sizeof(b)))goto bad;
            if(same(b,"nvdi"))t.nvdi=1;
        }else if(strlen(a)>=2&&a[0]=='0'&&a[1]=='1'&&
                 (!a[2]||same(a+2,"r")||same(a+2,"p"))){
            if(driver++)goto bad;
            v=token(v,t.driver,sizeof(t.driver));if(!v||!t.driver[0])goto bad;
            if(same(base_name(t.driver),"aranym.sys"))t.platform=FVSET_ARANYM;
            else if(same(base_name(t.driver),"saga.sys"))t.platform=FVSET_SAGA;
            while(1){
                v=token(v,a,sizeof(a));if(!v)goto bad;
                if(!*a)break;
                if(same(a,"mode")){
                    v=token(v,b,sizeof(b));if(!v)goto bad;
                    if(sscanf(b,"%dx%dx%d@%d",&t.width,&t.height,&t.depth,&t.refresh)!=4)goto bad;
                }
            }
        }
    }
    if(rc<0||!driver)goto bad;
    strcpy(t.old_fonts,t.fonts);t.old_antialias=t.antialias;t.old_nvdi=t.nvdi;
    fvset_dispose(s);*s=t;return 1;
bad:
    free(t.original);return fail(error,en,"Unsupported config syntax. Original file was kept.");
}
typedef struct {char *data;size_t size,capacity;} Output;
static int add(Output *o,const char *p,size_t n)
{
    if(n>=o->capacity-o->size)return 0;
    memcpy(o->data+o->size,p,n);o->size+=n;o->data[o->size]=0;return 1;
}
static int text(Output *o,const char *p){return add(o,p,strlen(p));}
static const char *comment(const char *line)
{
    int quoted=0;
    while(*line){if(*line=='"')quoted=!quoted;else if(*line=='#'&&!quoted)return line;line++;}
    return line;
}
static int font_path_ok(const char *p)
{
    size_t n=strlen(p);size_t i;
    if(!n||n>=FVSET_FONT)return 0;
    for(i=0;i<n;i++)if((unsigned char)p[i]<32||p[i]=='"'||p[i]=='#')return 0;
    return 1;
}
char *fvset_render(const FvSettings *s,size_t *length,char *error,size_t en)
{
    const char *p=s->original,*start,*v,*end;
    char line[1024],a[160],b[160],replacement[256];size_t raw;
    int rc,got_font=0,got_aa=0,got_nvdi=0;
    Output o;
    if(!s->original){fail(error,en,"Load a configuration first.");return NULL;}
    if(strcmp(s->fonts,s->old_fonts)&&!font_path_ok(s->fonts)){fail(error,en,"Choose a valid font folder.");return NULL;}
    if(s->mode_changed&&!fvset_mode_valid(s->platform,s->width,s->height,s->depth,s->refresh)){fail(error,en,"This display mode is not supported by the selected driver.");return NULL;}
    o.capacity=s->length+4096;o.size=0;o.data=malloc(o.capacity);if(!o.data){fail(error,en,"Not enough memory to prepare the settings.");return NULL;}o.data[0]=0;
    while((rc=line_text(&p,line,sizeof(line),&start,&raw))>0){
        v=token(line,a,sizeof(a));if(!v)goto bad;
        if(same(a,"fonts")&&!got_font++){
            if(strcmp(s->fonts,s->old_fonts)){
                snprintf(replacement,sizeof(replacement),"fonts = \"%s\"",s->fonts);
                if(!text(&o,replacement)||!text(&o," ")||!text(&o,comment(line))||!text(&o,"\r\n"))goto bad;
                continue;
            }
        }else if(same(a,"antialias")){
            got_aa=1;
            if(s->antialias!=s->old_antialias){
                if(s->antialias&&!text(&o,"antialias "))goto bad;
                if(!text(&o,comment(line))||!text(&o,"\r\n"))goto bad;
                continue;
            }
        }else if(same(a,"cookie")){
            if(!token(v,b,sizeof(b)))goto bad;
            if(same(b,"nvdi")){
                got_nvdi=1;
                if(s->nvdi!=s->old_nvdi){
                    if(s->nvdi&&!text(&o,"cookie nvdi = $0501 "))goto bad;
                    if(!text(&o,comment(line))||!text(&o,"\r\n"))goto bad;
                    continue;
                }
            }
        }else if(s->mode_changed&&s->platform!=FVSET_SHINOGI&&a[0]=='0'&&a[1]=='1'){
            /* Keep every other driver option, including its inline comment. */
            const char *mode_start=NULL,*mode_end=NULL,*q=v;
            q=token(q,b,sizeof(b));if(!q)goto bad;
            while(1){const char *before=q;q=token(q,b,sizeof(b));if(!q)goto bad;if(!*b)break;
                if(same(b,"mode")){mode_start=before;q=token(q,b,sizeof(b));if(!q)goto bad;mode_end=q;break;}}
            snprintf(replacement,sizeof(replacement)," mode %dx%dx%d@%d",s->width,s->height,s->depth,s->refresh);
            end=mode_start?mode_start:comment(line);
            if(!add(&o,line,end-line)||!text(&o,replacement)||!text(&o," ")||
               !text(&o,mode_end?mode_end:end)||!text(&o,"\r\n"))goto bad;
            continue;
        }
        if(!add(&o,start,raw))goto bad;
    }
    if(rc<0)goto bad;
    if(((!got_font&&s->fonts[0])||(!got_aa&&s->antialias)||(!got_nvdi&&s->nvdi))&&o.size&&o.data[o.size-1]!='\n'&&o.data[o.size-1]!='\r')if(!text(&o,"\r\n"))goto bad;
    if(!got_font&&s->fonts[0]){snprintf(replacement,sizeof(replacement),"fonts = \"%s\"\r\n",s->fonts);if(!text(&o,replacement))goto bad;}
    if(!got_aa&&s->antialias&&!text(&o,"antialias\r\n"))goto bad;
    if(!got_nvdi&&s->nvdi&&!text(&o,"cookie nvdi = $0501\r\n"))goto bad;
    *length=o.size;return o.data;
bad:
    free(o.data);fail(error,en,"Could not prepare configuration; nothing was changed.");return NULL;
}
/* GEMDOS refuses an existing destination. Use the direct binding rather than
 * a libc rename which may delete the destination to provide POSIX semantics. */
static int move_new(const char *from,const char *to)
{
#ifdef __m68k__
    return Frename(0,from,to)==0;
#else
    if(link(from,to))return 0;
    if(unlink(from)){unlink(to);return 0;}
    return 1;
#endif
}
static int side_name(const char *path,char kind,int i,char *out,size_t n)
{
    const char *base=base_name(path),*p=base;size_t prefix=base-path,stem=0;
    while(*p&&*p!='.'&&stem<8){stem++;p++;}
    if(prefix+stem+5>n)return 0;
    memcpy(out,path,prefix);memcpy(out+prefix,base,stem);
    snprintf(out+prefix+stem,n-prefix-stem,".%c%02d",kind,i);return 1;
}
static int store(const char *path,const char *old,size_t old_n,const char *data,size_t n,
                 char *backup,size_t bn,char *error,size_t en)
{
    char temp[FVSET_PATH],bak[FVSET_PATH],*current=NULL,*check=NULL;
    size_t cn;int fd=-1,i,ok=0,had_old=old!=NULL;size_t done=0;
    if(bn)backup[0]=0;
    if(had_old){
        if(!read_file(path,&current,&cn)||cn!=old_n||memcmp(current,old,cn)){free(current);return fail(error,en,"Config changed since loading. Reload before saving.");}
        free(current);
    }else if(access(path,F_OK)==0)return fail(error,en,"The file appeared since loading. Reload first.");
    for(i=0;i<100;i++){
        if(!side_name(path,'$',i,temp,sizeof(temp)))return fail(error,en,"Configuration path is too long.");
        fd=open(temp,O_CREAT|O_EXCL|O_WRONLY|O_BINARY,0600);
        if(fd>=0)break;
        if(errno!=EEXIST)return fail(error,en,"Cannot create a temporary file beside the config.");
    }
    if(fd<0)return fail(error,en,"No free temporary filename.");
    while(done<n){ssize_t amount=write(fd,data+done,n-done);if(amount<=0)break;done+=amount;}
    if(close(fd)||done!=n)goto failed;
    if(!read_file(temp,&check,&cn)||cn!=n||memcmp(check,data,n)){free(check);goto failed;}free(check);
    /* Recheck after preparing the new file, before moving the original. */
    if(had_old){
        current=NULL;if(!read_file(path,&current,&cn)||cn!=old_n||memcmp(current,old,cn)){free(current);unlink(temp);return fail(error,en,"Config changed during save. Reload before saving.");}free(current);
        for(i=0;i<100;i++){
            if(!side_name(path,'B',i,bak,sizeof(bak)))goto failed;
            if(access(bak,F_OK)==0)continue;
            if(move_new(path,bak)){ok=1;break;}
            if(access(bak,F_OK)!=0)goto failed;
        }
        if(!ok)goto failed;
        if(bn)snprintf(backup,bn,"%s",bak);
    }
    if(!move_new(temp,path)){
        if(had_old&&!move_new(bak,path)){unlink(temp);return fail(error,en,"Save failed. Original remains in the displayed backup.");}
        goto failed;
    }
    check=NULL;
    if(!read_file(path,&check,&cn)||cn!=n||memcmp(check,data,n)){
        free(check);
        return fail(error,en,"Saved file could not be verified. Keep the displayed backup.");
    }
    free(check);
    return 1;
failed:
    unlink(temp);return fail(error,en,"Save failed. Original configuration was kept.");
}
int fvset_save(FvSettings *s,char *backup,size_t bn,char *error,size_t en)
{
    char *data;size_t n;
    if(bn)backup[0]=0;
    data=fvset_render(s,&n,error,en);if(!data)return 0;
    if(n==s->length&&!memcmp(data,s->original,n)){
        char *current=NULL;size_t current_n;
        int valid=read_file(s->path,&current,&current_n)&&current_n==s->length&&!memcmp(current,s->original,current_n);
        free(current);free(data);if(bn)backup[0]=0;
        return valid?1:fail(error,en,"Config changed since loading. Reload before saving.");
    }
    if(!store(s->path,s->original,s->length,data,n,backup,bn,error,en)){free(data);return 0;}
    free(s->original);s->original=data;s->length=n;s->old_antialias=s->antialias;s->old_nvdi=s->nvdi;strcpy(s->old_fonts,s->fonts);s->mode_changed=0;
    return 1;
}
int fvset_resolution(const char *path,int w,int h,char *backup,size_t bn,char *error,size_t en)
{
    char *old=NULL,line[1024],a[160],replacement[64];const char *p,*start,*v;size_t old_n=0,raw;Output o;int rc,found=0,result;
    if(bn)backup[0]=0;
    if(!fvset_mode_valid(FVSET_SHINOGI,w,h,16,60))return fail(error,en,"Unsupported Shinogi resolution.");
    if(access(path,F_OK)==0&&!read_file(path,&old,&old_n))return fail(error,en,"Cannot read SHINOGI.INI.");
    o.capacity=old_n+128;o.size=0;o.data=malloc(o.capacity);if(!o.data){free(old);return fail(error,en,"Not enough memory to prepare launcher settings.");}o.data[0]=0;p=old?old:"";
    while((rc=line_text(&p,line,sizeof(line),&start,&raw))>0){
        v=token(line,a,sizeof(a));if(!v)goto bad;
        if(same(a,"res")){
            if(!found++){snprintf(replacement,sizeof(replacement),"res=%dx%d\r\n",w,h);if(!text(&o,replacement))goto bad;}
        }else if(!add(&o,start,raw))goto bad;
    }
    if(rc<0)goto bad;
    if(!found){if(o.size&&o.data[o.size-1]!='\n'&&!text(&o,"\r\n"))goto bad;snprintf(replacement,sizeof(replacement),"res=%dx%d\r\n",w,h);if(!text(&o,replacement))goto bad;}
    result=store(path,old,old_n,o.data,o.size,backup,bn,error,en);free(o.data);free(old);return result;
bad:
    free(o.data);free(old);return fail(error,en,"Unsupported launcher config. Original was kept.");
}
