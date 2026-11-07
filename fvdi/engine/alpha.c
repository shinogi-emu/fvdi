/* Driver-independent grayscale text compositing through native bitmap blits.
 * See LICENSE.TXT. No driver is handed an eight-plane form as monochrome.
 */
#include <stdint.h>
#include "fvdi.h"
#include "function.h"
#include "relocate.h"
#include "utility.h"

typedef struct { short shift[3], bits[3]; unsigned long mask; } AlphaFormat;
static int alpha_format(Virtual *v, AlphaFormat *f)
{
    Workstation *w=v->real_address;
    const Device *d=w->driver?w->driver->device:0;
    const short *where[3];
    int c,i;
    if(!d||d->format!=2||d->clut==1||
       (w->screen.mfdb.bitplanes!=16&&w->screen.mfdb.bitplanes!=32))return 0;
    where[0]=d->scrmap.bitnumber.red;where[1]=d->scrmap.bitnumber.green;where[2]=d->scrmap.bitnumber.blue;
    f->bits[0]=d->bits.red;f->bits[1]=d->bits.green;f->bits[2]=d->bits.blue;f->mask=0;
    for(c=0;c<3;c++){
        unsigned long mask;
        f->shift[c]=where[c][0];
        if(f->bits[c]<1||f->bits[c]>8||f->shift[c]<0||f->shift[c]+f->bits[c]>w->screen.mfdb.bitplanes)return 0;
        for(i=0;i<f->bits[c];i++)if(where[c][i]!=f->shift[c]+i)return 0;
        mask=((1UL<<f->bits[c])-1)<<f->shift[c];
        if(mask&f->mask)return 0;
        f->mask|=mask;
    }
    return 1;
}
int CDECL alpha_text_supported(Virtual *v)
{
    AlphaFormat f;
    return alpha_format(v,&f);
}
static int text_pixel(Virtual *v,short pen,unsigned long *pixel)
{
    Colour *p=v->palette;
    if(pen < -NEG_PAL_N || (pen < 0 && !p))return 0;
    if(!p||(((unsigned long)p&1)&&pen>=0))p=v->real_address->screen.palette.colours;
    p=(Colour *)((unsigned long)p&~1UL);
    if(!p||(pen>=0&&pen>=v->real_address->screen.palette.size))return 0;
    if(v->real_address->screen.mfdb.bitplanes==16){
        unsigned short value;
        memcpy(&value,&p[pen].real,2);
        *pixel=value;
    }else *pixel=p[pen].real;
    return 1;
}
static unsigned long load_pixel(const unsigned char *p,int bytes)
{
    unsigned long n=0;
    while(bytes--)n=(n<<8)|*p++;
    return n;
}
static void store_pixel(unsigned char *p,int bytes,unsigned long n)
{
    while(bytes--){p[bytes]=n;n>>=8;}
}
static unsigned long mix_pixel(unsigned long fg,unsigned long bg,int a,const AlphaFormat *f)
{
    unsigned long result=bg&~f->mask;
    int c;
    for(c=0;c<3;c++){
        unsigned long mask=(1UL<<f->bits[c])-1;
        unsigned long x=(fg>>f->shift[c])&mask,y=(bg>>f->shift[c])&mask;
        result|=((x*a+y*(255-a)+127)/255)<<f->shift[c];
    }
    return result;
}
int CDECL alpha_text_blit(Virtual *v,const MFDB *src,const short *xy,long mode)
{
    Workstation *w=v->real_address;
    AlphaFormat f;
    unsigned long fg,bg;
    long left,top,right,bottom,x,y,capacity,limit,bytes=w->screen.mfdb.bitplanes/8;
    unsigned char *buffer;
    MFDB row;
    if(!src||!src->address||src->standard!=0x0100||src->bitplanes!=8||
       src->wdwidth<=0||src->width<=0||src->height<=0||
       (long)src->wdwidth*2<src->width||xy[0]<0||xy[1]<0||
       xy[2]>=src->width||xy[3]>=src->height||xy[2]<xy[0]||xy[3]<xy[1]||
       mode<1||mode>4||!alpha_format(v,&f)||
       !text_pixel(v,v->text.colour.foreground,&fg)||!text_pixel(v,v->text.colour.background,&bg))return 0;
    left=MAX(0,xy[4]);top=MAX(0,xy[5]);
    right=MIN(w->screen.mfdb.width-1,(long)xy[4]+xy[2]-xy[0]);
    bottom=MIN(w->screen.mfdb.height-1,(long)xy[5]+xy[3]-xy[1]);
    if(v->clip.on){left=MAX(left,v->clip.rectangle.x1);top=MAX(top,v->clip.rectangle.y1);right=MIN(right,v->clip.rectangle.x2);bottom=MIN(bottom,v->clip.rectangle.y2);}
    if(left>right||top>bottom)return 1;
    buffer=(unsigned char *)allocate_block(16*bytes);
    if(!buffer)return 0;
    capacity=*(long *)buffer;limit=MIN(capacity/(16*bytes)*16,32752L);
    if(limit<=0){free_block(buffer);return 0;}
    memset(&row,0,sizeof(row));row.address=(short *)buffer;row.height=1;row.bitplanes=bytes*8;
    for(y=top;y<=bottom;y++)for(x=left;x<=right;x+=limit){
        long i,width=MIN(limit,right-x+1);
        short coords[8];
        const unsigned char *coverage=(const unsigned char *)src->address+
                   (y-xy[5]+xy[1])*(long)src->wdwidth*2+x-xy[4]+xy[0];
        row.width=width;row.wdwidth=(width+15)/16;
        coords[0]=x;coords[1]=y;coords[2]=x+width-1;coords[3]=y;coords[4]=coords[5]=0;coords[6]=width-1;coords[7]=0;
        if(mode!=1)lib_vdi_spppp(lib_vro_cpyfm,v,3,coords,0,&row,0);
        for(i=0;i<width;i++){
            int a=coverage[i];
            unsigned char *pixel=buffer+i*bytes;
            unsigned long old=mode==1?bg:load_pixel(pixel,bytes),target=fg;
            if(mode==3)target=old^f.mask;
            if(mode==4){target=bg;a=255-a;}
            store_pixel(pixel,bytes,mix_pixel(target,old,a,&f));
        }
        coords[0]=coords[1]=0;coords[2]=width-1;coords[3]=0;coords[4]=x;coords[5]=y;coords[6]=x+width-1;coords[7]=y;
        lib_vdi_spppp(lib_vro_cpyfm,v,3,coords,&row,0,0);
    }
    free_block(buffer);
    return 1;
}
