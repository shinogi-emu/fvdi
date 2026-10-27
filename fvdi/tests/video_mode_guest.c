/* Disposable AUTO setup only: run BEFORE fVDI, never on a live desktop. */
#include <mint/osbind.h>
#ifndef RASTER_TEST_REZ
#error Define RASTER_TEST_REZ to an Atari Setscreen resolution number
#endif
int main(void)
{
    const char *message;
    Setscreen((void *)-1L, (void *)-1L, RASTER_TEST_REZ);
    message = Getrez() == RASTER_TEST_REZ ? "\r\nRASTER TEST VIDEO MODE SET\r\n" :
              "\r\nFVDI TRANSFER FAIL: pre-fVDI video mode\r\n";
    while (*message) Bconout(1, *message++);
    return Getrez() == RASTER_TEST_REZ ? 0 : 1;
}
