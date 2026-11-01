/* Our64: libogc's gctypes.h for the Wii U build of Not64's core. */
#ifndef __GCTYPES_H__
#define __GCTYPES_H__
#include <stdint.h>
#include <stdbool.h>
#include <wut_types.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef volatile u8  vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8  vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;
typedef float  f32;
typedef double f64;

#ifndef BYTE_ORDER
#define LITTLE_ENDIAN 3412
#define BIG_ENDIAN    1234
#define BYTE_ORDER    BIG_ENDIAN
#endif
#endif
