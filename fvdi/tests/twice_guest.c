/*
 * twice_guest.c - run a guest test twice in a row, the second time after
 * the first has ended.
 *
 * What a VDI keeps for all programs (loaded fonts, their caches) must
 * outlive the program that made it.  A program cannot check that about
 * itself, so this one starts it twice with Pexec: the first run has
 * terminated, and GEMDOS has taken back what it owned, when the second
 * starts.
 *
 * TWICE.PRG [PROGRAM]   PROGRAM defaults to C:\VTEST\OUTLEDGE.PRG
 *
 * Reports over BIOS device 1 (serial) and ends with
 * "FVDI TWICE GUEST PASS" when both runs return 0, or
 * "FVDI TWICE FAIL: <check>".
 */

#include <mint/osbind.h>
#include <stdio.h>

static void say(const char *text)
{
    while (*text)
        Bconout(1, *text++);
    Bconout(1, '\r');
    Bconout(1, '\n');
}

int main(int argc, char **argv)
{
    const char *prog = argc > 1 ? argv[1] : "C:\\VTEST\\OUTLEDGE.PRG";
    char line[200];
    long r[2];
    int i;

    for (i = 0; i < 2; i++)
    {
        snprintf(line, sizeof(line), "TWICE run %d: %s", i + 1, prog);
        say(line);
        r[i] = Pexec(0, prog, "\0", NULL);
        snprintf(line, sizeof(line), "TWICE run %d returned %ld", i + 1, r[i]);
        say(line);
    }
    if (r[0] == 0 && r[1] == 0)
    {
        say("FVDI TWICE GUEST PASS");
        return 0;
    }
    snprintf(line, sizeof(line), "FVDI TWICE FAIL: runs returned %ld and %ld", r[0], r[1]);
    say(line);
    return 1;
}
