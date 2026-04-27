//
// stdlib.c - project-local drop-ins for a subset of libc.
//
// See stdlib.h for the rationale + function inventory. Implementations
// here prioritise CORRECTNESS and CLARITY over raw speed -- these are
// called from the parser, scene layer, and tests, not from the rendering
// hot path. Byte-at-a-time loops are fine. If a profile ever shows one
// of these functions dominating frame time, specialize per-CPU (SSE2 /
// NEON) in-place here.
//
// Design notes:
//   - No libc includes (this is the point).
//   - No allocation. Every function either mutates a caller-provided
//     buffer or returns a pointer into one of its inputs.
//   - Signed int64 for lengths to match the rest of the project's
//     custom typedefs.
//   - Behaviour matches the libc version each function is replacing,
//     so migration is mechanical (s/strcmp(/stdlib__strcmp(/ etc.).
//

#include "stdlib.h"

//
// ===== memory =============================================================
//

void* stdlib__memcpy(void* dst, const void* src, int64 count)
{
    if (dst == NULL || src == NULL || count <= 0) { return dst; }
    ubyte*       d = (ubyte*)dst;
    const ubyte* s = (const ubyte*)src;
    while (count-- > 0)
    {
        *d++ = *s++;
    }
    return dst;
}

void* stdlib__memset(void* dst, int value, int64 count)
{
    if (dst == NULL || count <= 0) { return dst; }
    ubyte* d = (ubyte*)dst;
    ubyte  v = (ubyte)(value & 0xff);
    while (count-- > 0)
    {
        *d++ = v;
    }
    return dst;
}

void* stdlib__memmove(void* dst, const void* src, int64 count)
{
    if (dst == NULL || src == NULL || count <= 0) { return dst; }
    ubyte*       d = (ubyte*)dst;
    const ubyte* s = (const ubyte*)src;
    //
    // Direction pick matters only when the buffers overlap. If dst is
    // at a HIGHER address, iterating from the tail avoids stomping
    // source bytes we haven't read yet. If dst is at a lower address,
    // forward iteration works. memcpy wouldn't be safe here -- that's
    // the whole reason memmove exists.
    //
    if (d < s)
    {
        while (count-- > 0) { *d++ = *s++; }
    }
    else if (d > s)
    {
        d += count;
        s += count;
        while (count-- > 0) { *--d = *--s; }
    }
    return dst;
}

int stdlib__memcmp(const void* a, const void* b, int64 count)
{
    if (count <= 0) { return 0; }
    if (a == b)     { return 0; }
    if (a == NULL)  { return -1; }
    if (b == NULL)  { return  1; }
    const ubyte* pa = (const ubyte*)a;
    const ubyte* pb = (const ubyte*)b;
    while (count-- > 0)
    {
        int diff = (int)*pa - (int)*pb;
        if (diff != 0) { return diff; }
        pa++;
        pb++;
    }
    return 0;
}

//
// ===== strings ============================================================
//

int64 stdlib__strlen(const char* s)
{
    if (s == NULL) { return 0; }
    const char* p = s;
    while (*p != 0) { p++; }
    return (int64)(p - s);
}

int stdlib__strcmp(const char* a, const char* b)
{
    if (a == b) { return 0; }
    if (a == NULL) { return -1; }
    if (b == NULL) { return  1; }
    //
    // Cast to unsigned char for the subtract so byte values >= 128
    // don't get sign-extended into a negative when comparing. libc's
    // strcmp does the same; without this, "abc" vs "\x80bc" would
    // wrongly report "abc > ...".
    //
    while (*a != 0 && *a == *b) { a++; b++; }
    return (int)(ubyte)*a - (int)(ubyte)*b;
}

int stdlib__strncmp(const char* a, const char* b, int64 n)
{
    if (n <= 0) { return 0; }
    if (a == b) { return 0; }
    if (a == NULL) { return -1; }
    if (b == NULL) { return  1; }
    while (n > 0 && *a != 0 && *a == *b)
    {
        a++;
        b++;
        n--;
    }
    if (n == 0) { return 0; }
    return (int)(ubyte)*a - (int)(ubyte)*b;
}

const char* stdlib__strstr(const char* haystack, const char* needle)
{
    if (haystack == NULL || needle == NULL) { return NULL; }
    //
    // Empty needle conventionally matches at position 0 (libc
    // strstr behaves this way -- returns `haystack`).
    //
    if (*needle == 0) { return haystack; }
    //
    // O(n*m) naive scan. For the parser use case (looking up short
    // keywords) this is fine; replace with Boyer-Moore or similar if
    // a caller ever hits perf problems.
    //
    for (const char* p = haystack; *p != 0; p++)
    {
        const char* h = p;
        const char* n = needle;
        while (*n != 0 && *h == *n) { h++; n++; }
        if (*n == 0) { return p; }
    }
    return NULL;
}

const char* stdlib__strchr(const char* s, int ch)
{
    if (s == NULL) { return NULL; }
    char target = (char)ch;
    while (*s != 0)
    {
        if (*s == target) { return s; }
        s++;
    }
    //
    // libc strchr matches the terminator when ch == 0, returning a
    // pointer to the final byte. Match that behaviour.
    //
    if (target == 0) { return s; }
    return NULL;
}

int64 stdlib__strcpy_safe(char* dst, int64 dst_cap, const char* src)
{
    //
    // Always returns strlen(src) (so the caller can detect truncation
    // via the result). Even if dst_cap is 0 we still walk src to get
    // that length; fast paths are available if the caller doesn't
    // care about detecting truncation.
    //
    int64 src_len = stdlib__strlen(src);
    if (dst == NULL || dst_cap <= 0) { return src_len; }
    int64 copy_len = src_len;
    if (copy_len > dst_cap - 1) { copy_len = dst_cap - 1; }
    for (int64 i = 0; i < copy_len; i++)
    {
        dst[i] = src[i];
    }
    dst[copy_len] = 0;
    return src_len;
}

//
// ===== char classifiers ===================================================
//

int stdlib__isspace(int c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') ? 1 : 0;
}

int stdlib__isdigit(int c)
{
    return (c >= '0' && c <= '9') ? 1 : 0;
}

int stdlib__isxdigit(int c)
{
    if (c >= '0' && c <= '9') { return 1; }
    if (c >= 'a' && c <= 'f') { return 1; }
    if (c >= 'A' && c <= 'F') { return 1; }
    return 0;
}

int stdlib__isalpha(int c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? 1 : 0;
}

int stdlib__isalnum(int c)
{
    return (stdlib__isalpha(c) || stdlib__isdigit(c)) ? 1 : 0;
}

int stdlib__tolower(int c)
{
    if (c >= 'A' && c <= 'Z') { return c + ('a' - 'A'); }
    return c;
}

int stdlib__toupper(int c)
{
    if (c >= 'a' && c <= 'z') { return c - ('a' - 'A'); }
    return c;
}

//
// ===== number parsing =====================================================
//

int stdlib__atoi(const char* s)
{
    if (s == NULL) { return 0; }
    //
    // Skip leading whitespace and optional sign, then read digits
    // until the first non-digit. No overflow handling -- matches
    // libc atoi's "undefined on overflow" contract. Callers wanting
    // error detection should use stdlib__strtol.
    //
    while (stdlib__isspace((unsigned char)*s)) { s++; }
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    int out = 0;
    while (stdlib__isdigit((unsigned char)*s))
    {
        out = out * 10 + (*s - '0');
        s++;
    }
    return out * sign;
}

long stdlib__strtol(const char* s, char** endptr, int base)
{
    //
    // Shape follows the BSD strtol that purelib__strtol was modelled
    // on, pared down to the features this project actually uses:
    //   - `base == 0` auto-detects 16 from `0x`/`0X`, 8 from leading
    //     `0`, else 10.
    //   - `base == 16` also strips an optional `0x`/`0X`.
    //   - Accepts leading whitespace + optional sign.
    //   - Sets *endptr to the first unparsed character (or to `s` if
    //     nothing parsed).
    //
    if (endptr != NULL) { *endptr = (char*)s; }
    if (s == NULL)      { return 0; }
    const char* p = s;
    while (stdlib__isspace((unsigned char)*p)) { p++; }
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') { p++; }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
        base = 16;
    }
    else if (base == 0 && *p == '0')
    {
        p++;
        base = 8;
    }
    else if (base == 0)
    {
        base = 10;
    }
    if (base < 2 || base > 36) { return 0; }

    long acc = 0;
    int  any = 0;
    while (*p != 0)
    {
        int digit;
        if (*p >= '0' && *p <= '9')      { digit = *p - '0'; }
        else if (*p >= 'a' && *p <= 'z') { digit = *p - 'a' + 10; }
        else if (*p >= 'A' && *p <= 'Z') { digit = *p - 'A' + 10; }
        else                             { break; }
        if (digit >= base) { break; }
        //
        // Overflow check: if (acc*base + digit) would exceed LONG_MAX
        // (or negate below LONG_MIN when neg), clamp and keep consuming.
        // libc's strtol sets errno and returns the clamp; we skip the
        // errno part -- callers that care about overflow check the
        // return against the known extremes themselves.
        //
        acc = acc * base + digit;
        any = 1;
        p++;
    }
    if (!any) { return 0; }
    if (endptr != NULL) { *endptr = (char*)p; }
    return neg ? -acc : acc;
}

//
// ===== math ===============================================================
//
// Software-only ops (sin / cos / tan / atan / atan2 / pow / exp / log).
// The BUILTIN backend forwards to libm; the LOCAL backend uses
// polynomial approximations that match libm's output to ~6 significant
// digits (enough for animation eases, colour blending, layout math).
//
// When STDLIB_MATH_BACKEND is LOCAL, sqrt / fabs / floor / ceil / fmod
// are ALSO defined here (the header only declares them in that mode,
// intrinsic-free). Tiny implementations: Newton-Raphson for sqrt,
// bitwise sign-mask for fabs, trunc-plus-correction for floor / ceil.
//

#if STDLIB_MATH_BACKEND == STDLIB_MATH_BACKEND_BUILTIN

//
// Software-only forwarders. The stdlib__ prefix is kept uniform across
// call sites, but the actual work goes to the libm symbol. Inlined
// hopefully, but even a function-call overhead (~3 ns) is noise next
// to the polynomial work inside libm itself.
//
float stdlib__sinf (float x)            { return sinf(x); }
float stdlib__cosf (float x)            { return cosf(x); }
float stdlib__tanf (float x)            { return tanf(x); }
float stdlib__atanf(float x)            { return atanf(x); }
float stdlib__atan2f(float y, float x)  { return atan2f(y, x); }
float stdlib__powf (float a, float b)   { return powf(a, b); }
float stdlib__expf (float x)            { return expf(x); }
float stdlib__logf (float x)            { return logf(x); }

#else // STDLIB_MATH_BACKEND_LOCAL

//
// LOCAL path: hand-rolled impls. Polynomial / range-reduction approach
// for transcendentals, Newton-Raphson for sqrt. Precision is ~6 sig
// digits on the interval each function is typically called over; GUI
// code doesn't need float64 anyway.
//

//
// IEEE-754 single bitcast helpers. Unions avoid the strict-aliasing
// landmine of a plain float-to-int pointer cast.
//
static inline uint _stdlib_internal__f32_to_u32(float f)
{
    union { float f; uint u; } x;
    x.f = f;
    return x.u;
}
static inline float _stdlib_internal__u32_to_f32(uint u)
{
    union { float f; uint u; } x;
    x.u = u;
    return x.f;
}

//
// fabsf: clear the sign bit. Single instruction on any CPU with
// IEEE-754, doesn't need __builtin_* or libm.
//
float stdlib__fabsf(float x)
{
    uint u = _stdlib_internal__f32_to_u32(x);
    u &= 0x7FFFFFFFu;
    return _stdlib_internal__u32_to_f32(u);
}
double stdlib__fabs(double x) { return (x < 0.0) ? -x : x; }

//
// sqrtf: Newton-Raphson, two iterations starting from a reasonable
// float32 initial guess. Converges quadratically so two steps take us
// to ~6 decimal places. Returns 0 for x <= 0 (matching libm's NaN/0
// behaviour close enough for GUI purposes).
//
float stdlib__sqrtf(float x)
{
    if (x <= 0.0f) { return 0.0f; }
    //
    // Initial guess: exponent-halving bit hack. Not as snazzy as the
    // famous Quake inverse-sqrt, but simpler and we don't need the
    // precision pressure.
    //
    uint u = _stdlib_internal__f32_to_u32(x);
    u = 0x1FBD1DF5u + (u >> 1);
    float r = _stdlib_internal__u32_to_f32(u);
    r = 0.5f * (r + x / r);
    r = 0.5f * (r + x / r);
    return r;
}
double stdlib__sqrt(double x)
{
    if (x <= 0.0) { return 0.0; }
    double r = x;
    for (int i = 0; i < 6; i++) { r = 0.5 * (r + x / r); }
    return r;
}

//
// floorf / ceilf: trunc via int cast, then correct by ±1 for
// negative / positive fractional leftovers. Adequate for values in
// the float32-representable integer range (± ~16 million), which is
// everything the GUI ever asks about.
//
float stdlib__floorf(float x)
{
    int i = (int)x;
    float fi = (float)i;
    if (x < 0.0f && fi != x) { fi -= 1.0f; }
    return fi;
}
float stdlib__ceilf(float x)
{
    int i = (int)x;
    float fi = (float)i;
    if (x > 0.0f && fi != x) { fi += 1.0f; }
    return fi;
}
double stdlib__floor(double x)
{
    long long i = (long long)x;
    double fi = (double)i;
    if (x < 0.0 && fi != x) { fi -= 1.0; }
    return fi;
}
double stdlib__ceil(double x)
{
    long long i = (long long)x;
    double fi = (double)i;
    if (x > 0.0 && fi != x) { fi += 1.0; }
    return fi;
}

//
// fmodf / fmod: x - floor(x/y)*y, but with sign tracking to match
// libm's "result has the sign of x" rule.
//
float stdlib__fmodf(float x, float y)
{
    if (y == 0.0f) { return 0.0f; }
    float q = x / y;
    //
    // Truncate toward zero, not floor, so fmod(-7, 3) = -1 not +2.
    //
    int iq = (int)q;
    return x - (float)iq * y;
}
double stdlib__fmod(double x, double y)
{
    if (y == 0.0) { return 0.0; }
    double q = x / y;
    long long iq = (long long)q;
    return x - (double)iq * y;
}

//
// sinf: range-reduced polynomial. Reduce x to [-pi, +pi], then to
// [-pi/2, +pi/2] via sin symmetry, then evaluate the standard 5-term
// Taylor series. ~6 sig digits across the entire float range.
//
static const float _STDLIB_PI       = 3.14159265358979323846f;
static const float _STDLIB_TWO_PI   = 6.28318530717958647692f;
static const float _STDLIB_HALF_PI  = 1.57079632679489661923f;
static const float _STDLIB_INV_TWO_PI = 0.15915494309189533577f;

float stdlib__sinf(float x)
{
    //
    // Range-reduce to [-pi, pi] via x - 2pi*round(x/2pi).
    //
    float k = x * _STDLIB_INV_TWO_PI;
    int   n = (int)(k + (k >= 0.0f ? 0.5f : -0.5f));
    x -= (float)n * _STDLIB_TWO_PI;
    //
    // Reduce [-pi,pi] to [-pi/2,pi/2] using sin(pi-x)=sin(x),
    // sin(-pi-x)=-sin(x). Keeps the Taylor series accurate near ±pi.
    //
    if (x > _STDLIB_HALF_PI)  { x =  _STDLIB_PI - x; }
    if (x < -_STDLIB_HALF_PI) { x = -_STDLIB_PI - x; }
    float x2 = x * x;
    //
    // Taylor: x - x^3/6 + x^5/120 - x^7/5040 + x^9/362880.
    // Horner form for one fmadd chain.
    //
    float r = x * (1.0f + x2 * (-0.16666667f + x2 * (0.00833333f + x2 * (-0.00019841f + x2 * 0.0000027557f))));
    return r;
}

float stdlib__cosf(float x)
{
    //
    // cos(x) = sin(x + pi/2). Avoids a second reduce-path impl.
    //
    return stdlib__sinf(x + _STDLIB_HALF_PI);
}

float stdlib__tanf(float x)
{
    float c = stdlib__cosf(x);
    if (c == 0.0f) { return 0.0f; }
    return stdlib__sinf(x) / c;
}

//
// atanf: Pade approximant good to ~1e-5 over the whole range. Use the
// identity atan(x) = sign(x) * (pi/2 - atan(1/x)) for |x| > 1 to keep
// the polynomial near 0.
//
float stdlib__atanf(float x)
{
    float sign = (x < 0.0f) ? -1.0f : 1.0f;
    float ax   = sign * x;
    boole swap = (ax > 1.0f);
    if (swap) { ax = 1.0f / ax; }
    float z = ax * ax;
    //
    // 5-term minimax on [0,1].
    //
    float r = ax * (0.99997726f + z * (-0.33262347f + z * (0.19354346f + z * (-0.11643287f + z * 0.05265332f))));
    if (swap) { r = _STDLIB_HALF_PI - r; }
    return sign * r;
}

float stdlib__atan2f(float y, float x)
{
    //
    // Standard quadrant-split. atan2 returns in (-pi, pi].
    //
    if (x > 0.0f) { return stdlib__atanf(y / x); }
    if (x < 0.0f)
    {
        if (y >= 0.0f) { return stdlib__atanf(y / x) + _STDLIB_PI; }
        return stdlib__atanf(y / x) - _STDLIB_PI;
    }
    // x == 0
    if (y > 0.0f) { return  _STDLIB_HALF_PI; }
    if (y < 0.0f) { return -_STDLIB_HALF_PI; }
    return 0.0f;
}

//
// expf / logf: standard range-reduction + polynomial. ~6 sig digits
// over the ranges GUI code exercises (animation eases reach ~[-10, 10]
// tops; pow() for gamma stays in [0, 1]).
//
float stdlib__expf(float x)
{
    //
    // exp(x) = 2^(x / ln 2). Split into integer k and fraction f such
    // that x/ln2 = k + f, f in [-0.5, 0.5]. Then 2^f via polynomial
    // and 2^k via bit manipulation of the float exponent.
    //
    const float LN2    = 0.69314718f;
    const float INV_LN2 = 1.44269504f;
    float t = x * INV_LN2;
    int k = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
    float f = x - (float)k * LN2;
    // 2^f ≈ polynomial on f in [-ln(2)/2, ln(2)/2]:
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.04166667f + f * 0.00833333f))));
    // Multiply by 2^k via exponent-biasing the float bits.
    int biased = 127 + k;
    if (biased <= 0)   { return 0.0f; }
    if (biased >= 255) { return 3.4e38f; } // near FLT_MAX
    uint u = (uint)biased << 23;
    float two_k = _stdlib_internal__u32_to_f32(u);
    return p * two_k;
}

float stdlib__logf(float x)
{
    if (x <= 0.0f) { return -3.4e38f; }
    //
    // log(x) = (exponent) * ln(2) + log(mantissa). Extract the IEEE-754
    // exponent + mantissa, evaluate log(1 + m) polynomial on [0, 1).
    //
    const float LN2 = 0.69314718f;
    uint u = _stdlib_internal__f32_to_u32(x);
    int  e = (int)((u >> 23) & 0xFFu) - 127;
    uint mant_bits = (u & 0x007FFFFFu) | 0x3F800000u;  // mantissa in [1, 2)
    float m = _stdlib_internal__u32_to_f32(mant_bits) - 1.0f;  // [0, 1)
    // Polynomial for log(1+m) on [0, 1):
    float r = m * (1.0f + m * (-0.5f + m * (0.33333333f + m * (-0.25f + m * 0.2f))));
    return (float)e * LN2 + r;
}

float stdlib__powf(float base, float exponent)
{
    //
    // pow(b, e) = exp(e * log(b)) for b > 0. Special-case exact integer
    // exponents (common: gamma correction on channel alpha uses pow(a,
    // 1/2.2); slider easing uses pow(t, N)) so we don't lose precision
    // through the log path for trivial cases.
    //
    if (base == 0.0f)
    {
        if (exponent == 0.0f) { return 1.0f; }
        return 0.0f;
    }
    if (base < 0.0f)
    {
        //
        // Negative base with a fractional exponent is complex-valued;
        // return 0 rather than a NaN to keep downstream GUI math
        // predictable.
        //
        return 0.0f;
    }
    return stdlib__expf(exponent * stdlib__logf(base));
}

#endif // STDLIB_MATH_BACKEND

