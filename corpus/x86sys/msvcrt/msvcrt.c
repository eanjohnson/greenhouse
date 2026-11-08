// msvcrt.dll for 32-bit guests -- the subset mingw-built game libraries import.
//
// Half-Life 2's vgui2 links libfreetype/libfontconfig/libharfbuzz/libxml2 and
// SDL2, all built with mingw against the old system msvcrt.dll. A 64-bit Xbox
// has no SysWOW64 to find one in, and most of this cannot live on the host
// side of the thunk layer anyway: FILE objects and _iob are guest data,
// setjmp/longjmp move the guest's own stack, and freetype calls memcpy far too
// often to pay a gate transition each time. So it is a real PE32 DLL, mapped
// like any game DLL, that imports only kernel32 -- whose entry points are the
// loader's beds and forwards.
//
// Semantics follow msvcrt.dll, not the UCRT: time_t, _stat and _finddata_t
// carry 32-bit times; FILE is the 32-byte _iobuf; the "C" locale only.
// Files are always binary (no CRLF translation): mingw libraries open what
// they parse in "rb", and a text-mode read of an XML file only differs in the
// line endings libxml2 accepts either way.
//
// No CRT headers: every declaration is ours, so nothing in a header can
// disagree with a definition here. Built by build.sh.

typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned short     wchar;
typedef unsigned int       size_t;
typedef int                intptr_t;
typedef unsigned int       uintptr_t;
typedef __builtin_va_list  va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_arg(a, t)   __builtin_va_arg(a, t)
#define va_end(a)      __builtin_va_end(a)
#define NULL ((void*)0)

// ---- kernel32 -------------------------------------------------------------
#define K32 __declspec(dllimport) __stdcall
typedef void* HANDLE;
typedef struct { u32 lo, hi; } FILETIME;
typedef struct {
    u32 attr; FILETIME ctime, atime, mtime; u32 size_hi, size_lo;
} FILE_ATTR_DATA;
typedef struct {
    u32 attr; FILETIME ctime, atime, mtime; u32 size_hi, size_lo, r0, r1;
    char name[260]; char alt[14];
} FIND_DATA_A;
typedef struct { void* a; long b, c; HANDLE d, e; u32 f; } CRITICAL_SECTION;   // 24 bytes on x86

K32 HANDLE GetProcessHeap(void);
K32 void*  HeapAlloc(HANDLE, u32, size_t);
K32 int    HeapFree(HANDLE, u32, void*);
K32 void*  HeapReAlloc(HANDLE, u32, void*, size_t);
K32 size_t HeapSize(HANDLE, u32, const void*);
K32 HANDLE CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
K32 HANDLE CreateFileW(const wchar*, u32, u32, void*, u32, u32, HANDLE);
K32 int    ReadFile(HANDLE, void*, u32, u32*, void*);
K32 int    WriteFile(HANDLE, const void*, u32, u32*, void*);
K32 int    CloseHandle(HANDLE);
K32 int    SetFilePointerEx(HANDLE, i64, i64*, u32);
K32 u32    GetFileAttributesA(const char*);
K32 int    GetFileAttributesExA(const char*, int, FILE_ATTR_DATA*);
K32 int    GetFileAttributesExW(const wchar*, int, FILE_ATTR_DATA*);
K32 int    SetFileAttributesA(const char*, u32);
K32 int    CreateDirectoryA(const char*, void*);
K32 int    RemoveDirectoryA(const char*);
K32 int    DeleteFileA(const char*);
K32 int    MoveFileExA(const char*, const char*, u32);
K32 u32    GetCurrentDirectoryA(u32, char*);
K32 u32    GetFullPathNameA(const char*, u32, char*, char**);
K32 HANDLE FindFirstFileA(const char*, FIND_DATA_A*);
K32 int    FindNextFileA(HANDLE, FIND_DATA_A*);
K32 int    FindClose(HANDLE);
K32 u32    GetEnvironmentVariableA(const char*, char*, u32);
K32 void   GetSystemTimeAsFileTime(FILETIME*);
K32 void   OutputDebugStringA(const char*);
K32 void   ExitProcess(u32);
K32 HANDLE CreateThread(void*, size_t, void*, void*, u32, u32*);
K32 void   ExitThread(u32);
K32 void   InitializeCriticalSection(CRITICAL_SECTION*);
K32 void   EnterCriticalSection(CRITICAL_SECTION*);
K32 void   LeaveCriticalSection(CRITICAL_SECTION*);
K32 int    DuplicateHandle(HANDLE, HANDLE, HANDLE, HANDLE*, u32, int, u32);
K32 HANDLE GetCurrentProcess(void);
K32 int    MultiByteToWideChar(u32, u32, const char*, int, wchar*, int);
K32 int    WideCharToMultiByte(u32, u32, const wchar*, int, char*, int, const char*, int*);
K32 u32    GetLastError(void);
K32 u32    TlsAlloc(void);
K32 void*  TlsGetValue(u32);
K32 int    TlsSetValue(u32, void*);

#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)

// ---- errno ----------------------------------------------------------------
enum { EPERM = 1, ENOENT = 2, EBADF = 9, ENOMEM = 12, EACCES = 13, EEXIST = 17,
       EINVAL = 22, EMFILE = 24, ENOSPC = 28, ERANGE = 34 };

// clang emits a reference whenever floating point is used; the real CRT
// defines it to pull in its printf float support. Ours is always there.
int _fltused = 0;

static u32  g_tls = 0xFFFFFFFFu;
static int  g_errno_fallback;
static HANDLE g_heap;

int* _errno(void)
{
    if (g_tls == 0xFFFFFFFFu) return &g_errno_fallback;
    int* p = (int*)TlsGetValue(g_tls);
    if (!p) {
        p = (int*)HeapAlloc(g_heap, 8 /*HEAP_ZERO_MEMORY*/, sizeof(int));
        if (!p) return &g_errno_fallback;
        TlsSetValue(g_tls, p);
    }
    return p;
}
#define errno (*_errno())

static void set_errno_from_win32(void)
{
    switch (GetLastError()) {
    case 2: case 3: case 15: case 123: errno = ENOENT; break;   // file/path not found, bad name
    case 5: case 32: case 33:          errno = EACCES; break;
    case 80: case 183:                 errno = EEXIST; break;
    case 8: case 14:                   errno = ENOMEM; break;
    case 112:                          errno = ENOSPC; break;
    case 6:                            errno = EBADF;  break;
    default:                           errno = EINVAL; break;
    }
}

// ---- memory and strings ---------------------------------------------------
// The compiler lowers struct copies to these, so they must never be compiled
// back into calls to themselves: written with string instructions.
void* memcpy(void* d, const void* s, size_t n)
{
    void* r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void* memmove(void* d, const void* s, size_t n)
{
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    dp += n - 1; sp += n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory");
    return d;
}
void* memset(void* d, int c, size_t n)
{
    void* r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* p = (const unsigned char*)a; const unsigned char* q = (const unsigned char*)b;
    for (; n; --n, ++p, ++q) if (*p != *q) return *p - *q;
    return 0;
}
void* memchr(const void* s, int c, size_t n)
{
    const unsigned char* p = (const unsigned char*)s;
    for (; n; --n, ++p) if (*p == (unsigned char)c) return (void*)p;
    return NULL;
}
size_t strlen(const char* s) { const char* p = s; while (*p) ++p; return (size_t)(p - s); }
size_t wcslen(const wchar* s) { const wchar* p = s; while (*p) ++p; return (size_t)(p - s); }
int strcmp(const char* a, const char* b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char* a, const char* b, size_t n)
{
    for (; n; --n, ++a, ++b) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
char* strcpy(char* d, const char* s) { char* r = d; while ((*d++ = *s++)) {} return r; }
char* strncpy(char* d, const char* s, size_t n)
{
    char* r = d;
    for (; n && *s; --n) *d++ = *s++;
    for (; n; --n) *d++ = 0;
    return r;
}
char* strcat(char* d, const char* s) { strcpy(d + strlen(d), s); return d; }
char* strncat(char* d, const char* s, size_t n)
{
    char* e = d + strlen(d);
    for (; n && *s; --n) *e++ = *s++;
    *e = 0;
    return d;
}
char* strchr(const char* s, int c)
{
    for (;; ++s) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return NULL;
    }
}
char* strrchr(const char* s, int c)
{
    const char* r = NULL;
    for (;; ++s) {
        if (*s == (char)c) r = s;
        if (!*s) return (char*)r;
    }
}
unsigned char* _mbsrchr(const unsigned char* s, unsigned int c) { return (unsigned char*)strrchr((const char*)s, (int)c); }
char* strstr(const char* h, const char* n)
{
    size_t ln = strlen(n);
    if (!ln) return (char*)h;
    for (; *h; ++h) if (*h == *n && !strncmp(h, n, ln)) return (char*)h;
    return NULL;
}
char* strpbrk(const char* s, const char* set)
{
    for (; *s; ++s) for (const char* p = set; *p; ++p) if (*s == *p) return (char*)s;
    return NULL;
}

int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isxdigit(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int tolower(int c)  { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toupper(int c)  { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int isdig(int c) { return c >= '0' && c <= '9'; }

void* malloc(size_t n)  { void* p = HeapAlloc(g_heap, 0, n ? n : 1); if (!p) errno = ENOMEM; return p; }
void* calloc(size_t n, size_t sz)
{
    if (sz && n > 0xFFFFFFFFu / sz) { errno = ENOMEM; return NULL; }
    void* p = HeapAlloc(g_heap, 8 /*HEAP_ZERO_MEMORY*/, n * sz ? n * sz : 1);
    if (!p) errno = ENOMEM;
    return p;
}
void free(void* p) { if (p) HeapFree(g_heap, 0, p); }
void* realloc(void* p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    void* q = HeapReAlloc(g_heap, 0, p, n);
    if (!q) errno = ENOMEM;
    return q;
}
char* _strdup(const char* s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

// ---- 64-bit helpers -------------------------------------------------------
// clang lowers 64-bit division to _aulldiv and friends, which a CRT-less DLL
// does not have. Everything here divides through this one routine instead.
static u64 udiv64(u64 n, u32 d, u32* rem)
{
    if ((u32)(n >> 32) == 0) {
        u32 lo = (u32)n;
        if (rem) *rem = lo % d;
        return lo / d;
    }
    u64 q = 0; u64 r = 0;
    for (int i = 63; i >= 0; --i) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = (u32)r;
    return q;
}

// ---- number parsing -------------------------------------------------------
static u64 parse_u64(const char* s, char** end, int base, int* neg, int* overflow)
{
    const char* p = s;
    while (isspace((unsigned char)*p)) ++p;
    *neg = 0; *overflow = 0;
    if (*p == '-') { *neg = 1; ++p; } else if (*p == '+') ++p;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2; base = 16;
    } else if (base == 0) base = (*p == '0') ? 8 : 10;
    const u64 cutoff = udiv64(~(u64)0, (u32)base, NULL);
    u64 v = 0; int any = 0;
    for (;; ++p) {
        int c = (unsigned char)*p, dv;
        if (isdig(c)) dv = c - '0';
        else if (c >= 'a' && c <= 'z') dv = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') dv = c - 'A' + 10;
        else break;
        if (dv >= base) break;
        any = 1;
        if (v > cutoff || (v == cutoff && (u32)dv > (u32)(~(u64)0 - cutoff * (u32)base))) *overflow = 1;
        v = v * (u32)base + (u32)dv;
    }
    if (end) *end = (char*)(any ? p : s);
    return v;
}
long strtol(const char* s, char** end, int base)
{
    int neg, ov;
    u64 v = parse_u64(s, end, base, &neg, &ov);
    if (ov || v > (neg ? 0x80000000ull : 0x7FFFFFFFull)) { errno = ERANGE; return neg ? (long)0x80000000 : 0x7FFFFFFF; }
    return neg ? -(long)v : (long)v;
}
unsigned long strtoul(const char* s, char** end, int base)
{
    int neg, ov;
    u64 v = parse_u64(s, end, base, &neg, &ov);
    if (ov || v > 0xFFFFFFFFull) { errno = ERANGE; return 0xFFFFFFFFu; }
    return neg ? (unsigned long)-(long)v : (unsigned long)v;
}
i64 _strtoi64(const char* s, char** end, int base)
{
    int neg, ov;
    u64 v = parse_u64(s, end, base, &neg, &ov);
    if (ov || v > (neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull)) {
        errno = ERANGE;
        return neg ? (i64)0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFll;
    }
    return neg ? -(i64)v : (i64)v;
}
u64 _strtoui64(const char* s, char** end, int base)
{
    int neg, ov;
    u64 v = parse_u64(s, end, base, &neg, &ov);
    if (ov) { errno = ERANGE; return ~(u64)0; }
    return neg ? (u64)-(i64)v : v;
}
int atoi(const char* s) { return (int)strtol(s, NULL, 10); }

static double pow10i(int e)
{
    static const double big[] = { 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256 };
    double r = 1.0;
    int a = e < 0 ? -e : e;
    for (int i = 0; a && i < 9; ++i, a >>= 1) if (a & 1) r *= big[i];
    if (a) r = 1.0 / 0.0;
    return e < 0 ? 1.0 / r : r;
}

static int ieq(const char* p, const char* w)
{
    for (; *w; ++p, ++w) if (tolower((unsigned char)*p) != *w) return 0;
    return 1;
}

double strtod(const char* s, char** end)
{
    const char* p = s;
    while (isspace((unsigned char)*p)) ++p;
    int neg = 0;
    if (*p == '-') { neg = 1; ++p; } else if (*p == '+') ++p;
    if (ieq(p, "inf")) { p += ieq(p, "infinity") ? 8 : 3; if (end) *end = (char*)p; return neg ? -1.0 / 0.0 : 1.0 / 0.0; }
    if (ieq(p, "nan")) { p += 3; if (end) *end = (char*)p; return 0.0 / 0.0; }
    u64 mant = 0; int digits = 0, exp10 = 0, any = 0;
    for (; isdig((unsigned char)*p); ++p) {
        any = 1;
        if (digits < 19) { mant = mant * 10 + (u32)(*p - '0'); if (mant) ++digits; }
        else ++exp10;
    }
    if (*p == '.') {
        ++p;
        for (; isdig((unsigned char)*p); ++p) {
            any = 1;
            if (digits < 19) { mant = mant * 10 + (u32)(*p - '0'); if (mant) ++digits; --exp10; }
        }
    }
    if (!any) { if (end) *end = (char*)s; return 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char* q = p + 1; int eneg = 0, ev = 0;
        if (*q == '-') { eneg = 1; ++q; } else if (*q == '+') ++q;
        if (isdig((unsigned char)*q)) {
            for (; isdig((unsigned char)*q); ++q) if (ev < 10000) ev = ev * 10 + (*q - '0');
            exp10 += eneg ? -ev : ev;
            p = q;
        }
    }
    if (end) *end = (char*)p;
    double v = (double)(i64)mant;   // < 10^19, so the signed conversion is exact enough
    if (mant >> 63) v = (double)(i64)(mant >> 1) * 2.0;
    if (exp10 < -300) { v *= pow10i(-300); exp10 += 300; }
    v = exp10 < 0 ? v / pow10i(-exp10) : v * pow10i(exp10);
    if (v == 1.0 / 0.0) errno = ERANGE;
    return neg ? -v : v;
}
double atof(const char* s) { return strtod(s, NULL); }

char* _ultoa(unsigned long v, char* buf, int radix)
{
    char tmp[36]; int n = 0;
    if (radix < 2 || radix > 36) { buf[0] = 0; return buf; }
    do { u32 d = v % (u32)radix; tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (u32)radix; } while (v);
    for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return buf;
}

// ---- math (x87) -----------------------------------------------------------
double tan(double x)    { double r; __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
double log10(double x)  { double r; __asm__("fldlg2\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x)); return r; }
static double fsqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
double _hypot(double x, double y) { return fsqrt(x * x + y * y); }
void __setusermatherr(void* fn) { (void)fn; }

// ---- formatted output -----------------------------------------------------
typedef struct { void (*put)(void* ctx, const char* s, int n); void* ctx; int total; } Out;
static void out_n(Out* o, const char* s, int n) { if (n > 0) { o->put(o->ctx, s, n); o->total += n; } }
static void out_pad(Out* o, char c, int n) { char b[32]; memset(b, c, sizeof b); while (n > 0) { int k = n > 32 ? 32 : n; out_n(o, b, k); n -= k; } }

enum { F_LEFT = 1, F_PLUS = 2, F_SPACE = 4, F_ALT = 8, F_ZERO = 16 };

// Significant digits of v > 0: ndig of them into dig, rounded, and the
// decimal exponent of the first. Accurate to about 15 digits, which is what a
// double holds; past that the digits are zeros.
static void gen_digits(double v, int ndig, char* dig, int* exp10)
{
    int e = 0; double m = v;
    while (m >= 1e16) { m /= 1e16; e += 16; }
    while (m >= 10.0) { m /= 10.0; ++e; }
    while (m < 1e-16) { m *= 1e16; e -= 16; }
    while (m < 1.0)   { m *= 10.0; --e; }
    int real = ndig > 17 ? 17 : ndig;
    double r = 0.5; for (int i = 1; i < real; ++i) r /= 10.0;
    m += r;
    if (m >= 10.0) { m /= 10.0; ++e; }
    for (int i = 0; i < ndig; ++i) {
        if (i >= real) { dig[i] = '0'; continue; }
        int d = (int)m; if (d > 9) d = 9; if (d < 0) d = 0;
        dig[i] = (char)('0' + d);
        m = (m - d) * 10.0;
    }
    *exp10 = e;
}
static int exp_of(double v) { char d[1]; int e; gen_digits(v, 1, d, &e); return e; }

// One %e/%f/%g conversion of |v| into buf (no sign). Returns the length.
static int fmt_float(char* buf, double v, int prec, char conv, int flags)
{
    char dig[400]; int n = 0, e;
    int upper = (conv == 'E' || conv == 'G' || conv == 'F');
    if (v != v) { memcpy(buf, upper ? "NAN" : "nan", 3); return 3; }
    if (v == 1.0 / 0.0) { memcpy(buf, upper ? "INF" : "inf", 3); return 3; }
    if (prec < 0) prec = 6;
    if (prec > 300) prec = 300;
    char c = (char)tolower(conv);
    int strip = 0;
    if (c == 'g') {
        int P = prec ? prec : 1;
        int X = v == 0.0 ? 0 : (gen_digits(v, P, dig, &e), e);
        if (P > X && X >= -4) { c = 'f'; prec = P - 1 - X; } else { c = 'e'; prec = P - 1; }
        strip = !(flags & F_ALT);
    }
    if (c == 'e') {
        if (v == 0.0) { memset(dig, '0', (size_t)prec + 1); e = 0; }
        else gen_digits(v, prec + 1, dig, &e);
        buf[n++] = dig[0];
        if (prec || (flags & F_ALT)) buf[n++] = '.';
        for (int i = 1; i <= prec; ++i) buf[n++] = dig[i];
        if (strip && prec) { while (buf[n - 1] == '0') --n; if (buf[n - 1] == '.') --n; }
        buf[n++] = upper ? 'E' : 'e';
        buf[n++] = e < 0 ? '-' : '+';
        int ae = e < 0 ? -e : e;
        if (ae >= 100) buf[n++] = (char)('0' + ae / 100);
        buf[n++] = (char)('0' + ae / 10 % 10);
        buf[n++] = (char)('0' + ae % 10);
        return n;
    }
    // %f
    int nint, ndig;
    if (v == 0.0) { e = 0; ndig = 0; }
    else {
        e = exp_of(v);
        ndig = e + 1 + prec;
        if (ndig > 0) {
            if (ndig > 360) ndig = 360;
            int e2; gen_digits(v, ndig, dig, &e2);
            if (e2 != e) { e = e2; ndig = e + 1 + prec; if (ndig > 360) ndig = 360; gen_digits(v, ndig, dig, &e2); }
        }
    }
    if (ndig <= 0) {
        // Every significant digit falls past the precision: 0 or one unit in
        // the last place.
        int up = v != 0.0 && ndig == 0 && v >= 0.5 * pow10i(-prec);
        buf[n++] = (prec == 0 && up) ? '1' : '0';
        if (prec || (flags & F_ALT)) buf[n++] = '.';
        for (int i = 1; i <= prec; ++i) buf[n++] = (i == prec && up) ? '1' : '0';
    } else {
        nint = e + 1;
        int k = 0;
        if (nint <= 0) buf[n++] = '0';
        else for (; k < nint; ++k) buf[n++] = dig[k];
        if (prec || (flags & F_ALT)) buf[n++] = '.';
        for (int i = 0; i < prec; ++i) {
            int pos = nint + i;                       // digit index, negative = leading zero
            buf[n++] = (pos < 0) ? '0' : dig[pos];
        }
    }
    if (strip && prec) { while (buf[n - 1] == '0') --n; if (buf[n - 1] == '.') --n; }
    return n;
}

static int vformat(Out* o, const char* f, va_list ap)
{
    for (; *f; ++f) {
        if (*f != '%') {
            const char* s = f;
            while (f[1] && f[1] != '%') ++f;
            out_n(o, s, (int)(f - s + 1));
            continue;
        }
        ++f;
        int flags = 0, width = 0, prec = -1;
        for (;; ++f) {
            if (*f == '-') flags |= F_LEFT; else if (*f == '+') flags |= F_PLUS;
            else if (*f == ' ') flags |= F_SPACE; else if (*f == '#') flags |= F_ALT;
            else if (*f == '0') flags |= F_ZERO; else break;
        }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { flags |= F_LEFT; width = -width; } ++f; }
        else while (isdig((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            ++f; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); ++f; }
            else while (isdig((unsigned char)*f)) prec = prec * 10 + (*f++ - '0');
        }
        int lng = 0;   // 0 int, 1 long, 2 64-bit, -1 short, -2 char
        int wide = 0;
        for (;; ++f) {
            if (*f == 'l') { if (lng == 1) lng = 2; else lng = 1; wide = 1; }
            else if (*f == 'h') { lng = lng == -1 ? -2 : -1; wide = -1; }
            else if (*f == 'L' || *f == 'q') lng = 2;
            else if (*f == 'I' && f[1] == '6' && f[2] == '4') { lng = 2; f += 2; }
            else if (*f == 'I' && f[1] == '3' && f[2] == '2') { lng = 1; f += 2; }
            else if (*f == 'I' || *f == 'z' || *f == 't' || *f == 'j') { if (*f == 'j') lng = 2; else lng = 1; }
            else break;
        }
        char conv = *f;
        if (!conv) break;
        char buf[420]; int n = 0; const char* body = buf;
        char sign = 0; const char* prefix = ""; int zeropad_ok = 1;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            u64 v; int neg = 0;
            if (conv == 'p') { v = (u32)va_arg(ap, void*); if (prec < 8) prec = 8; conv = 'X'; }
            else if (lng == 2) v = va_arg(ap, u64);
            else v = va_arg(ap, u32);
            if (conv == 'd' || conv == 'i') {
                i64 sv = lng == 2 ? (i64)v : (lng == -1 ? (short)v : lng == -2 ? (signed char)v : (int)(u32)v);
                if (sv < 0) { neg = 1; v = (u64)-sv; } else v = (u64)sv;
                if (neg) sign = '-'; else if (flags & F_PLUS) sign = '+'; else if (flags & F_SPACE) sign = ' ';
            } else if (lng == -1) v &= 0xFFFF; else if (lng == -2) v &= 0xFF;
            u32 base = (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : 10;
            const char* dg = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[24]; int k = 0;
            while (v) { u32 r; v = udiv64(v, base, &r); tmp[k++] = dg[r]; }
            if (prec >= 0) zeropad_ok = 0; else prec = 1;
            for (int i = k; i < prec && n < 400; ++i) buf[n++] = '0';
            for (int i = k - 1; i >= 0; --i) buf[n++] = tmp[i];
            if ((flags & F_ALT) && k) { if (base == 16) prefix = conv == 'X' ? "0X" : "0x"; else if (base == 8 && buf[0] != '0') prefix = "0"; }
            break;
        }
        case 'c':
            buf[n++] = (char)va_arg(ap, int);
            zeropad_ok = 0;
            break;
        case 'C':
            buf[n++] = (char)va_arg(ap, int);
            zeropad_ok = 0;
            break;
        case 's': case 'S': {
            zeropad_ok = 0;
            int w = (conv == 'S') ? (wide >= 0) : (wide == 1);
            if (w) {
                const wchar* ws = va_arg(ap, const wchar*);
                if (!ws) { body = "(null)"; n = 6; break; }
                for (; *ws && n < 400 && (prec < 0 || n < prec); ++ws) buf[n++] = (char)(*ws < 128 ? *ws : '?');
            } else {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                body = s;
                n = 0;
                while (s[n] && (prec < 0 || n < prec)) ++n;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double v = va_arg(ap, double);
            if (conv == 'a') conv = 'e'; else if (conv == 'A') conv = 'E';
            if (v < 0 || (v == 0.0 && 1.0 / v < 0)) { sign = '-'; v = -v; }
            else if (flags & F_PLUS) sign = '+'; else if (flags & F_SPACE) sign = ' ';
            n = fmt_float(buf, v, prec, conv, flags);
            if (v != v || v == 1.0 / 0.0) zeropad_ok = 0;
            break;
        }
        case 'n': {
            int* p = va_arg(ap, int*);
            if (p) *p = o->total;
            continue;
        }
        default:   // "%%" and anything unknown print themselves
            buf[n++] = conv;
            zeropad_ok = 0;
            break;
        }
        int plen = (int)strlen(prefix) + (sign ? 1 : 0);
        int pad = width - n - plen;
        if (!(flags & F_LEFT) && !((flags & F_ZERO) && zeropad_ok)) out_pad(o, ' ', pad);
        if (sign) out_n(o, &sign, 1);
        out_n(o, prefix, (int)strlen(prefix));
        if (!(flags & F_LEFT) && (flags & F_ZERO) && zeropad_ok) out_pad(o, '0', pad);
        out_n(o, body, n);
        if (flags & F_LEFT) out_pad(o, ' ', pad);
    }
    return o->total;
}

static void put_mem(void* ctx, const char* s, int n) { char** p = (char**)ctx; memcpy(*p, s, (size_t)n); *p += n; }

int vsprintf(char* d, const char* f, va_list ap)
{
    char* p = d;
    Out o = { put_mem, &p, 0 };
    vformat(&o, f, ap);
    *p = 0;
    return o.total;
}
int sprintf(char* d, const char* f, ...)
{
    va_list ap; va_start(ap, f);
    int r = vsprintf(d, f, ap);
    va_end(ap);
    return r;
}

// ---- file descriptors -----------------------------------------------------
// fds 0..2 are the console streams. There is no console: stdout and stderr go
// to OutputDebugStringA (the loader logs it as "X86 dbg:"), a line at a time;
// stdin reads as end of file.
enum { O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_APPEND = 8, O_CREAT = 0x100,
       O_TRUNC = 0x200, O_EXCL = 0x400 };
#define MAX_FD 2048
typedef struct { HANDLE h; int open, append; } Fd;
static Fd g_fd[MAX_FD];
static CRITICAL_SECTION g_fdlock;

static int fd_new(HANDLE h, int append)
{
    EnterCriticalSection(&g_fdlock);
    for (int i = 3; i < MAX_FD; ++i)
        if (!g_fd[i].open) {
            g_fd[i].h = h; g_fd[i].open = 1; g_fd[i].append = append;
            LeaveCriticalSection(&g_fdlock);
            return i;
        }
    LeaveCriticalSection(&g_fdlock);
    errno = EMFILE;
    return -1;
}
static int fd_ok(int fd) { if (fd < 0 || fd >= MAX_FD || !g_fd[fd].open) { errno = EBADF; return 0; } return 1; }

static char g_line[2][512];
static int  g_linen[2];
static void console_write(int fd, const char* s, u32 n)
{
    int k = fd == 2;
    for (u32 i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '\n' || g_linen[k] >= 510) {
            g_line[k][g_linen[k]] = 0;
            if (g_linen[k]) OutputDebugStringA(g_line[k]);
            g_linen[k] = 0;
            if (c == '\n') continue;
        }
        if (c != '\r') g_line[k][g_linen[k]++] = c;
    }
}

static u32 open_disp(int of)
{
    if ((of & O_CREAT) && (of & O_EXCL)) return 1;    // CREATE_NEW
    if ((of & O_CREAT) && (of & O_TRUNC)) return 2;   // CREATE_ALWAYS
    if (of & O_CREAT) return 4;                       // OPEN_ALWAYS
    if (of & O_TRUNC) return 5;                       // TRUNCATE_EXISTING
    return 3;                                         // OPEN_EXISTING
}
static u32 open_access(int of)
{
    switch (of & 3) {
    case O_WRONLY: return 0x40000000u;                // GENERIC_WRITE
    case O_RDWR:   return 0xC0000000u;
    default:       return 0x80000000u;                // GENERIC_READ
    }
}
static int open_handle(HANDLE h, int of)
{
    if (h == INVALID_HANDLE) { set_errno_from_win32(); return -1; }
    int fd = fd_new(h, (of & O_APPEND) != 0);
    if (fd < 0) CloseHandle(h);
    return fd;
}
int _open(const char* path, int of, ...)
{
    return open_handle(CreateFileA(path, open_access(of), 7, NULL, open_disp(of), 0x80, NULL), of);
}
int _sopen(const char* path, int of, int share, ...)
{
    (void)share;
    return open_handle(CreateFileA(path, open_access(of), 7, NULL, open_disp(of), 0x80, NULL), of);
}
int _wopen(const wchar* path, int of, ...)
{
    return open_handle(CreateFileW(path, open_access(of), 7, NULL, open_disp(of), 0x80, NULL), of);
}
int _close(int fd)
{
    if (!fd_ok(fd)) return -1;
    if (fd > 2) CloseHandle(g_fd[fd].h);
    if (fd > 2) g_fd[fd].open = 0;
    return 0;
}
int _read(int fd, void* buf, unsigned n)
{
    if (!fd_ok(fd)) return -1;
    if (fd == 0) return 0;
    u32 got = 0;
    if (!ReadFile(g_fd[fd].h, buf, n, &got, NULL)) {
        if (GetLastError() == 109) return 0;          // ERROR_BROKEN_PIPE: end of a pipe
        set_errno_from_win32();
        return -1;
    }
    return (int)got;
}
int _write(int fd, const void* buf, unsigned n)
{
    if (!fd_ok(fd)) return -1;
    if (fd == 1 || fd == 2) { console_write(fd, (const char*)buf, n); return (int)n; }
    if (g_fd[fd].append) SetFilePointerEx(g_fd[fd].h, 0, NULL, 2);
    u32 put = 0;
    if (!WriteFile(g_fd[fd].h, buf, n, &put, NULL)) { set_errno_from_win32(); return -1; }
    return (int)put;
}
i64 _lseeki64(int fd, i64 off, int whence)
{
    if (!fd_ok(fd)) return -1;
    i64 at = 0;
    if (fd <= 2 || !SetFilePointerEx(g_fd[fd].h, off, &at, (u32)whence)) { errno = EINVAL; return -1; }
    return at;
}
int _dup(int fd)
{
    if (!fd_ok(fd)) return -1;
    if (fd <= 2) return fd;
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), g_fd[fd].h, GetCurrentProcess(), &h, 0, 0, 2 /*SAME_ACCESS*/)) {
        set_errno_from_win32();
        return -1;
    }
    return fd_new(h, g_fd[fd].append);
}
intptr_t _get_osfhandle(int fd) { return fd_ok(fd) ? (intptr_t)g_fd[fd].h : -1; }
int _locking(int fd, int mode, long n) { (void)mode; (void)n; return fd_ok(fd) ? 0 : -1; }

// ---- FILE -----------------------------------------------------------------
typedef struct {
    char* _ptr; int _cnt; char* _base; int _flag; int _file; int _charbuf; int _bufsiz; char* _tmpfname;
} FILE;
enum { IOREAD = 1, IOWRT = 2, IOEOF = 0x10, IOERR = 0x20, IORW = 0x80 };
#define BUFSZ 4096

FILE _iob[20] = {
    { 0, 0, 0, IOREAD, 0, 0, 0, 0 },
    { 0, 0, 0, IOWRT,  1, 0, 0, 0 },
    { 0, 0, 0, IOWRT,  2, 0, 0, 0 },
};
int __mb_cur_max = 1;

static FILE* file_new(int fd, int flag)
{
    FILE* f = (FILE*)calloc(1, sizeof(FILE));
    if (!f) return NULL;
    f->_file = fd; f->_flag = flag;
    return f;
}
static int parse_mode(const char* m, int* flag)
{
    int of;
    switch (*m) {
    case 'r': of = O_RDONLY; *flag = IOREAD; break;
    case 'w': of = O_WRONLY | O_CREAT | O_TRUNC; *flag = IOWRT; break;
    case 'a': of = O_WRONLY | O_CREAT | O_APPEND; *flag = IOWRT; break;
    default: errno = EINVAL; return -1;
    }
    for (++m; *m; ++m)
        if (*m == '+') { of = (of & ~3) | O_RDWR; *flag = IORW | IOREAD | IOWRT; }
        else if (*m == 'x') of |= O_EXCL;
    return of;
}
FILE* fopen(const char* path, const char* mode)
{
    int flag, of = parse_mode(mode, &flag);
    if (of < 0) return NULL;
    int fd = _open(path, of);
    if (fd < 0) return NULL;
    FILE* f = file_new(fd, flag);
    if (!f) _close(fd);
    return f;
}
FILE* _wfopen(const wchar* path, const wchar* mode)
{
    char m[8]; int i = 0;
    for (; mode[i] && i < 7; ++i) m[i] = (char)mode[i];
    m[i] = 0;
    int flag, of = parse_mode(m, &flag);
    if (of < 0) return NULL;
    int fd = _wopen(path, of);
    if (fd < 0) return NULL;
    FILE* f = file_new(fd, flag);
    if (!f) _close(fd);
    return f;
}
FILE* _fdopen(int fd, const char* mode)
{
    int flag;
    if (!fd_ok(fd) || parse_mode(mode, &flag) < 0) return NULL;
    return file_new(fd, flag);
}
int _fileno(FILE* f) { return f->_file; }

// Read-ahead buffer. A pending read buffer means the descriptor is ahead of
// the stream by _cnt bytes; anything that writes or flushes puts it back.
static void drop_readahead(FILE* f)
{
    if (f->_cnt > 0 && f->_file > 2) _lseeki64(f->_file, -(i64)f->_cnt, 1);
    f->_cnt = 0;
    f->_ptr = f->_base;
}
static int refill(FILE* f)
{
    if (!f->_base) {
        f->_base = (char*)malloc(BUFSZ);
        if (!f->_base) { f->_flag |= IOERR; return -1; }
        f->_bufsiz = BUFSZ;
    }
    int n = _read(f->_file, f->_base, (unsigned)f->_bufsiz);
    if (n <= 0) { f->_flag |= n < 0 ? IOERR : IOEOF; f->_cnt = 0; return -1; }
    f->_ptr = f->_base; f->_cnt = n;
    return 0;
}
int getc(FILE* f)
{
    if (f->_cnt <= 0 && refill(f) < 0) return -1;
    f->_cnt--;
    return (unsigned char)*f->_ptr++;
}
int ungetc(int c, FILE* f)
{
    if (c == -1) return -1;
    if (!f->_base) {
        f->_base = (char*)malloc(BUFSZ);
        if (!f->_base) return -1;
        f->_bufsiz = BUFSZ; f->_ptr = f->_base; f->_cnt = 0;
    }
    if (f->_ptr > f->_base) *--f->_ptr = (char)c;
    else if (f->_cnt < f->_bufsiz) {
        memmove(f->_base + 1, f->_ptr, (size_t)f->_cnt);
        f->_ptr = f->_base;
        *f->_ptr = (char)c;
    } else return -1;
    f->_cnt++;
    f->_flag &= ~IOEOF;
    return c & 0xFF;
}
size_t fread(void* buf, size_t sz, size_t cnt, FILE* f)
{
    size_t want = sz * cnt, got = 0;
    if (!want) return 0;
    char* d = (char*)buf;
    while (got < want) {
        if (f->_cnt > 0) {
            size_t k = (size_t)f->_cnt < want - got ? (size_t)f->_cnt : want - got;
            memcpy(d + got, f->_ptr, k);
            f->_ptr += k; f->_cnt -= (int)k; got += k;
            continue;
        }
        if (want - got >= BUFSZ) {
            int n = _read(f->_file, d + got, (unsigned)(want - got));
            if (n <= 0) { f->_flag |= n < 0 ? IOERR : IOEOF; break; }
            got += (size_t)n;
            continue;
        }
        if (refill(f) < 0) break;
    }
    return got / sz;
}
size_t fwrite(const void* buf, size_t sz, size_t cnt, FILE* f)
{
    size_t want = sz * cnt;
    if (!want) return 0;
    drop_readahead(f);
    int n = _write(f->_file, buf, (unsigned)want);
    if (n < 0) { f->_flag |= IOERR; return 0; }
    return (size_t)n / sz;
}
int fputc(int c, FILE* f) { char ch = (char)c; return fwrite(&ch, 1, 1, f) == 1 ? (c & 0xFF) : -1; }
int fputs(const char* s, FILE* f) { size_t n = strlen(s); return fwrite(s, 1, n, f) == n ? 0 : -1; }
int fflush(FILE* f)
{
    if (!f) return 0;
    if (f->_file == 1 || f->_file == 2) return 0;
    drop_readahead(f);
    return 0;
}
int fclose(FILE* f)
{
    if (!f) return -1;
    drop_readahead(f);
    int r = _close(f->_file);
    free(f->_base);
    if (f < _iob || f >= _iob + 20) free(f);
    else memset(f, 0, sizeof *f);
    return r;
}
int feof(FILE* f)   { return (f->_flag & IOEOF) != 0; }
int ferror(FILE* f) { return (f->_flag & IOERR) != 0; }

static void put_file(void* ctx, const char* s, int n) { fwrite(s, 1, (size_t)n, (FILE*)ctx); }
int vfprintf(FILE* f, const char* fmt, va_list ap)
{
    Out o = { put_file, f, 0 };
    return vformat(&o, fmt, ap);
}
int fprintf(FILE* f, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
int printf(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(&_iob[1], fmt, ap);
    va_end(ap);
    return r;
}

// ---- file system ----------------------------------------------------------
// msvcrt.dll's own layouts: 32-bit time_t throughout.
struct stat32   { u32 dev; unsigned short ino, mode; short nlink, uid, gid; u32 rdev; long size; long at, mt, ct; };
struct stati64  { u32 dev; unsigned short ino, mode; short nlink, uid, gid; u32 rdev; i64 size; long at, mt, ct; };
struct finddata { u32 attrib; long ct, at, wt; u32 size; char name[260]; };

static long ft_to_unix(FILETIME t)
{
    u64 v = ((u64)t.hi << 32) | t.lo;
    if (v < 116444736000000000ull) return 0;
    return (long)udiv64(v - 116444736000000000ull, 10000000u, NULL);
}
static unsigned short attr_mode(u32 a, const char* name)
{
    unsigned short m = (a & 0x10) ? 0x4000 | 0x49 : 0x8000;    // _S_IFDIR + exec bits | _S_IFREG
    m |= (a & 1) ? 0x124 : 0x1B6;                             // read-only: r--r--r--, else rw-rw-rw-
    if (name && !(a & 0x10)) {
        const char* dot = strrchr(name, '.');
        if (dot && (!strcmp(dot, ".exe") || !strcmp(dot, ".bat") || !strcmp(dot, ".com") || !strcmp(dot, ".cmd")))
            m |= 0x49;
    }
    return m;
}
// GetFileAttributesEx refuses "dir\", which stat accepts.
static const char* strip_slash(const char* p, char* tmp)
{
    size_t n = strlen(p);
    if (n < 2 || n >= 260 || (p[n - 1] != '\\' && p[n - 1] != '/') || (n == 3 && p[1] == ':')) return p;
    memcpy(tmp, p, n - 1); tmp[n - 1] = 0;
    return tmp;
}
static int fill_stat(const FILE_ATTR_DATA* d, const char* name, struct stati64* s)
{
    memset(s, 0, sizeof *s);
    s->mode = attr_mode(d->attr, name);
    s->nlink = 1;
    s->size = (i64)(((u64)d->size_hi << 32) | d->size_lo);
    s->at = ft_to_unix(d->atime); s->mt = ft_to_unix(d->mtime); s->ct = ft_to_unix(d->ctime);
    if (name && name[0] && name[1] == ':') s->dev = s->rdev = (u32)(tolower(name[0]) - 'a');
    return 0;
}
static void narrow_stat(const struct stati64* w, struct stat32* s)
{
    s->dev = w->dev; s->ino = w->ino; s->mode = w->mode; s->nlink = w->nlink;
    s->uid = w->uid; s->gid = w->gid; s->rdev = w->rdev;
    s->size = (long)w->size; s->at = w->at; s->mt = w->mt; s->ct = w->ct;
}
int _stati64(const char* path, struct stati64* s)
{
    char tmp[260]; FILE_ATTR_DATA d;
    const char* p = strip_slash(path, tmp);
    if (!GetFileAttributesExA(p, 0, &d)) { set_errno_from_win32(); return -1; }
    return fill_stat(&d, path, s);
}
int _stat(const char* path, struct stat32* s)
{
    struct stati64 w;
    if (_stati64(path, &w) < 0) return -1;
    narrow_stat(&w, s);
    return 0;
}
int _wstat(const wchar* path, struct stat32* s)
{
    FILE_ATTR_DATA d; struct stati64 w;
    if (!GetFileAttributesExW(path, 0, &d)) { set_errno_from_win32(); return -1; }
    fill_stat(&d, NULL, &w);
    narrow_stat(&w, s);
    return 0;
}
int _access(const char* path, int mode)
{
    u32 a = GetFileAttributesA(path);
    if (a == 0xFFFFFFFFu) { set_errno_from_win32(); return -1; }
    if ((mode & 2) && (a & 1) && !(a & 0x10)) { errno = EACCES; return -1; }
    return 0;
}
int _chmod(const char* path, int mode)
{
    u32 a = GetFileAttributesA(path);
    if (a == 0xFFFFFFFFu) { set_errno_from_win32(); return -1; }
    a = (mode & 0x80) ? (a & ~1u) : (a | 1u);                 // _S_IWRITE clears read-only
    if (!SetFileAttributesA(path, a)) { set_errno_from_win32(); return -1; }
    return 0;
}
int _mkdir(const char* p)  { if (!CreateDirectoryA(p, NULL)) { set_errno_from_win32(); return -1; } return 0; }
int _rmdir(const char* p)  { if (!RemoveDirectoryA(p)) { set_errno_from_win32(); return -1; } return 0; }
int _unlink(const char* p) { if (!DeleteFileA(p)) { set_errno_from_win32(); return -1; } return 0; }
int remove(const char* p)  { return _unlink(p); }
int rename(const char* a, const char* b)
{
    if (!MoveFileExA(a, b, 2 /*COPY_ALLOWED*/)) { set_errno_from_win32(); return -1; }
    return 0;
}
char* _getcwd(char* buf, int n)
{
    u32 need = GetCurrentDirectoryA(0, NULL);
    if (!need) { set_errno_from_win32(); return NULL; }
    if (!buf) { if ((u32)n < need) n = (int)need; buf = (char*)malloc((size_t)n); if (!buf) return NULL; }
    else if ((u32)n < need) { errno = ERANGE; return NULL; }
    GetCurrentDirectoryA((u32)n, buf);
    return buf;
}
char* _fullpath(char* abs, const char* rel, size_t n)
{
    char tmp[1024];
    u32 k = GetFullPathNameA(rel && *rel ? rel : ".", sizeof tmp, tmp, NULL);
    if (!k || k >= sizeof tmp) { set_errno_from_win32(); return NULL; }
    if (!abs) { abs = (char*)malloc(k + 1); if (!abs) return NULL; n = k + 1; }
    if (k + 1 > n) { errno = ERANGE; return NULL; }
    memcpy(abs, tmp, k + 1);
    return abs;
}
static void fill_find(const FIND_DATA_A* w, struct finddata* d)
{
    d->attrib = w->attr;
    d->ct = ft_to_unix(w->ctime); d->at = ft_to_unix(w->atime); d->wt = ft_to_unix(w->mtime);
    d->size = w->size_lo;
    memcpy(d->name, w->name, 260);
}
intptr_t _findfirst(const char* spec, struct finddata* d)
{
    FIND_DATA_A w;
    HANDLE h = FindFirstFileA(spec, &w);
    if (h == INVALID_HANDLE) { set_errno_from_win32(); return -1; }
    fill_find(&w, d);
    return (intptr_t)h;
}
int _findnext(intptr_t h, struct finddata* d)
{
    FIND_DATA_A w;
    if (!FindNextFileA((HANDLE)h, &w)) { errno = ENOENT; return -1; }
    fill_find(&w, d);
    return 0;
}
int _findclose(intptr_t h) { return FindClose((HANDLE)h) ? 0 : -1; }

// ---- environment, time, locale --------------------------------------------
// getenv's answer must outlive the call. One cached copy per name, replaced
// only when the value changes (the old copy is leaked: a caller may still be
// holding it).
static struct { char name[64]; char* val; } g_env[64];
static CRITICAL_SECTION g_envlock;
char* getenv(const char* name)
{
    char tmp[2048];
    u32 n = GetEnvironmentVariableA(name, tmp, sizeof tmp);
    if (!n || n >= sizeof tmp || strlen(name) >= 64) return NULL;
    EnterCriticalSection(&g_envlock);
    int slot = -1;
    for (int i = 0; i < 64; ++i) {
        if (g_env[i].val && !strcmp(g_env[i].name, name)) { slot = i; break; }
        if (!g_env[i].val && slot < 0) slot = i;
    }
    char* r = NULL;
    if (slot >= 0) {
        if (!g_env[slot].val || strcmp(g_env[slot].val, tmp)) {
            strcpy(g_env[slot].name, name);
            g_env[slot].val = _strdup(tmp);
        }
        r = g_env[slot].val;
    }
    LeaveCriticalSection(&g_envlock);
    return r;
}

long time(long* t)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    long v = ft_to_unix(ft);
    if (t) *t = v;
    return v;
}
struct tm { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
struct tm* gmtime(const long* t)
{
    static struct tm r;
    if (!t || *t < 0) return NULL;
    u32 s = (u32)*t, days = s / 86400, rem = s % 86400;
    r.hour = (int)(rem / 3600); r.min = (int)(rem % 3600 / 60); r.sec = (int)(rem % 60);
    r.wday = (int)((days + 4) % 7);
    int y = 1970;
    for (;;) {
        int yl = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
        if (days < (u32)yl) break;
        days -= (u32)yl; ++y;
    }
    r.year = y - 1900; r.yday = (int)days;
    static const int ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0, m = 0;
    for (; m < 12; ++m) { int l = ml[m] + (m == 1 && leap); if (days < (u32)l) break; days -= (u32)l; }
    r.mon = m; r.mday = (int)days + 1; r.isdst = 0;
    return &r;
}

struct lconv {
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol,
         *mon_decimal_point, *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes,
         n_sep_by_space, p_sign_posn, n_sign_posn;
};
static struct lconv g_lconv = { ".", "", "", "", "", "", "", "", "", "",
                                127, 127, 127, 127, 127, 127, 127, 127 };
struct lconv* localeconv(void) { return &g_lconv; }
char* setlocale(int cat, const char* loc)
{
    (void)cat;
    static char c[] = "C";
    if (!loc || !loc[0] || !strcmp(loc, "C")) return c;
    return NULL;
}
size_t mbstowcs(wchar* d, const char* s, size_t n)
{
    int k = MultiByteToWideChar(0 /*CP_ACP*/, 0, s, -1, NULL, 0);
    if (k <= 0) { errno = 42 /*EILSEQ*/; return (size_t)-1; }
    if (!d) return (size_t)(k - 1);
    if ((size_t)k <= n) { MultiByteToWideChar(0, 0, s, -1, d, k); return (size_t)(k - 1); }
    wchar tmp[2048];
    if (k > 2048) return (size_t)-1;
    MultiByteToWideChar(0, 0, s, -1, tmp, k);
    memcpy(d, tmp, n * sizeof(wchar));
    return n;
}
size_t wcstombs(char* d, const wchar* s, size_t n)
{
    int k = WideCharToMultiByte(0, 0, s, -1, NULL, 0, NULL, NULL);
    if (k <= 0) { errno = 42; return (size_t)-1; }
    if (!d) return (size_t)(k - 1);
    if ((size_t)k <= n) { WideCharToMultiByte(0, 0, s, -1, d, k, NULL, NULL); return (size_t)(k - 1); }
    char tmp[4096];
    if (k > 4096) return (size_t)-1;
    WideCharToMultiByte(0, 0, s, -1, tmp, k, NULL, NULL);
    memcpy(d, tmp, n);
    return n;
}

char* strerror(int e)
{
    switch (e) {
    case 0: return "No error";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case EBADF: return "Bad file descriptor";
    case ENOMEM: return "Not enough space";
    case EACCES: return "Permission denied";
    case EEXIST: return "File exists";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case ENOSPC: return "No space left on device";
    case ERANGE: return "Result too large";
    default: return "Unknown error";
    }
}
void perror(const char* s)
{
    if (s && *s) { fputs(s, &_iob[2]); fputs(": ", &_iob[2]); }
    fputs(strerror(errno), &_iob[2]);
    fputs("\n", &_iob[2]);
}

// ---- process, threads, startup support ------------------------------------
static u32 g_seed = 1;
void srand(unsigned s) { g_seed = s; }
int rand(void) { g_seed = g_seed * 214013u + 2531011u; return (int)((g_seed >> 16) & 0x7FFF); }

void abort(void)
{
    OutputDebugStringA("msvcrt: abort()");
    ExitProcess(3);
}
void exit(int code) { ExitProcess((u32)code); }
void _amsg_exit(int code)
{
    char b[48];
    sprintf(b, "msvcrt: runtime error R60%02d", code);
    OutputDebugStringA(b);
    ExitProcess(255);
}
void _assert(const char* msg, const char* file, unsigned line)
{
    char b[600];
    sprintf(b, "msvcrt: assertion failed: %.300s, file %.200s, line %u", msg ? msg : "", file ? file : "", line);
    OutputDebugStringA(b);
    abort();
}

typedef void (*vfn)(void);
void _initterm(vfn* a, vfn* b) { for (; a < b; ++a) if (*a) (*a)(); }

static CRITICAL_SECTION g_locks[36];
void _lock(int n)   { if (n >= 0 && n < 36) EnterCriticalSection(&g_locks[n]); }
void _unlock(int n) { if (n >= 0 && n < 36) LeaveCriticalSection(&g_locks[n]); }

uintptr_t _beginthreadex(void* sec, unsigned stack, void* start, void* arg, unsigned flags, unsigned* tid)
{
    (void)sec;
    HANDLE h = CreateThread(NULL, stack, start, arg, flags, tid);
    if (!h) set_errno_from_win32();
    return (uintptr_t)h;
}
void _endthreadex(unsigned code) { ExitThread(code); }

void* bsearch(const void* key, const void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{
    const char* b = (const char*)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp(key, b + mid * sz);
        if (!c) return (void*)(b + mid * sz);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
static void swap_bytes(char* a, char* b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
// Heap sort: no recursion, no scratch, O(n log n) whatever the input.
static void sift(char* b, size_t i, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) return;
        if (c + 1 < n && cmp(b + c * sz, b + (c + 1) * sz) < 0) ++c;
        if (cmp(b + i * sz, b + c * sz) >= 0) return;
        swap_bytes(b + i * sz, b + c * sz, sz);
        i = c;
    }
}
void qsort(void* base, size_t n, size_t sz, int (*cmp)(const void*, const void*))
{
    char* b = (char*)base;
    if (n < 2 || !sz) return;
    for (size_t i = n / 2; i-- > 0;) sift(b, i, n, sz, cmp);
    for (size_t e = n - 1; e > 0; --e) { swap_bytes(b, b + e * sz, sz); sift(b, 0, e, sz, cmp); }
}

// setjmp/longjmp on MSVC's x86 jmp_buf: Ebp, Ebx, Edi, Esi, Esp, Eip,
// _setjmp and _setjmp3 share this code: _setjmp3's second argument is a frame
// count the caller does not pop (both are cdecl), and filling all eight fields
// is a superset of what _setjmp promises.
// Registration (fs:[0] at the time of the setjmp). longjmp restores the SEH
// chain head rather than unwinding through it: C code has no handlers to run,
// and leaving fs:[0] pointing into the abandoned frames would be fatal at the
// next exception.
__asm__(
    ".att_syntax prefix\n"
    ".text\n"
    ".globl _setjmp\n"
    "_setjmp:\n"
    ".globl __setjmp\n"
    "__setjmp:\n"
    ".globl __setjmp3\n"
    "__setjmp3:\n"
    "  movl 4(%esp), %edx\n"
    "  movl %ebp, 0(%edx)\n"
    "  movl %ebx, 4(%edx)\n"
    "  movl %edi, 8(%edx)\n"
    "  movl %esi, 12(%edx)\n"
    "  leal 4(%esp), %eax\n"
    "  movl %eax, 16(%edx)\n"
    "  movl (%esp), %eax\n"
    "  movl %eax, 20(%edx)\n"
    "  movl %fs:0, %eax\n"
    "  movl %eax, 24(%edx)\n"
    "  movl $0, 28(%edx)\n"
    "  xorl %eax, %eax\n"
    "  ret\n"
    ".globl _longjmp\n"
    "_longjmp:\n"
    "  movl 4(%esp), %edx\n"
    "  movl 8(%esp), %eax\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  incl %eax\n"
    "1:\n"
    "  movl 24(%edx), %ecx\n"
    "  movl %ecx, %fs:0\n"
    "  movl 0(%edx), %ebp\n"
    "  movl 4(%edx), %ebx\n"
    "  movl 8(%edx), %edi\n"
    "  movl 12(%edx), %esi\n"
    "  movl 16(%edx), %esp\n"
    "  jmpl *20(%edx)\n"
);

// ---- the rest of libm, and the msvcrt data exports -------------------------
// Half-Life 1's video path (avutil-51/avcodec, a mingw FFmpeg build) and its
// gameui want the whole of msvcrt's math surface plus the ctype table. None of
// this can be a host bed: the callers pass and return doubles on the x87 stack
// and index _pctype directly.

// x87 single-instruction primitives, in the shape log10() above already proved
// correct: one input in st(0) via the "0" constraint, the result in st(0).
double sqrt(double x)  { return fsqrt(x); }
double sin(double x)   { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
double cos(double x)   { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
double log(double x)   { double r; __asm__("fldln2\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x)); return r; }
// fpatan takes st(1)/st(0), so the 1.0 has to go on top of x.
double atan(double x)  { double r; __asm__("fld1\n\tfpatan" : "=t"(r) : "0"(x)); return r; }
static double flog2(double x) { double r; __asm__("fld1\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x)); return r; }

// Truncation toward zero by clearing the fraction bits: a (long long) cast
// would lower to _ftol2, which a CRT-less DLL has no one to link against.
static double ftrunc(double x)
{
    union { double d; u64 u; } v; v.d = x;
    int e = (int)((v.u >> 52) & 0x7FF) - 1023;
    if (e < 0)  return (v.u >> 63) ? -0.0 : 0.0;
    if (e >= 52) return x;                       // already integral, or inf/NaN
    v.u &= ~(((u64)1 << (52 - e)) - 1);
    return v.d;
}
double floor(double x) { double t = ftrunc(x); return (t > x) ? t - 1.0 : t; }
double ceil(double x)  { double t = ftrunc(x); return (t < x) ? t + 1.0 : t; }

double ldexp(double x, int e)
{
    // At most three steps, so the scaling factor is always a normal double.
    while (e >  1023) { x *= 8.98846567431158e307;    e -= 1023; }   // 2^1023
    while (e < -1022) { x *= 2.2250738585072014e-308; e += 1022; }   // 2^-1022
    union { double d; u64 u; } f; f.u = (u64)(u32)(e + 1023) << 52;
    return x * f.d;
}

double frexp(double x, int* e)
{
    union { double d; u64 u; } v; v.d = x;
    int ex = (int)((v.u >> 52) & 0x7FF);
    if (ex == 0x7FF) { if (e) *e = 0; return x; }                 // inf or NaN
    if (ex == 0) {
        if (x == 0.0) { if (e) *e = 0; return x; }
        v.d = x * 18446744073709551616.0;                         // 2^64
        ex = (int)((v.u >> 52) & 0x7FF) - 64;
    }
    if (e) *e = ex - 1022;
    v.u = (v.u & ~((u64)0x7FF << 52)) | ((u64)1022 << 52);
    return v.d;
}

// 2^x. f2xm1 only accepts |x| <= 1 and needs fscale to put the integer part
// back, which is two x87 stack slots to get right in inline asm; the series is
// shorter to read and good to about 1 ulp over the half-open interval it sees.
static double fexp2(double x)
{
    if (x >  1100.0) return 1.0e308 * 1.0e308;    // +inf
    if (x < -1100.0) return 0.0;
    double n = floor(x + 0.5);
    double y = (x - n) * 0.69314718055994530942;  // ln 2; |y| <= 0.3466
    double r = 1.0;
    for (int k = 14; k >= 1; --k) r = 1.0 + y * r / (double)k;
    return ldexp(r, (int)n);
}
double exp(double x) { return fexp2(x * 1.4426950408889634074); }  // log2 e

double pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x == 0.0) return (y > 0.0) ? 0.0 : 1.0e308 * 1.0e308;
    if (x > 0.0) return fexp2(y * flog2(x));
    // Negative base: defined only for an integral exponent, and the sign comes
    // from its parity. Anything else is a domain error, and msvcrt returns a
    // NaN for it rather than trapping.
    double iy = ftrunc(y);
    if (iy != y) { double z = 0.0; return z / z; }
    double mag = fexp2(y * flog2(-x));
    return (ftrunc(iy * 0.5) * 2.0 == iy) ? mag : -mag;
}

double asin(double x)
{
    if (x >=  1.0) return  1.57079632679489661923;
    if (x <= -1.0) return -1.57079632679489661923;
    return atan(x / fsqrt(1.0 - x * x));
}
double acos(double x) { return 1.57079632679489661923 - asin(x); }

double sinh(double x) { double e = exp(x); return (e - 1.0 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) * 0.5; }
double tanh(double x)
{
    if (x >  20.0) return  1.0;                   // exp(40) already saturates
    if (x < -20.0) return -1.0;
    double e = exp(x + x);
    return (e - 1.0) / (e + 1.0);
}

// ---- ctype table ----------------------------------------------------------
// _pctype points at the entry for 0 and is indexed with EOF (-1) as well as
// 0..255, so the storage starts 128 below it. The "C" locale only: nothing
// above 0x7F is a letter.
enum { _CT_UPPER = 0x01, _CT_LOWER = 0x02, _CT_DIGIT = 0x04, _CT_SPACE = 0x08,
       _CT_PUNCT = 0x10, _CT_CNTRL = 0x20, _CT_BLANK = 0x40, _CT_HEX = 0x80 };
static unsigned short g_ctype[384];
unsigned short* _pctype = g_ctype + 128;
unsigned short* pctype  = g_ctype + 128;
static void init_ctype(void)
{
    for (int c = 0; c < 256; ++c) {
        unsigned short f = 0;
        if (c < 0x20 || c == 0x7F) { f |= _CT_CNTRL; if (c >= 0x09 && c <= 0x0D) f |= _CT_SPACE; }
        else if (c == ' ')         f |= _CT_SPACE | _CT_BLANK;
        else if (c >= '0' && c <= '9') f |= _CT_DIGIT | _CT_HEX;
        else if (c >= 'A' && c <= 'Z') { f |= _CT_UPPER; if (c <= 'F') f |= _CT_HEX; }
        else if (c >= 'a' && c <= 'z') { f |= _CT_LOWER; if (c <= 'f') f |= _CT_HEX; }
        else if (c < 0x7F)         f |= _CT_PUNCT;
        g_ctype[128 + c] = f;
    }
}
int _isctype(int c, int mask) { return (c >= -1 && c <= 255) ? (_pctype[c] & mask) : 0; }
int isctype(int c, int mask)  { return _isctype(c, mask); }

// The codepage the narrow CRT converts with, and the Windows version the CRT
// recorded at startup. Both are read, never written, by the libraries that
// import them. 1252 is what our MultiByteToWideChar bed answers for CP_ACP.
u32 __lc_codepage = 1252;
u32 _lc_codepage  = 1252;
u32 _winmajor = 6;
u32 winmajor  = 6;

// ---- strings --------------------------------------------------------------
int _stricmp(const char* a, const char* b)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    while (*p && tolower(*p) == tolower(*q)) { ++p; ++q; }
    return tolower(*p) - tolower(*q);
}
int stricmp(const char* a, const char* b) { return _stricmp(a, b); }
int _strnicmp(const char* a, const char* b, size_t n)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (; n; --n, ++p, ++q) {
        int d = tolower(*p) - tolower(*q);
        if (d || !*p) return d;
    }
    return 0;
}
int strnicmp(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); }
size_t strspn(const char* s, const char* accept)
{
    size_t n = 0;
    for (; s[n]; ++n) {
        const char* a = accept;
        while (*a && *a != s[n]) ++a;
        if (!*a) break;
    }
    return n;
}

// ---- time -----------------------------------------------------------------
// The console has no timezone database we can reach from here, so local time
// IS UTC. Saying so once is better than inventing an offset: every caller that
// matters here is timestamping a log line or measuring an interval.
struct tm* localtime(const long* t) { return gmtime(t); }

static const int g_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static int leap_year(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
long mktime(struct tm* m)
{
    if (!m) return -1;
    int y = m->year + 1900, mo = m->mon;
    y += mo / 12; mo %= 12;
    if (mo < 0) { mo += 12; --y; }
    if (y < 1970 || y > 2200) return -1;
    u32 days = 0;
    for (int i = 1970; i < y; ++i) days += 365u + (u32)leap_year(i);
    for (int i = 0; i < mo; ++i) days += (u32)g_mdays[i] + (u32)((i == 1) ? leap_year(y) : 0);
    days += (u32)(m->mday - 1);
    // 32-bit time_t, so this wraps past 2038 exactly as msvcrt's own does.
    long s = (long)((u32)days * 86400u + (u32)m->hour * 3600u +
                    (u32)m->min * 60u + (u32)m->sec);
    long back = s;
    struct tm* n = gmtime(&back);        // mktime also normalises its argument
    if (n) *m = *n;
    return s;
}

struct timeb32 { long time; unsigned short millitm; short timezone; short dstflag; };
void _ftime(struct timeb32* b)
{
    if (!b) return;
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    u64 v = ((u64)ft.hi << 32) | ft.lo;
    if (v < 116444736000000000ull) v = 116444736000000000ull;
    u64 ms = udiv64(v - 116444736000000000ull, 10000u, NULL);
    u32 r2; b->time = (long)udiv64(ms, 1000u, &r2);
    b->millitm = (unsigned short)r2;
    b->timezone = 0; b->dstflag = 0;
}
void ftime(struct timeb32* b) { _ftime(b); }

static u64 g_clock_base;
static u64 now_ms(void)
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    u64 v = ((u64)ft.hi << 32) | ft.lo;
    return udiv64(v, 10000u, NULL);
}
long clock(void) { return (long)(now_ms() - g_clock_base); }   // CLOCKS_PER_SEC 1000

// The "C" locale only, which is what every caller reaching us is formatting a
// log stamp or a file name with. An unknown specifier is copied through verbatim
// rather than dropped, so a format we do not know still shows what it asked for.
// Returns 0 and leaves the buffer unusable when it will not fit, as the CRT does.
static const char* const g_wd[7]  = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday" };
static const char* const g_mo[12] = { "January", "February", "March", "April",
                                      "May", "June", "July", "August",
                                      "September", "October", "November", "December" };
size_t strftime(char* out, size_t cap, const char* fmt, const struct tm* m)
{
    size_t n = 0;
    char tmp[64];
    if (!out || !cap || !fmt || !m) return 0;
    #define PUTC(ch) do { if (n + 1 >= cap) { out[0] = 0; return 0; } out[n++] = (char)(ch); } while (0)
    #define PUTS(s)  do { const char* q_ = (s); while (*q_) PUTC(*q_++); } while (0)
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { PUTC(*p); continue; }
        switch (*++p) {
        case 'a': tmp[0] = 0; { const char* w = g_wd[m->wday % 7];
                  tmp[0] = w[0]; tmp[1] = w[1]; tmp[2] = w[2]; tmp[3] = 0; } PUTS(tmp); break;
        case 'A': PUTS(g_wd[m->wday % 7]); break;
        case 'b': case 'h': { const char* w = g_mo[m->mon % 12];
                  tmp[0] = w[0]; tmp[1] = w[1]; tmp[2] = w[2]; tmp[3] = 0; } PUTS(tmp); break;
        case 'B': PUTS(g_mo[m->mon % 12]); break;
        case 'c': sprintf(tmp, "%.3s %.3s %2d %02d:%02d:%02d %d",
                          g_wd[m->wday % 7], g_mo[m->mon % 12], m->mday,
                          m->hour, m->min, m->sec, m->year + 1900); PUTS(tmp); break;
        case 'd': sprintf(tmp, "%02d", m->mday); PUTS(tmp); break;
        case 'e': sprintf(tmp, "%2d",  m->mday); PUTS(tmp); break;
        case 'H': sprintf(tmp, "%02d", m->hour); PUTS(tmp); break;
        case 'I': sprintf(tmp, "%02d", m->hour % 12 ? m->hour % 12 : 12); PUTS(tmp); break;
        case 'j': sprintf(tmp, "%03d", m->yday + 1); PUTS(tmp); break;
        case 'm': sprintf(tmp, "%02d", m->mon + 1); PUTS(tmp); break;
        case 'M': sprintf(tmp, "%02d", m->min);  PUTS(tmp); break;
        case 'p': PUTS(m->hour < 12 ? "AM" : "PM"); break;
        case 'S': sprintf(tmp, "%02d", m->sec);  PUTS(tmp); break;
        // Week of the year counted from the first Sunday (%U) or Monday (%W).
        case 'U': sprintf(tmp, "%02d", (m->yday + 7 - m->wday) / 7); PUTS(tmp); break;
        case 'W': sprintf(tmp, "%02d", (m->yday + 7 - ((m->wday + 6) % 7)) / 7); PUTS(tmp); break;
        case 'w': sprintf(tmp, "%d",   m->wday % 7); PUTS(tmp); break;
        case 'x': sprintf(tmp, "%02d/%02d/%02d", m->mon + 1, m->mday,
                          ((m->year + 1900) % 100 + 100) % 100); PUTS(tmp); break;
        case 'X': sprintf(tmp, "%02d:%02d:%02d", m->hour, m->min, m->sec); PUTS(tmp); break;
        case 'y': sprintf(tmp, "%02d", ((m->year + 1900) % 100 + 100) % 100); PUTS(tmp); break;
        case 'Y': sprintf(tmp, "%d",   m->year + 1900); PUTS(tmp); break;
        case 'Z': PUTS("UTC"); break;       // local time IS UTC here; see localtime
        case 'z': PUTS("+0000"); break;
        case 'n': PUTC('\n'); break;
        case 't': PUTC('\t'); break;
        case '%': PUTC('%');  break;
        case 0:   PUTC('%');  --p; break;   // a trailing '%' is literal
        default:  PUTC('%');  PUTC(*p); break;
        }
    }
    #undef PUTS
    #undef PUTC
    out[n] = 0;
    return n;
}

// ---- descriptors ----------------------------------------------------------
// fds 0..2 have no handle behind them (there is no console), which is exactly
// what a character device looks like to a caller: not seekable, no size.
int _isatty(int fd) { return (fd >= 0 && fd <= 2) ? 1 : 0; }
int isatty(int fd)  { return _isatty(fd); }

int _fstati64(int fd, struct stati64* s)
{
    if (!fd_ok(fd) || !s) { errno = EBADF; return -1; }
    for (u32 i = 0; i < sizeof *s; ++i) ((char*)s)[i] = 0;
    s->nlink = 1;
    if (fd <= 2) { s->mode = 0x2000 | 0x1B6; return 0; }          // _S_IFCHR | rw-rw-rw-
    s->mode = 0x8000 | 0x1B6;                                     // _S_IFREG | rw-rw-rw-
    i64 cur = 0, end = 0;
    if (!SetFilePointerEx(g_fd[fd].h, 0, &cur, 1) ||
        !SetFilePointerEx(g_fd[fd].h, 0, &end, 2)) { set_errno_from_win32(); return -1; }
    SetFilePointerEx(g_fd[fd].h, cur, NULL, 0);
    s->size = end;
    return 0;
}
int _fstat(int fd, struct stat32* s)
{
    struct stati64 w;
    if (_fstati64(fd, &w) < 0) return -1;
    narrow_stat(&w, s);
    return 0;
}
int fstat(int fd, struct stat32* s) { return _fstat(fd, s); }

// ---- per-DLL atexit -------------------------------------------------------
// __dllonexit grows the calling DLL's own table, whose head and tail pointers
// it is handed. The table starts empty (both NULL) in every mingw and MSVC
// DLL startup that imports this, so realloc owns it from the first call.
typedef void (*PVFV)(void);
PVFV __dllonexit(PVFV func, PVFV** pbegin, PVFV** pend)
{
    if (!func || !pbegin || !pend) return NULL;
    u32 n = (*pbegin && *pend) ? (u32)(*pend - *pbegin) : 0;
    PVFV* nb = (PVFV*)realloc(n ? *pbegin : NULL, (n + 1) * sizeof(PVFV));
    if (!nb) return NULL;
    nb[n] = func;
    *pbegin = nb; *pend = nb + n + 1;
    return func;
}
PVFV _dllonexit(PVFV func, PVFV** pbegin, PVFV** pend) { return __dllonexit(func, pbegin, pend); }

// ---- scanf ----------------------------------------------------------------
// One scanner over either a string or a FILE, so fscanf and sscanf share it.
typedef struct { const char* s; size_t len, pos; FILE* f; int back; } Src;
static int src_get(Src* r)
{
    if (r->back >= 0) { int c = r->back; r->back = -1; return c; }
    if (r->f) return getc(r->f);
    if (r->pos >= r->len || !r->s[r->pos]) return -1;
    return (unsigned char)r->s[r->pos++];
}
static void src_unget(Src* r, int c)
{
    if (c < 0) return;
    if (r->f) { r->back = c; return; }
    if (r->pos) --r->pos; else r->back = c;
}
static int scan_num(Src* r, int base, int width, i64* out, int* neg)
{
    int c = src_get(r), got = 0, n = 0;
    *neg = 0;
    if (c == '+' || c == '-') { *neg = (c == '-'); c = src_get(r); ++n; }
    if (base == 16 && c == '0') {
        int c2 = src_get(r);
        if (c2 == 'x' || c2 == 'X') { c = src_get(r); n += 2; }
        else { src_unget(r, c2); }
    }
    u64 v = 0;
    for (; c >= 0 && (width <= 0 || n < width); c = src_get(r), ++n) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base > 10 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base > 10 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (u64)(unsigned)base + (u64)(unsigned)d;
        got = 1;
    }
    src_unget(r, c);
    *out = (i64)v;
    return got;
}
static int gh_vscan(Src* r, const char* f, va_list ap)
{
    int assigned = 0;
    for (; *f; ++f) {
        if (isspace((unsigned char)*f)) {
            int c;
            do { c = src_get(r); } while (c >= 0 && isspace(c));
            src_unget(r, c);
            continue;
        }
        if (*f != '%') {
            int c = src_get(r);
            if (c != (unsigned char)*f) { src_unget(r, c); return assigned; }
            continue;
        }
        ++f;
        if (*f == '%') {
            int c = src_get(r);
            if (c != '%') { src_unget(r, c); return assigned; }
            continue;
        }
        int skip = 0, width = 0, lng = 0;
        if (*f == '*') { skip = 1; ++f; }
        for (; isdig((unsigned char)*f); ++f) width = width * 10 + (*f - '0');
        for (;; ++f) {
            if (*f == 'l' || *f == 'L') lng++;
            else if (*f == 'h' || *f == 'I' || *f == '6' || *f == '4' || *f == '3' || *f == '2') {}
            else break;
        }
        int c;
        switch (*f) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            do { c = src_get(r); } while (c >= 0 && isspace(c));
            src_unget(r, c);
            i64 v; int neg;
            int base = (*f == 'x' || *f == 'X') ? 16 : (*f == 'o' ? 8 : 10);
            if (!scan_num(r, base, width, &v, &neg)) return assigned;
            if (neg) v = -v;
            if (!skip) {
                if (lng >= 2) *va_arg(ap, i64*) = v;
                else *va_arg(ap, int*) = (int)v;
                ++assigned;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            do { c = src_get(r); } while (c >= 0 && isspace(c));
            char b[64];
            int n = 0;
            for (; c >= 0 && n < 63 && (width <= 0 || n < width); c = src_get(r)) {
                if (isdig(c) || c == '+' || c == '-' || c == '.' ||
                    c == 'e' || c == 'E') b[n++] = (char)c;
                else break;
            }
            src_unget(r, c);
            b[n] = 0;
            if (!n) return assigned;
            double d = strtod(b, NULL);
            if (!skip) {
                if (lng) *va_arg(ap, double*) = d;
                else *va_arg(ap, float*) = (float)d;
                ++assigned;
            }
            break;
        }
        case 's': {
            do { c = src_get(r); } while (c >= 0 && isspace(c));
            char* d = skip ? NULL : va_arg(ap, char*);
            int n = 0;
            for (; c >= 0 && !isspace(c) && (width <= 0 || n < width); c = src_get(r), ++n)
                if (d) d[n] = (char)c;
            src_unget(r, c);
            if (!n) return assigned;
            if (d) { d[n] = 0; ++assigned; }
            break;
        }
        case 'c': {
            int n = width > 0 ? width : 1;
            char* d = skip ? NULL : va_arg(ap, char*);
            for (int i = 0; i < n; ++i) {
                c = src_get(r);
                if (c < 0) return assigned;
                if (d) d[i] = (char)c;
            }
            if (d) ++assigned;
            break;
        }
        case '[': {
            int invert = 0;
            ++f;
            if (*f == '^') { invert = 1; ++f; }
            const char* set = f;
            if (*f == ']') ++f;
            for (; *f && *f != ']'; ++f) {}
            size_t setlen = (size_t)(f - set);
            char* d = skip ? NULL : va_arg(ap, char*);
            int n = 0;
            for (;;) {
                c = src_get(r);
                if (c < 0 || (width > 0 && n >= width)) break;
                int in = 0;
                for (size_t i = 0; i < setlen; ++i) {
                    if (set[i] == '-' && i && i + 1 < setlen) {
                        if (c > (unsigned char)set[i - 1] && c < (unsigned char)set[i + 1]) in = 1;
                    } else if ((unsigned char)set[i] == c) in = 1;
                }
                if (in == invert) break;
                if (d) d[n] = (char)c;
                ++n;
            }
            src_unget(r, c);
            if (!n) return assigned;
            if (d) { d[n] = 0; ++assigned; }
            break;
        }
        case 'n':
            if (!skip) { *va_arg(ap, int*) = (int)r->pos; ++assigned; }
            break;
        default:
            return assigned;
        }
    }
    return assigned;
}
int __stdio_common_vfscanf(u64 options, FILE* f, const char* fmt, void* loc, va_list ap)
{
    (void)options; (void)loc;
    if (!f || !fmt) return -1;
    Src r = { NULL, 0, 0, f, -1 };
    return gh_vscan(&r, fmt, ap);
}
int __stdio_common_vsscanf(u64 options, const char* buf, size_t cap, const char* fmt, void* loc, va_list ap)
{
    (void)options; (void)loc;
    if (!buf || !fmt) return -1;
    Src r = { buf, cap == (size_t)-1 ? strlen(buf) : cap, 0, NULL, -1 };
    return gh_vscan(&r, fmt, ap);
}

// msvcrt names them plainly; the UCRT spells them __stdio_common_v*scanf and
// has its own headers inline the narrow wrappers, so only msvcrt exports these.
int vsscanf(const char* buf, const char* fmt, va_list ap)
{
    if (!buf || !fmt) return -1;
    Src r = { buf, strlen(buf), 0, NULL, -1 };
    return gh_vscan(&r, fmt, ap);
}
int sscanf(const char* buf, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsscanf(buf, fmt, ap);
    va_end(ap);
    return n;
}
int vfscanf(FILE* f, const char* fmt, va_list ap)
{
    if (!f || !fmt) return -1;
    Src r = { NULL, 0, 0, f, -1 };
    return gh_vscan(&r, fmt, ap);
}
int fscanf(FILE* f, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

// ---- entry ----------------------------------------------------------------
int __stdcall DllMainCRTStartup(void* inst, u32 reason, void* reserved)
{
    (void)inst; (void)reserved;
    if (reason == 1) {
        g_heap = GetProcessHeap();
        g_tls = TlsAlloc();
        InitializeCriticalSection(&g_fdlock);
        InitializeCriticalSection(&g_envlock);
        for (int i = 0; i < 36; ++i) InitializeCriticalSection(&g_locks[i]);
        for (int i = 0; i < 3; ++i) { g_fd[i].open = 1; g_fd[i].h = NULL; }
        init_ctype();
        g_clock_base = now_ms();
    }
    return 1;
}
