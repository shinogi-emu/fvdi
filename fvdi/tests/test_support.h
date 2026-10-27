/* Keep target assertion failures visible in unattended serial-log checks. */
#ifndef RASTER_TEST_SUPPORT_H
#define RASTER_TEST_SUPPORT_H
#ifdef __m68k__
void reference_fail(const char *, const char *, long);
#undef assert
#define assert(condition) ((condition) ? (void)0 : reference_fail(#condition, __FILE__, __LINE__))
#endif
#endif
