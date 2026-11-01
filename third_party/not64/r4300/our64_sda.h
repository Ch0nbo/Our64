/* Our64: variables the generated code reads through r13 (see SDAREL in
 * ppc/Wrappers.h). libogc's -G 8 default puts them in small data on its own;
 * wut has no small data area, so the Wii U build places them by hand. */
#ifndef OUR64_SDA_H
#define OUR64_SDA_H
#ifdef __WIIU__
#define OUR64_SDA      __attribute__((section(".sbss")))
#define OUR64_SDA_INIT __attribute__((section(".sdata")))
#else
#define OUR64_SDA
#define OUR64_SDA_INIT
#endif
#endif
