#ifndef TYPES_H
#define TYPES_H

//
//types.h - project-wide fixed-width and native-size typedefs
//used in place of stdint. every .c/.h in this project includes this.
//
//we pull in <stddef.h> so NULL, size_t, ptrdiff_t, and offsetof are
//available wherever types.h is included. avoids needing each
//widget file to remember to drag in stdlib/stdio just for NULL.
//

#include <stddef.h>

//
//architecture detection. clang on windows defines _M_X64/_M_AMD64 for x64
//and _M_IX86 for x86; _M_ARM64 for ARM64 Windows. we also honor the
//gcc/clang __x86_64__ / __i386__ / __aarch64__ / __arm__ spellings used
//by the Android NDK toolchain and every POSIX compiler.
//
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
    #define ARCHITECTURE_AMD64
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define ARCHITECTURE_ARM64
#elif defined(__i386__) || defined(_M_IX86)
    #define ARCHITECTURE_I386
#elif defined(__arm__) || defined(_M_ARM)
    #define ARCHITECTURE_ARM32
#else
    #error "unsupported architecture"
#endif

//
//byte-aliasing helper
//
typedef union safe_byte_t
{
    signed char   safe1;
    unsigned char saf2;
} safe_byte_t;

//
//address-size native integer. on amd64: 8 bytes. on i386: 4 bytes.
//

#if defined(ARCHITECTURE_AMD64) || defined(ARCHITECTURE_ARM64)
typedef unsigned long long nuint;
typedef signed long long   nint;
#endif

#if defined(ARCHITECTURE_I386) || defined(ARCHITECTURE_ARM32)
typedef unsigned int nuint;
typedef signed int   nint;
#endif

//
//fixed-width convenience types. use these across the project so that
//number sizes are unambiguous without dragging stdint everywhere.
//
typedef signed char        boole;
typedef unsigned char      ubyte;
typedef unsigned int       uint;
typedef unsigned short     ushort;
typedef unsigned long long uint64;
typedef signed long long   int64;
typedef unsigned long long timestamp;

//
//boolean literals. boole holds 0 or 1; any non-zero is "true" if tested,
//but we always write these two values explicitly.
//
//guarded with #ifndef because windows.h (via minwindef.h) already
//defines FALSE / TRUE as plain 0 / 1 ints, and our redefinitions
//trigger -Wmacro-redefined warnings in any TU that pulls in both
//headers (platform_win32.c, fs_win32.c). the Win32 defs evaluate to
//the same values our casts would produce (0 and 1 implicitly
//convertible to boole), so skipping the redefine is safe.
//
#ifndef FALSE
#define FALSE ((boole)0)
#endif
#ifndef TRUE
#define TRUE  ((boole)1)
#endif

#endif
