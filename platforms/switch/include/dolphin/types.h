#ifndef DOLPHIN_TYPES_H
#define DOLPHIN_TYPES_H

// Nintendo Switch/libnx host ABI override for Aurora's Dolphin SDK types.
//
// The original non-TARGET_PC branch models the 32-bit GameCube compiler and
// uses 'long' for s32/u32. On AArch64 LP64, long is 64-bit, which is both
// incorrect for the GameCube ABI and conflicts with libnx's fixed-width types.
// Keep the GameCube-visible widths explicit on Switch without defining
// TARGET_PC globally (PartyBoard uses TARGET_PC for desktop-only behaviour).

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BIT_64 1

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;

typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float f32;
typedef double f64;

typedef volatile f32 vf32;
typedef volatile f64 vf64;

typedef char* Ptr;

typedef int BOOL;
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

#ifndef NULL
#define NULL 0
#endif

#ifndef __cplusplus
#ifndef nullptr
#define nullptr NULL
#endif
#endif

#if defined(__GNUC__)
#define AT_ADDRESS(addr)
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#else
#error unsupported Switch compiler
#endif

#ifndef DECL_WEAK
#define DECL_WEAK __attribute__((weak))
#endif

#define NORETURN

#define __REGISTER

#endif // DOLPHIN_TYPES_H
