/* AUTO\00MODE.PRG for a disposable Falcon boot: switch to 16-bit true colour (320x480 on VGA, 640x400 otherwise) */
#include <mint/osbind.h>
#include <mint/falcon.h>
static void say(const char *t) { while (*t) Bconout(1, *t++); }
int main(void)
{
    short old = VsetMode(-1), mode;
    if (old & VGA)
        mode = (old & (VGA | PAL)) | BPS16 | COL40;          /* 320x480 */
    else
        mode = (old & (VGA | PAL)) | BPS16 | COL80 | VERTFLAG; /* 640x400 */
    VsetScreen((void *)-1L, (void *)-1L, 3, mode);
    say((VsetMode(-1) & NUMCOLS) == BPS16 ? "\r\nRASTER TEST VIDEO MODE SET (Falcon TC)\r\n" : "\r\nRASTER TEST VIDEO MODE FAILED\r\n");
    return 0;
}
