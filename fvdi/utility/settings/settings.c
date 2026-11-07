/* A resource-free GEM settings app for fVDI on Atari systems and emulators.
 * See LICENSE.TXT. Configuration changes apply after restarting the guest.
 */
#include <gem.h>
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"

enum {FV_ROOT,FV_TITLE,FV_CONFIG,FV_DRIVER,FV_FONTS,FV_FONTNAME,FV_AA,FV_NVDI,FV_DISPLAY,FV_LOAD,FV_SAVE,FV_CLOSE,FV_COUNT};
static OBJECT tree[FV_COUNT],modes[9];
static FvSettings settings;
static short cw,ch,bw,bh;
static int dirty,shinogi_available;
static char config_label[37],driver_label[37],font_label[35],aa_label[35],nvdi_label[35];
static char mode_label[39],depth_label[39],refresh_label[39],platform_label[39],ini[FVSET_PATH];
static long cookie_id,cookie_value;
static int cookie_found;

static long boot_drive(void){return *(volatile unsigned short *)0x446;}
static long query_cookie(void)
{
    volatile long *p=*(volatile long **)0x5a0;
    cookie_found=0;cookie_value=0;
    while(p&&p[0]){if(p[0]==cookie_id){cookie_found=1;cookie_value=p[1];break;}p+=2;}
    return 0;
}
static void alert(const char *text)
{
    char b[240];size_t i,j=4;int column=0,lines=1;
    strcpy(b,"[1][");
    for(i=0;text[i]&&j<sizeof(b)-12;i++){
        if(column==27){if(lines==5){strcpy(b+j,"...");j+=3;break;}b[j++]='|';column=0;lines++;}
        b[j++]=(text[i]=='['||text[i]==']'||text[i]=='|')?' ':text[i];column++;
    }
    strcpy(b+j,"][OK]");form_alert(1,b);
}
static int discard(void){return !dirty||form_alert(2,"[2][Discard unsaved changes?][Discard|Keep editing]")==1;}
static void shorten(char *out,size_t n,const char *prefix,const char *path)
{
    size_t p=strlen(prefix),len=strlen(path);
    if(p+len<n)snprintf(out,n,"%s%s",prefix,path);
    else snprintf(out,n,"%s...%s",prefix,path+len-(n-p-4));
}
static void object(OBJECT *t,int id,int type,int flags,char *label,int x,int y,int w)
{
    t[id].ob_next=t[id].ob_head=t[id].ob_tail=-1;t[id].ob_type=type;t[id].ob_flags=flags;
    t[id].ob_state=OS_NORMAL;t[id].ob_spec.free_string=label;t[id].ob_x=x*cw;t[id].ob_y=y*ch;t[id].ob_width=w*cw;t[id].ob_height=ch;
    if(id)objc_add(t,0,id);
}
static void root(OBJECT *t,int count)
{
    memset(t,0,count*sizeof(*t));object(t,0,G_BOX,0,NULL,0,0,38);t[0].ob_height=11*ch;t[0].ob_spec.index=0x00001100L;
}
static void button(OBJECT *t,int id,char *label,int x,int y,int w)
{
    object(t,id,G_BUTTON,OF_SELECTABLE|OF_EXIT,label,x,y,w);
}
static void open_form(OBJECT *t,short *x,short *y,short *w,short *h)
{
    form_center(t,x,y,w,h);form_dial(FMD_START,0,0,0,0,*x,*y,*w,*h);objc_draw(t,0,MAX_DEPTH,*x,*y,*w,*h);
}
static void close_form(short x,short y,short w,short h){form_dial(FMD_FINISH,0,0,0,0,x,y,w,h);}
static int choose_file(const char *current,const char *pattern,const char *title,char *result)
{
    char path[FVSET_PATH],file[14];const char *base=current,*p;size_t prefix;short ok;
    for(p=current;*p;p++)if(*p=='\\'||*p=='/'||*p==':')base=p+1;
    prefix=base-current;if(prefix+strlen(pattern)>=sizeof(path)||strlen(base)>=sizeof(file))return 0;
    memcpy(path,current,prefix);strcpy(path+prefix,pattern);strcpy(file,base);
    if(!fsel_exinput(path,file,&ok,(char *)title)||!ok||!file[0])return 0;
    base=path;for(p=path;*p;p++)if(*p=='\\'||*p=='/'||*p==':')base=p+1;
    prefix=base-path;if(prefix+strlen(file)>=FVSET_PATH)return 0;
    memcpy(result,path,prefix);strcpy(result+prefix,file);return 1;
}
static int load_config(const char *path)
{
    char error[180],driver[128];const char *base;FILE *f;
    if(!fvset_load(&settings,path,error,sizeof(error))){alert(error);return 0;}
    base=settings.driver;for(const char *p=base;*p;p++)if(*p=='\\'||*p=='/'||*p==':')base=p+1;
    snprintf(driver,sizeof(driver),"%s",base);for(char *p=driver;*p;p++)if(*p>='A'&&*p<='Z')*p+='a'-'A';
    cookie_id=0x5f4d4348L;Supexec(query_cookie);
    shinogi_available=cookie_found&&cookie_value==-1L&&!strcmp(driver,"16_bit.sys");
    if(shinogi_available&&(f=fopen(ini,"r"))!=NULL){char line[256];settings.platform=FVSET_SHINOGI;while(fgets(line,sizeof(line),f)){int w,h;if(sscanf(line," res = %dx%d",&w,&h)==2&&fvset_mode_valid(FVSET_SHINOGI,w,h,16,60)){settings.width=w;settings.height=h;}}fclose(f);settings.depth=16;}
    if(settings.platform==FVSET_BOOT){
        short in[11]={1,1,1,1,1,1,1,1,1,1,2},out[57],v=graf_handle(&cw,&ch,&bw,&bh);
        v_opnvwk(in,&v,out);
        if(v){settings.width=out[0]+1;settings.height=out[1]+1;vq_extnd(v,1,out);settings.depth=out[4];v_clsvwk(v);}
    }
    dirty=0;return 1;
}
static void labels(void)
{
    shorten(config_label,sizeof(config_label),"Config: ",settings.original?settings.path:"choose a file");
    shorten(driver_label,sizeof(driver_label),"Driver: ",settings.original?settings.driver:"unknown");
    shorten(font_label,sizeof(font_label),"",settings.fonts[0]?settings.fonts:"No outline font folder configured");
    snprintf(aa_label,sizeof(aa_label),"Smooth outline text: %s",settings.antialias?"On":"Off");
    snprintf(nvdi_label,sizeof(nvdi_label),"NVDI cookie: %s",settings.nvdi?"On":"Off");
}
static void display_settings(void)
{
    static const int sizes[][2]={{640,480},{800,600},{1024,768},{1280,720},{1280,1024},{1600,900},{1920,1080}};
    short x,y,w,h;int done=0,id;
    root(modes,9);object(modes,1,G_STRING,0,"Display settings",2,0,34);
    button(modes,2,platform_label,2,2,34);button(modes,3,mode_label,2,4,34);button(modes,4,depth_label,2,5,34);button(modes,5,refresh_label,2,6,34);
    object(modes,6,G_STRING,0,"Changes apply after restarting.",2,8,34);button(modes,7,"Back",13,9,12);modes[7].ob_flags|=OF_DEFAULT|OF_LASTOB;
    open_form(modes,&x,&y,&w,&h);
    while(!done){int p=settings.platform,enabled=p!=FVSET_BOOT;
        snprintf(platform_label,sizeof(platform_label),"%s",p==FVSET_ARANYM?"ARAnyM driver mode":p==FVSET_SAGA?"SAGA driver mode":p==FVSET_SHINOGI?"Shinogi launcher mode":shinogi_available?"Set up Shinogi display...":"Uses the desktop's boot video mode");
        snprintf(mode_label,sizeof(mode_label),"Resolution: %d x %d",settings.width,settings.height);
        snprintf(depth_label,sizeof(depth_label),"Colour depth: %d bits",settings.depth);
        snprintf(refresh_label,sizeof(refresh_label),"Refresh: %d Hz",settings.refresh);
        modes[2].ob_state=shinogi_available&&p==FVSET_BOOT?OS_NORMAL:OS_DISABLED;
        modes[3].ob_state=enabled?OS_NORMAL:OS_DISABLED;
        modes[4].ob_state=p==FVSET_ARANYM?OS_NORMAL:OS_DISABLED;
        modes[5].ob_state=p==FVSET_ARANYM?OS_NORMAL:OS_DISABLED;
        objc_draw(modes,0,MAX_DEPTH,x,y,w,h);id=form_do(modes,0)&0x7fff;if(id<1||id>7){done=1;continue;}modes[id].ob_state&=~OS_SELECTED;
        if(id==7)done=1;
        else if(id==2&&shinogi_available&&p==FVSET_BOOT){if(form_alert(2,"[2][Use the Shinogi launcher|for this setup's screen mode?][Yes|Cancel]")==1){settings.platform=FVSET_SHINOGI;settings.depth=16;settings.mode_changed=0;}}
        else if(id==3&&enabled){int i,start=0;for(i=0;i<7;i++)if(sizes[i][0]==settings.width&&sizes[i][1]==settings.height)start=i+1;for(i=0;i<7;i++){int k=(start+i)%7;if(fvset_mode_valid(p,sizes[k][0],sizes[k][1],settings.depth,p==FVSET_SAGA?60:settings.refresh)){settings.width=sizes[k][0];settings.height=sizes[k][1];if(p==FVSET_SAGA)settings.refresh=60;settings.mode_changed=dirty=1;break;}}}
        else if(id==4&&p==FVSET_ARANYM){settings.depth=settings.depth==8?16:settings.depth==16?32:8;settings.mode_changed=dirty=1;}
        else if(id==5&&p==FVSET_ARANYM){settings.refresh=settings.refresh==60?70:settings.refresh==70?75:60;settings.mode_changed=dirty=1;}
    }
    close_form(x,y,w,h);
}
static void save_config(void)
{
    char backup[FVSET_PATH],error[180],message[220];int mode=settings.mode_changed,platform=settings.platform;
    if(!settings.original){alert("Load your FVDI.SYS first.");return;}
    if(!fvset_save(&settings,backup,sizeof(backup),error,sizeof(error))){alert(error);if(backup[0])alert(backup);return;}
    if(mode&&platform==FVSET_SHINOGI){char ini_backup[FVSET_PATH];if(!fvset_resolution(ini,settings.width,settings.height,ini_backup,sizeof(ini_backup),error,sizeof(error))){settings.mode_changed=1;dirty=1;alert("fVDI config saved; launcher resolution was not saved.");alert(error);return;}}
    dirty=0;settings.mode_changed=0;
    if(backup[0]){snprintf(message,sizeof(message),"Saved. Backup: %s",backup);alert(message);}else alert("Settings saved.");
    alert("Restart the guest to apply changes. Palette screens use monochrome text.");
}
static void choose_fonts(void)
{
    char initial[FVSET_PATH],selected[FVSET_PATH],fallback[40];
    const char *folder=settings.fonts;
    char *end,*other;
    snprintf(fallback,sizeof(fallback),"%c:\\GEMSYS\\FONTS",ini[0]);
    if(!*folder||strlen(folder)+5>=sizeof(initial))folder=fallback;
    snprintf(initial,sizeof(initial),"%s%s*.*",folder,
             folder[strlen(folder)-1]=='\\'||folder[strlen(folder)-1]=='/'?"":"\\");
    if(!choose_file(initial,"*.*","Choose a font in the folder",selected))return;
    end=strrchr(selected,'\\');other=strrchr(selected,'/');
    if(!end||(other&&other>end))end=other;
    if(end){*end=0;strcpy(settings.fonts,selected);dirty=1;}
}

int main(int argc,char **argv)
{
    short x,y,w,h;int id,done=0;char path[FVSET_PATH],selected[FVSET_PATH];
    if(appl_init()<0)return 1;
    graf_handle(&cw,&ch,&bw,&bh);snprintf(path,sizeof(path),"%c:\\FVDI.SYS",'A'+(int)Supexec(boot_drive));snprintf(ini,sizeof(ini),"%c:\\SHINOGI.INI",'A'+(int)Supexec(boot_drive));
    if(argc>1){if(strlen(argv[1])>=sizeof(path)){alert("Configuration path is too long.");appl_exit();return 1;}strcpy(path,argv[1]);}else if(access("FVDI.SYS",F_OK)==0)strcpy(path,"FVDI.SYS");
    load_config(path);
    root(tree,FV_COUNT);object(tree,FV_TITLE,G_STRING,0,"fVDI settings",2,0,34);object(tree,FV_CONFIG,G_STRING,0,config_label,1,2,36);object(tree,FV_DRIVER,G_STRING,0,driver_label,1,3,36);
    button(tree,FV_FONTS,"Font folder...",2,4,34);object(tree,FV_FONTNAME,G_STRING,0,font_label,2,5,34);button(tree,FV_AA,aa_label,2,6,34);button(tree,FV_NVDI,nvdi_label,2,7,34);button(tree,FV_DISPLAY,"Display...",2,8,34);
    button(tree,FV_LOAD,"Load...",2,9,10);button(tree,FV_SAVE,"Save",14,9,10);button(tree,FV_CLOSE,"Close",26,9,10);tree[FV_CLOSE].ob_flags|=OF_DEFAULT|OF_LASTOB;
    while(!done){labels();open_form(tree,&x,&y,&w,&h);id=form_do(tree,0)&0x7fff;if(id<1||id>=FV_COUNT){close_form(x,y,w,h);alert("The GEM dialog could not continue.");break;}tree[id].ob_state&=~OS_SELECTED;close_form(x,y,w,h);
        if(id==FV_CLOSE){if(discard())done=1;}
        else if(id==FV_LOAD){if(discard()&&choose_file(settings.path[0]?settings.path:path,"*.SYS","Choose FVDI.SYS",selected))load_config(selected);}
        else if(id==FV_FONTS&&settings.original)choose_fonts();
        else if(id==FV_AA&&settings.original){settings.antialias=!settings.antialias;dirty=1;}
        else if(id==FV_NVDI&&settings.original){settings.nvdi=!settings.nvdi;dirty=1;}
        else if(id==FV_DISPLAY&&settings.original)display_settings();
        else if(id==FV_SAVE)save_config();
    }
    fvset_dispose(&settings);appl_exit();return 0;
}
