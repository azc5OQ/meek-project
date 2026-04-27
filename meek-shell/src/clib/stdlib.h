#ifndef GUI_CLIB_STDLIB_H
#define GUI_CLIB_STDLIB_H

#include "../types.h"

//
// stdlib.h - project-local drop-ins for a subset of the C
// standard library.
//
// Goal: reduce the toolkit's surface area onto libc. The functions
// below are the high-frequency ones (found by surveying `gui/src/`:
// strcmp at 170 sites, memset 101, memcpy 71, strlen 31). Pure
// functions with no platform dependency; reimplementing them is
// cheap and makes the library easier to port to environments
// where a full libc isn't ideal (freestanding builds, obscure
// embedded targets, etc.).
//
// Naming convention: each function carries the `stdlib__` prefix
// that the project uses for cross-file-module public APIs. Call
// sites replace `strcmp(a, b)` with `stdlib__strcmp(a, b)`, etc.
//
// NOT included here:
//   - malloc / calloc / realloc / free — wrapped by memory_manager.
//   - fopen / fread / fwrite / fclose — platform I/O, kept on libc.
//   - printf / fprintf / snprintf — kept on libc; small family and
//     a proper formatter would dwarf the rest of this file.
//   - wide-string variants — codebase uses them rarely (only
//     platform-specific Win32 APIs which already live outside the
//     portable core).
//
// Math (sqrt / pow / sin / cos / fabs / floor / ceil / fmod / atan2)
// IS included, gated on the STDLIB_MATH_BACKEND switch below.
//
// Adding more: any time a stdlib symbol appears in enough call
// sites to justify a replacement, write the stdlib__ version here
// and port usages opportunistically.
//

//
// ===== math backend switch ================================================
//
// STDLIB_MATH_BACKEND selects where `stdlib__sqrtf`, `stdlib__sinf`, etc.
// come from.
//
//   STDLIB_MATH_BACKEND_BUILTIN (default, = 1)
//     Hardware-intrinsic ops map to compiler builtins
//     (`__builtin_sqrtf`, `__builtin_fabsf`, ...). These compile to a
//     single CPU instruction on modern x86 / ARM — no libm symbol
//     dependency.
//
//     Software-only ops (sin, cos, pow, atan2, log, exp) map to the
//     matching libm functions via <math.h>. Compilers don't
//     intrinsify these universally, so they involve a libm call, but
//     that call is ~20-100 ns — fine for the toolkit's use (animation
//     eases, colour-space math, not per-pixel trig).
//
//   STDLIB_MATH_BACKEND_LOCAL (= 0)
//     Every math function uses the project's own implementation in
//     stdlib.c. No `__builtin_*`, no <math.h>. Cost model:
//       - sqrt / fabs / floor / ceil: ~20-100 cycles vs 1-5 with the
//         builtin. ~1-6% frame-time overhead in typical scenes.
//       - sin / cos / pow / atan2: roughly neutral — libm does a
//         polynomial approximation too; our version is similar at
//         float32 precision.
//     Useful for: freestanding builds, dropping libm from the link,
//     validating the local impl matches libm behaviour, or profiling
//     experiments.
//
// Flip the switch by defining the alternative before including this
// header, or via `-DSTDLIB_MATH_BACKEND=0` on the compile line.
//
#define STDLIB_MATH_BACKEND_BUILTIN 1
#define STDLIB_MATH_BACKEND_LOCAL 0

#ifndef STDLIB_MATH_BACKEND
#define STDLIB_MATH_BACKEND STDLIB_MATH_BACKEND_BUILTIN
#endif

#if STDLIB_MATH_BACKEND == STDLIB_MATH_BACKEND_BUILTIN
#include <math.h>
#endif

//
// ===== memory =============================================================
//

/**
 * Copy `count` bytes from `src` to `dst`. Behaviour matches memcpy:
 * no overlap allowed between the buffers. Use stdlib__memmove for
 * the overlapping case.
 *
 * @function stdlib__memcpy
 * @param {void*} dst - destination buffer.
 * @param {const void*} src - source buffer.
 * @param {int64} count - bytes to copy.
 * @return {void*} dst (matches libc).
 */
void *stdlib__memcpy(void *dst, const void *src, int64 count);

/**
 * Fill `count` bytes of `dst` with the low byte of `value`. Matches
 * memset semantics.
 *
 * @function stdlib__memset
 * @param {void*} dst - destination buffer.
 * @param {int} value - byte value (only low 8 bits used).
 * @param {int64} count - bytes to fill.
 * @return {void*} dst.
 */
void *stdlib__memset(void *dst, int value, int64 count);

/**
 * Copy bytes from `src` to `dst` with overlap-safe direction
 * selection. Matches memmove semantics; prefer memcpy when you
 * know the buffers don't overlap.
 *
 * @function stdlib__memmove
 * @param {void*} dst
 * @param {const void*} src
 * @param {int64} count
 * @return {void*} dst.
 */
void *stdlib__memmove(void *dst, const void *src, int64 count);

/**
 * Byte-wise compare `count` bytes. Returns 0 on equality, negative
 * if first differing byte in `a` is less than the one in `b`,
 * positive otherwise. Matches memcmp.
 *
 * @function stdlib__memcmp
 * @param {const void*} a
 * @param {const void*} b
 * @param {int64} count
 * @return {int}
 */
int stdlib__memcmp(const void *a, const void *b, int64 count);

//
// ===== strings ============================================================
//

/**
 * Length of a null-terminated C string, NOT counting the terminator.
 * No arbitrary cap (the existing purelib__strlen capped at 1000,
 * which is a silent-truncation hazard this implementation avoids).
 *
 * @function stdlib__strlen
 * @param {const char*} s - null-terminated string.
 * @return {int64} byte count before the terminator.
 */
int64 stdlib__strlen(const char *s);

/**
 * Lexicographic compare of two null-terminated strings. Returns
 * 0 on equality, negative if `a < b`, positive if `a > b`. Matches
 * strcmp semantics.
 *
 * @function stdlib__strcmp
 * @param {const char*} a
 * @param {const char*} b
 * @return {int}
 */
int stdlib__strcmp(const char *a, const char *b);

/**
 * Like strcmp but compares at most `n` bytes. Early-returns 0 if
 * both strings match for their first n bytes (even if longer).
 *
 * @function stdlib__strncmp
 * @param {const char*} a
 * @param {const char*} b
 * @param {int64} n
 * @return {int}
 */
int stdlib__strncmp(const char *a, const char *b, int64 n);

/**
 * Find the first occurrence of `needle` inside `haystack`. Returns
 * a pointer to the start of the match, or NULL. Matches strstr.
 * Both arguments must be null-terminated.
 *
 * @function stdlib__strstr
 * @param {const char*} haystack
 * @param {const char*} needle
 * @return {const char*}
 */
const char *stdlib__strstr(const char *haystack, const char *needle);

/**
 * Find the first occurrence of `ch` in the null-terminated string
 * `s`. Returns a pointer to it or NULL. Matches strchr. The
 * terminator IS matched if you pass `ch = 0`.
 *
 * @function stdlib__strchr
 * @param {const char*} s
 * @param {int} ch
 * @return {const char*}
 */
const char *stdlib__strchr(const char *s, int ch);

/**
 * Copy at most `dst_cap` bytes from `src` into `dst`. Always
 * null-terminates `dst` (provided dst_cap >= 1). Truncates if
 * `src` doesn't fit. Returns the number of source bytes that
 * WOULD have been copied (not including the terminator), so
 * callers can detect truncation: `if (written >= dst_cap) { ... }`.
 *
 * This is the strlcpy contract -- safer than strncpy (which
 * doesn't always terminate) and safer than strcpy (no cap).
 *
 * @function stdlib__strcpy_safe
 * @param {char*} dst
 * @param {int64} dst_cap - total bytes in dst (including terminator room).
 * @param {const char*} src
 * @return {int64} strlen(src) at time of call.
 */
int64 stdlib__strcpy_safe(char *dst, int64 dst_cap, const char *src);

//
// ===== char classifiers ===================================================
//
// ASCII-only. The gui parses CSS-shaped style files and XML-shaped
// UI files; both specs restrict structure bytes to ASCII, so full
// locale-aware ctype.h is overkill.
//

/**
 * ASCII whitespace: space, tab, newline, carriage return, form
 * feed, vertical tab. Matches isspace over the 7-bit subset.
 *
 * @function stdlib__isspace
 * @param {int} c - byte value.
 * @return {int} non-zero if whitespace.
 */
int stdlib__isspace(int c);

/** ASCII decimal digit. @function stdlib__isdigit */
int stdlib__isdigit(int c);

/** ASCII hexadecimal digit (0-9, a-f, A-F). @function stdlib__isxdigit */
int stdlib__isxdigit(int c);

/** ASCII alphabetic letter. @function stdlib__isalpha */
int stdlib__isalpha(int c);

/** ASCII alphanumeric. @function stdlib__isalnum */
int stdlib__isalnum(int c);

/** ASCII lowercase conversion; non-letters pass through. @function
 * stdlib__tolower */
int stdlib__tolower(int c);

/** ASCII uppercase conversion; non-letters pass through. @function
 * stdlib__toupper */
int stdlib__toupper(int c);

//
// ===== number parsing =====================================================
//

/**
 * Parse a base-10 integer from `s`, consuming leading whitespace +
 * optional sign + contiguous digits. Matches atoi semantics
 * (including "returns 0 on garbage input" — so prefer
 * `stdlib__strtol` if you need to distinguish "parsed zero" from
 * "couldn't parse").
 *
 * @function stdlib__atoi
 * @param {const char*} s
 * @return {int}
 */
int stdlib__atoi(const char *s);

/**
 * strtol-compatible integer parse. Accepts base 10 (default),
 * 16 (with `0x` / `0X` prefix when base is 0 or 16), or any base
 * 2..36. Sets `*endptr` to the first unparsed character.
 *
 * @function stdlib__strtol
 * @param {const char*} s
 * @param {char**} endptr - receives pointer past the last parsed char (may be
 * NULL).
 * @param {int} base - 0 (auto-detect) or 2..36.
 * @return {long}
 */
long stdlib__strtol(const char *s, char **endptr, int base);

//
// ===== math ===============================================================
//
// Two tiers of exposure. Hardware-intrinsic ops (sqrt / fabs / floor /
// ceil / fmod / fmin / fmax) are declared as `static inline` here so
// the builtin / libm-or-local selection happens at the call site with
// zero dispatch overhead. Software-only ops (sin / cos / tan / pow /
// exp / log / atan / atan2) are declared as normal function prototypes
// and defined in stdlib.c; the LOCAL backend gets polynomial
// approximations, the BUILTIN backend gets thin forwarders to libm.
//
// Everything is float32 by default (suffix-less like `stdlib__sqrtf`
// stays float). The GUI never needs double precision for animation /
// colour / layout; if a rare caller needs doubles, double-suffixed
// variants can be added without breaking existing ones.
//

#if STDLIB_MATH_BACKEND == STDLIB_MATH_BACKEND_BUILTIN
//
// Intrinsified path. `__builtin_*` forms let Clang / GCC emit a single
// instruction (`sqrtss`, `andps` with a sign-mask, `roundss`, etc.)
// with no libm symbol dependency. Behaviour identical to the matching
// libc function.
//
static inline float stdlib__sqrtf(float x)
{
	return __builtin_sqrtf(x);
}

static inline float stdlib__fabsf(float x)
{
	return __builtin_fabsf(x);
}

static inline float stdlib__floorf(float x)
{
	return __builtin_floorf(x);
}

static inline float stdlib__ceilf(float x)
{
	return __builtin_ceilf(x);
}

static inline float stdlib__fmodf(float x, float y)
{
	return __builtin_fmodf(x, y);
}

static inline float stdlib__fminf(float x, float y)
{
	return (x < y) ? x : y;
}

static inline float stdlib__fmaxf(float x, float y)
{
	return (x > y) ? x : y;
}

//
// Double variants — rare in the toolkit but a few colour-space +
// font paths use them. Kept for drop-in compat with existing code.
//
static inline double stdlib__sqrt(double x)
{
	return __builtin_sqrt(x);
}

static inline double stdlib__fabs(double x)
{
	return __builtin_fabs(x);
}

static inline double stdlib__floor(double x)
{
	return __builtin_floor(x);
}

static inline double stdlib__ceil(double x)
{
	return __builtin_ceil(x);
}

static inline double stdlib__fmod(double x, double y)
{
	return __builtin_fmod(x, y);
}

#else
float stdlib__sqrtf(float x);
float stdlib__fabsf(float x);
float stdlib__floorf(float x);
float stdlib__ceilf(float x);
float stdlib__fmodf(float x, float y);
static inline float stdlib__fminf(float x, float y)
{
	return (x < y) ? x : y;
}

static inline float stdlib__fmaxf(float x, float y)
{
	return (x > y) ? x : y;
}

double stdlib__sqrt(double x);
double stdlib__fabs(double x);
double stdlib__floor(double x);
double stdlib__ceil(double x);
double stdlib__fmod(double x, double y);
#endif

//
// Software-only ops. Prototypes only here; definitions in stdlib.c
// branch on STDLIB_MATH_BACKEND.
//
float stdlib__sinf(float x);
float stdlib__cosf(float x);
float stdlib__tanf(float x);
float stdlib__atanf(float x);
float stdlib__atan2f(float y, float x);
float stdlib__powf(float base, float exponent);
float stdlib__expf(float x);
float stdlib__logf(float x);

#endif
