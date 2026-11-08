// ucrtbase.dll for 32-bit guests -- the UCRT surface MSVC-2015-and-later game
// binaries import, on top of the msvcrt.dll we already build.
//
// Half-Life 1 is the first title built against the modern CRT: hl.exe, sw.dll,
// tier0.dll, vstdlib.dll and FileSystem_Stdio.dll import VCRUNTIME140.dll and
// the api-ms-win-crt-*-l1-1-0.dll api sets, none of which exist on a 64-bit
// Xbox. hl.exe died at its entry point -- "unimplemented import
// api-ms-win-crt-runtime-l1-1-0.dll!_initterm_e", 0xC0000139 -- with 54 of its
// 93 imports unbound.
//
// Almost all of it is the same C library msvcrt.c already implements, so this
// file INCLUDES that source and adds only what the UCRT renamed, split or
// invented:
//   * the api-set split itself (see apisets.txt: one forwarder DLL per set,
//     all pointing here, so there is a single copy of the CRT's state);
//   * the __stdio_common_* printf/scanf entry points, which carry an options
//     word and a locale and are what every inline <stdio.h> function calls;
//   * 64-bit time_t and the _stat64i32 / _finddata64i32_t layouts;
//   * _libm_sse2_*_precise, which take their argument in XMM0 rather than on
//     the stack, and _CIatan2/_CIfmod, which take theirs on the x87 stack;
//   * the onexit tables (_initterm_e, _crt_atexit, _register_onexit_function);
//   * vcruntime140's exception plumbing, of which _except_handler4_common is
//     the one piece that really runs: the loader walks FS:[0] and calls the
//     handler a guest frame registered, and for /GS + __try code that handler
//     is ours. C++ EH (__CxxFrameHandler3, __RTDynamicCast) is diagnosed
//     rather than implemented -- see the note there.
//
// Only `exit` is replaced rather than added: msvcrt's goes straight to
// ExitProcess, and the UCRT's has to run the atexit tables first.
//
// No CRT headers here either: every declaration is ours. Built by build.sh.

#define exit msvcrt_exit
#include "../msvcrt/msvcrt.c"
#undef exit
void msvcrt_exit(int code);

// ---- the few kernel32 entry points msvcrt.c did not need ------------------
K32 int   SetCurrentDirectoryA(const char*);
K32 char* GetCommandLineA(void);
K32 void  DeleteCriticalSection(CRITICAL_SECTION*);
K32 void  RaiseException(u32, u32, u32, const u32*);
K32 void  RtlUnwind(void*, void*, void*, void*);

static void dbg(const char* s) { OutputDebugStringA(s); }
static void dbgf(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf(b, fmt, ap);
    va_end(ap);
    OutputDebugStringA(b);
}
// A name worth reporting once rather than every call: a guest that asks for a
// stub a million times would otherwise fill the diag and nothing else.
static int once(int* flag) { if (*flag) return 0; *flag = 1; return 1; }

// ---- heap -----------------------------------------------------------------
static int g_newmode;

size_t _msize(void* p)       { return p ? HeapSize(g_heap, 0, p) : 0; }
int    _heapchk(void)        { return -2; }            // _HEAPOK
int    _callnewh(size_t n)   { (void)n; return 0; }    // no new_handler: malloc failed
int    _set_new_mode(int m)  { int o = g_newmode; g_newmode = m; return o; }

// ---- strings, characters, multibyte ---------------------------------------
// _stricmp/_strnicmp/strspn, and ldexp/floor/ceil below, come from msvcrt.c:
// it is included above, and one implementation serving both CRTs is the point
// of that include.
char* _strlwr(char* s) { for (char* p = s; *p; ++p) *p = (char)tolower((unsigned char)*p); return s; }
char* _strupr(char* s) { for (char* p = s; *p; ++p) *p = (char)toupper((unsigned char)*p); return s; }

int isalnum(int c)  { return isalpha(c) || isdig(c); }
int isdigit(int c)  { return isdig(c); }
int ispunct(int c)  { return c > 32 && c < 127 && !isalnum(c); }
int isprint(int c)  { return c >= 32 && c < 127; }
int iswalnum(int c) { return isalnum(c); }
int iswdigit(int c) { return isdig(c); }
int iswprint(int c) { return c >= 32 && c != 0x7F; }
int iswspace(int c) { return isspace(c); }
int towlower(int c) { return tolower(c); }
int towupper(int c) { return toupper(c); }

size_t strcspn(const char* s, const char* set)
{
    const char* p = s;
    for (; *p; ++p)
        for (const char* q = set; *q; ++q)
            if (*p == *q) return (size_t)(p - s);
    return (size_t)(p - s);
}
// The real CRT keeps this per thread. One global is the historical msvcrt
// behaviour and is what a single-threaded parser (every strtok caller here)
// observes anyway.
static char* g_tok;
char* strtok(char* s, const char* delim)
{
    if (!s) s = g_tok;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { g_tok = NULL; return NULL; }
    char* start = s;
    s += strcspn(s, delim);
    if (*s) { *s = 0; g_tok = s + 1; } else g_tok = NULL;
    return start;
}
wchar* wcsncpy(wchar* d, const wchar* s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; ++i) d[i] = s[i];
    for (; i < n; ++i) d[i] = 0;
    return d;
}
wchar* wcschr(const wchar* s, int c)
{
    for (; *s; ++s) if (*s == (wchar)c) return (wchar*)s;
    return (c == 0) ? (wchar*)s : NULL;
}
wchar* wcscpy(wchar* d, const wchar* s) { wchar* r = d; while ((*d++ = *s++)) {} return r; }
int wcscmp(const wchar* a, const wchar* b)
{
    for (; *a && *a == *b; ++a, ++b) {}
    return (int)*a - (int)*b;
}
// The wide siblings Half-Life 1's Chromium (libcef/chromehtml) imports and the
// narrow ones did not cover. `wcsicmp` without the underscore is the legacy
// alias the real api-ms-win-crt-string still exports, so both spellings are
// here and both are listed in apisets.txt.
int _wcsicmp(const wchar* a, const wchar* b)
{
    for (; *a && towlower(*a) == towlower(*b); ++a, ++b) {}
    return towlower(*a) - towlower(*b);
}
int wcsicmp(const wchar* a, const wchar* b) { return _wcsicmp(a, b); }
int _wcsnicmp(const wchar* a, const wchar* b, size_t n)
{
    for (; n; --n, ++a, ++b) {
        if (towlower(*a) != towlower(*b)) return towlower(*a) - towlower(*b);
        if (!*a) return 0;
    }
    return 0;
}
wchar* wcscat(wchar* d, const wchar* s) { wcscpy(d + wcslen(d), s); return d; }
wchar* wcsncat(wchar* d, const wchar* s, size_t n)
{
    wchar* e = d + wcslen(d);
    for (; n && *s; --n) *e++ = *s++;
    *e = 0;
    return d;
}
int wcsncmp(const wchar* a, const wchar* b, size_t n)
{
    for (; n; --n, ++a, ++b) {
        if (*a != *b) return (int)*a - (int)*b;
        if (!*a) return 0;
    }
    return 0;
}
wchar* wcsrchr(const wchar* s, int c)
{
    const wchar* hit = NULL;
    for (;; ++s) { if (*s == (wchar)c) hit = s; if (!*s) break; }
    return (wchar*)hit;
}
wchar* wcsstr(const wchar* h, const wchar* n)
{
    if (!*n) return (wchar*)h;
    for (; *h; ++h) {
        const wchar* a = h; const wchar* b = n;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return (wchar*)h;
    }
    return NULL;
}

int   _mbscmp(const unsigned char* a, const unsigned char* b)  { return strcmp((const char*)a, (const char*)b); }
int   _mbsicmp(const unsigned char* a, const unsigned char* b) { return _stricmp((const char*)a, (const char*)b); }
unsigned char* _mbsnbcpy(unsigned char* d, const unsigned char* s, size_t n)
{
    strncpy((char*)d, (const char*)s, n);
    return d;
}
unsigned char* _mbsstr(const unsigned char* h, const unsigned char* n)
{
    return (unsigned char*)strstr((const char*)h, (const char*)n);
}

// ---- conversion -----------------------------------------------------------
i64 _atoi64(const char* s) { return _strtoi64(s, NULL, 10); }
int _wtoi(const wchar* s)
{
    char b[64];
    int i = 0;
    for (; s[i] && i < 63; ++i) b[i] = (char)s[i];
    b[i] = 0;
    return atoi(b);
}

// ---- math -----------------------------------------------------------------
double round(double x) { return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5); }
double fabs(double x)  { return x < 0.0 ? -x : x; }

static double x_sin(double x)  { double r; __asm__("fsin"               : "=t"(r) : "0"(x)); return r; }
static double x_cos(double x)  { double r; __asm__("fcos"               : "=t"(r) : "0"(x)); return r; }
static double x_sqrt(double x) { double r; __asm__("fsqrt"              : "=t"(r) : "0"(x)); return r; }
static double x_log2(double x) { double r; __asm__("fld1\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x)); return r; }
static double x_ln(double x)   { double r; __asm__("fldln2\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x)); return r; }
static double x_atan(double x) { double r; __asm__("fld1\n\tfpatan"     : "=t"(r) : "0"(x)); return r; }
// fpatan takes atan(st(1)/st(0)) and keeps the quadrant, which is atan2(y, x).
static double x_atan2(double y, double x)
{
    double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y));
    return r;
}
// 2^y by range reduction and the exp series: x87's f2xm1/fscale pair needs
// more stack juggling than the polynomial costs, and |f| <= 0.5 keeps the
// series exact to about 1e-15.
static double x_exp2(double y)
{
    if (y >= 1024.0)  { double i = 1e308; return i * i; }
    if (y <= -1075.0) return 0.0;
    double n = y >= 0.0 ? floor(y + 0.5) : ceil(y - 0.5);
    double u = (y - n) * 0.69314718055994530942;             // f * ln2
    double p = 1.0 + u * (1.0 + u * (0.5 + u * (1.0 / 6 + u * (1.0 / 24 +
               u * (1.0 / 120 + u * (1.0 / 720 + u * (1.0 / 5040)))))));
    return ldexp(p, (int)n);
}
double gh_sin(double x)   { return x_sin(x); }
double gh_cos(double x)   { return x_cos(x); }
double gh_tan(double x)   { return tan(x); }
double gh_atan(double x)  { return x_atan(x); }
double gh_sqrt(double x)  { return x_sqrt(x); }
double gh_log(double x)   { return x_ln(x); }
double gh_log10(double x) { return log10(x); }
double gh_exp(double x)   { return x_exp2(x * 1.4426950408889634074); }
double gh_acos(double x)
{
    if (x >= 1.0)  return 0.0;
    if (x <= -1.0) return 3.14159265358979323846;
    return x_atan2(x_sqrt(1.0 - x * x), x);
}
double gh_pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (x == 0.0) { if (y > 0.0) return 0.0; double i = 1e308; return i * i; }
    int neg = 0;
    if (x < 0.0) {
        double ry = y >= 0.0 ? floor(y + 0.5) : ceil(y - 0.5);
        if (ry != y) { double z = 0.0; return z / z; }        // non-integer power: NaN
        neg = ((int)ry) & 1;
        x = -x;
    }
    double r = x_exp2(x_log2(x) * y);
    return neg ? -r : r;
}
double gh_fmod(double x, double y)
{
    double r;
    __asm__("1: fprem\n\tfnstsw %%ax\n\ttestb $4, %%ah\n\tjnz 1b"
            : "=t"(r) : "0"(x), "u"(y) : "ax");
    return r;
}

// The /arch:SSE2 math calls: the argument arrives in XMM0 (and XMM1), not on
// the stack, and the result goes back in XMM0. Written as raw symbols because
// no C calling convention describes that.
__asm__(
    ".att_syntax prefix\n"
    ".text\n"
#define GH_SSE2_1(name, impl)                 \
    ".globl _" #name "\n"                     \
    "_" #name ":\n"                           \
    "  subl $8, %esp\n"                       \
    "  movsd %xmm0, (%esp)\n"                 \
    "  call _" #impl "\n"                     \
    "  fstpl (%esp)\n"                        \
    "  movsd (%esp), %xmm0\n"                 \
    "  addl $8, %esp\n"                       \
    "  ret\n"
#define GH_SSE2_2(name, impl)                 \
    ".globl _" #name "\n"                     \
    "_" #name ":\n"                           \
    "  subl $16, %esp\n"                      \
    "  movsd %xmm0, (%esp)\n"                 \
    "  movsd %xmm1, 8(%esp)\n"                \
    "  call _" #impl "\n"                     \
    "  fstpl (%esp)\n"                        \
    "  movsd (%esp), %xmm0\n"                 \
    "  addl $16, %esp\n"                      \
    "  ret\n"
    GH_SSE2_1(_libm_sse2_sin_precise,   gh_sin)
    GH_SSE2_1(_libm_sse2_cos_precise,   gh_cos)
    GH_SSE2_1(_libm_sse2_tan_precise,   gh_tan)
    GH_SSE2_1(_libm_sse2_atan_precise,  gh_atan)
    GH_SSE2_1(_libm_sse2_acos_precise,  gh_acos)
    GH_SSE2_1(_libm_sse2_exp_precise,   gh_exp)
    GH_SSE2_1(_libm_sse2_log_precise,   gh_log)
    GH_SSE2_1(_libm_sse2_log10_precise, gh_log10)
    GH_SSE2_2(_libm_sse2_pow_precise,   gh_pow)
    // sqrt needs no helper at all.
    ".globl __libm_sse2_sqrt_precise\n"
    "__libm_sse2_sqrt_precise:\n"
    "  sqrtsd %xmm0, %xmm0\n"
    "  ret\n"
    // The x87 pair: arguments are already on the FPU stack, first pushed in
    // st(1). fpatan(st1/st0) IS atan2(y, x); fmod needs the operands swapped
    // because fprem divides st(0) by st(1).
    ".globl __CIatan2\n"
    "__CIatan2:\n"
    "  fpatan\n"
    "  ret\n"
    ".globl __CIfmod\n"
    "__CIfmod:\n"
    "  fxch %st(1)\n"
    // A named label, not "1:": a backward numeric label in module-level asm
    // crashes clang 23's COFF writer in recordRelocation.
    "gh_cifmod_again:\n"
    "  fprem\n"
    "  fnstsw %ax\n"
    "  testb $4, %ah\n"
    "  jnz gh_cifmod_again\n"
    "  fstp %st(1)\n"
    "  ret\n"
);

// ---- 64-bit time ----------------------------------------------------------
i64 _time64(i64* t)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    i64 v = (i64)ft_to_unix(ft);
    if (t) *t = v;
    return v;
}
struct tm* _gmtime64(const i64* t)
{
    if (!t) return NULL;
    long n = (long)*t;
    return gmtime(&n);
}
// No timezone database: UTC. Every caller here uses it for a log stamp or a
// file time, where the offset is cosmetic.
struct tm* _localtime64(const i64* t) { return _gmtime64(t); }
// The 26-byte "Www Mmm dd hh:mm:ss yyyy\n\0" form, in the CRT's own static
// buffer. ctime is defined as asctime(localtime(t)), so it is written once.
char* asctime(const struct tm* m)
{
    static const char* wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char* mo[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static char b[32];
    if (!m) return NULL;
    sprintf(b, "%s %s %2d %02d:%02d:%02d %d\n", wd[m->wday & 7], mo[m->mon % 12],
            m->mday, m->hour, m->min, m->sec, m->year + 1900);
    return b;
}
char* _ctime64(const i64* t) { return asctime(_gmtime64(t)); }

// ---- file system, UCRT layouts --------------------------------------------
// _stat64i32: 64-bit times, 32-bit size. _finddata64i32_t likewise.
struct stat64i32   { u32 dev; unsigned short ino, mode; short nlink, uid, gid; u32 rdev; long size; i64 at, mt, ct; };
struct finddata64i32 { u32 attrib; i64 ct, at, wt; u32 size; char name[260]; };

int _stat64i32(const char* path, struct stat64i32* s)
{
    struct stati64 w;
    if (_stati64(path, &w) < 0) return -1;
    memset(s, 0, sizeof *s);
    s->dev = w.dev; s->ino = w.ino; s->mode = w.mode; s->nlink = w.nlink;
    s->uid = w.uid; s->gid = w.gid; s->rdev = w.rdev;
    s->size = (long)w.size;
    s->at = (i64)w.at; s->mt = (i64)w.mt; s->ct = (i64)w.ct;
    return 0;
}
static void fill_find64(const struct finddata* n, struct finddata64i32* d)
{
    d->attrib = n->attrib;
    d->ct = (i64)n->ct; d->at = (i64)n->at; d->wt = (i64)n->wt;
    d->size = n->size;
    memcpy(d->name, n->name, 260);
}
intptr_t _findfirst64i32(const char* spec, struct finddata64i32* d)
{
    struct finddata n;
    intptr_t h = _findfirst(spec, &n);
    if (h == -1) return -1;
    fill_find64(&n, d);
    return h;
}
int _findnext64i32(intptr_t h, struct finddata64i32* d)
{
    struct finddata n;
    if (_findnext(h, &n) < 0) return -1;
    fill_find64(&n, d);
    return 0;
}
int _chdir(const char* p)
{
    if (!SetCurrentDirectoryA(p)) { set_errno_from_win32(); return -1; }
    return 0;
}

// ---- stdio ----------------------------------------------------------------
static int g_commode, g_fmode;

FILE* __acrt_iob_func(unsigned i) { return &_iob[i < 20 ? i : 0]; }
int*  __p__commode(void)          { return &g_commode; }
int   _set_fmode(int m)           { g_fmode = m; return 0; }
int   _get_fmode(int* m)          { if (m) *m = g_fmode; return 0; }
int   fgetc(FILE* f)              { return getc(f); }
int   setvbuf(FILE* f, char* b, int mode, size_t sz)
{
    (void)f; (void)b; (void)mode; (void)sz;
    return 0;                        // writes are unbuffered; reads read ahead
}
char* fgets(char* s, int n, FILE* f)
{
    if (n <= 0) return NULL;
    int i = 0;
    for (; i < n - 1; ) {
        int c = getc(f);
        if (c < 0) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    s[i] = 0;
    return s;
}
int _fseeki64(FILE* f, i64 off, int whence)
{
    drop_readahead(f);
    f->_flag &= ~IOEOF;
    return _lseeki64(f->_file, off, whence) < 0 ? -1 : 0;
}
i64 _ftelli64(FILE* f)
{
    drop_readahead(f);
    return _lseeki64(f->_file, 0, 1);
}
int  fseek(FILE* f, long off, int whence) { return _fseeki64(f, (i64)off, whence); }
long ftell(FILE* f)                       { return (long)_ftelli64(f); }
int  fopen_s(FILE** pf, const char* path, const char* mode)
{
    if (!pf) return EINVAL;
    *pf = fopen(path, mode);
    return *pf ? 0 : (errno ? errno : ENOENT);
}

// A bounded memory sink: snprintf's whole point. o->total still counts every
// byte the format produced, which is what the standard return value is.
typedef struct { char* p; char* end; } MemLim;
static void put_mem_lim(void* ctx, const char* s, int n)
{
    MemLim* m = (MemLim*)ctx;
    int room = (int)(m->end - m->p);
    if (room <= 0 || n <= 0) return;
    int k = n < room ? n : room;
    memcpy(m->p, s, (size_t)k);
    m->p += k;
}

// The UCRT's inline <stdio.h> funnels every printf into these. `options` is a
// bitfield; bit 0 is legacy vsprintf null-termination, bit 2 asks for standard
// snprintf return values. The locale is always "C" here.
int __stdio_common_vfprintf(u64 options, FILE* f, const char* fmt, void* loc, va_list ap)
{
    (void)options; (void)loc;
    if (!f || !fmt) return -1;
    Out o = { put_file, f, 0 };
    vformat(&o, fmt, ap);
    return o.total;
}
int __stdio_common_vsprintf(u64 options, char* buf, size_t cap, const char* fmt, void* loc, va_list ap)
{
    (void)loc;
    if (!fmt) return -1;
    // sprintf passes (size_t)-1: unbounded, always terminated.
    if (cap == (size_t)-1) {
        int n = vsprintf(buf, fmt, ap);
        return n;
    }
    if (!buf || !cap) {
        Out o = { put_mem_lim, NULL, 0 };
        MemLim m = { NULL, NULL };
        o.ctx = &m;
        vformat(&o, fmt, ap);
        return o.total;
    }
    MemLim m = { buf, buf + cap - 1 };
    Out o = { put_mem_lim, &m, 0 };
    vformat(&o, fmt, ap);
    *m.p = 0;
    // Without _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOUR a truncated
    // result is -1, which is what _snprintf's callers expect.
    if ((size_t)o.total > cap - 1 && !(options & 4)) return -1;
    return o.total;
}

// Wide formatting, written out rather than borrowed: the narrow engine would
// read a %s argument as a narrow string, and in a wide format it is wide.
static int wformat(wchar* d, size_t cap, const wchar* f, va_list ap)
{
    size_t n = 0;
    wchar* end = cap ? d + cap - 1 : d;
    #define WPUT(c) do { if (d < end) *d++ = (wchar)(c); ++n; } while (0)
    for (; *f; ++f) {
        if (*f != '%') { WPUT(*f); continue; }
        ++f;
        if (*f == '%') { WPUT('%'); continue; }
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, shrt = 0;
        for (;; ++f) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+' || *f == ' ' || *f == '#') {}
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); ++f; }
        else for (; isdig(*f); ++f) width = width * 10 + (*f - '0');
        if (*f == '.') {
            ++f; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); ++f; }
            else for (; isdig(*f); ++f) prec = prec * 10 + (*f - '0');
        }
        for (;; ++f) {
            if (*f == 'l' || *f == 'w' || *f == 'I') lng = 1;
            else if (*f == 'h') shrt = 1;
            else if (*f == '6' || *f == '4' || *f == '3' || *f == '2') {}
            else break;
        }
        char nb[384];
        nb[0] = 0;
        const wchar* ws = NULL;
        wchar wbuf[2];
        switch (*f) {
        case 'd': case 'i': sprintf(nb, "%d", va_arg(ap, int)); break;
        case 'u':           sprintf(nb, "%u", va_arg(ap, unsigned)); break;
        case 'x':           sprintf(nb, "%x", va_arg(ap, unsigned)); break;
        case 'X':           sprintf(nb, "%X", va_arg(ap, unsigned)); break;
        case 'o':           sprintf(nb, "%o", va_arg(ap, unsigned)); break;
        case 'p':           sprintf(nb, "%08X", va_arg(ap, unsigned)); break;
        case 'e': case 'E': case 'f': case 'g': case 'G':
            fmt_float(nb, va_arg(ap, double), prec < 0 ? 6 : prec, *f, 0);
            prec = -1;
            break;
        case 'c':
            wbuf[0] = shrt ? (wchar)(unsigned char)va_arg(ap, int) : (wchar)va_arg(ap, int);
            wbuf[1] = 0;
            ws = wbuf;
            break;
        case 's':
            if (shrt) { const char* s = va_arg(ap, const char*); sprintf(nb, "%s", s ? s : "(null)"); }
            else      { ws = va_arg(ap, const wchar*); if (!ws) ws = (const wchar*)L"(null)"; }
            break;
        case 'S': { const char* s = va_arg(ap, const char*); sprintf(nb, "%s", s ? s : "(null)"); break; }
        default:  WPUT('%'); WPUT(*f); continue;
        }
        size_t len = ws ? wcslen(ws) : strlen(nb);
        if (prec >= 0 && (*f == 's' || *f == 'S') && (size_t)prec < len) len = (size_t)prec;
        if (!left) for (size_t i = len; i < (size_t)width; ++i) WPUT(zero && !ws ? '0' : ' ');
        for (size_t i = 0; i < len; ++i) WPUT(ws ? ws[i] : (wchar)(unsigned char)nb[i]);
        if (left) for (size_t i = len; i < (size_t)width; ++i) WPUT(' ');
    }
    #undef WPUT
    if (cap) *d = 0;
    return (int)n;
}
int __stdio_common_vswprintf(u64 options, wchar* buf, size_t cap, const wchar* fmt, void* loc, va_list ap)
{
    (void)loc;
    if (!fmt) return -1;
    if (cap == (size_t)-1) return wformat(buf, 0x7FFFFFFF, fmt, ap);
    int n = wformat(buf, cap, fmt, ap);
    if (cap && (size_t)n > cap - 1 && !(options & 4)) return -1;
    return n;
}

// scanf: the scanner (Src/src_get/scan_num/gh_vscan) and both entry points
// come from msvcrt.c, which is included above; one implementation serves
// both CRTs, which is the point of that include.

// ---- startup, onexit tables, exit -----------------------------------------
typedef void (*onexit_fn)(void);
typedef struct { onexit_fn* first; onexit_fn* last; onexit_fn* end; } onexit_table;

static onexit_table g_atexit;
static void* g_tls_dtor;

int _initialize_onexit_table(onexit_table* t)
{
    if (!t) return -1;
    if (!t->first && !t->last && !t->end) { t->first = t->last = t->end = NULL; }
    return 0;
}
int _register_onexit_function(onexit_table* t, onexit_fn f)
{
    if (!t) return -1;
    if (t->last == t->end) {
        size_t have = (size_t)(t->last - t->first), cap = have ? have * 2 : 32;
        onexit_fn* nb = (onexit_fn*)(t->first ? realloc(t->first, cap * sizeof(onexit_fn))
                                              : malloc(cap * sizeof(onexit_fn)));
        if (!nb) return -1;
        t->first = nb; t->last = nb + have; t->end = nb + cap;
    }
    *t->last++ = f;
    return 0;
}
int _execute_onexit_table(onexit_table* t)
{
    if (!t || !t->first) return 0;
    while (t->last > t->first) { onexit_fn f = *--t->last; if (f) f(); }
    free(t->first);
    t->first = t->last = t->end = NULL;
    return 0;
}
int  _crt_atexit(onexit_fn f)       { return _register_onexit_function(&g_atexit, f); }
int  _crt_at_quick_exit(onexit_fn f) { (void)f; return 0; }
int  _register_thread_local_exe_atexit_callback(void* f) { g_tls_dtor = f; return 0; }

typedef int (*initterm_e_fn)(void);
int _initterm_e(initterm_e_fn* a, initterm_e_fn* b)
{
    for (; a < b; ++a)
        if (*a) { int r = (*a)(); if (r) return r; }
    return 0;
}

void exit(int code)   { _execute_onexit_table(&g_atexit); msvcrt_exit(code); }
void _exit(int code)  { msvcrt_exit(code); }
void _cexit(void)     { _execute_onexit_table(&g_atexit); }
void _c_exit(void)    {}
void quick_exit(int code) { msvcrt_exit(code); }

int   _configure_narrow_argv(int mode)      { (void)mode; return 0; }
int   _initialize_narrow_environment(void)  { return 0; }
void  _set_app_type(int t)                  { (void)t; }
int   _configthreadlocale(int m)            { (void)m; return 1; }   // _ENABLE_PER_THREAD_LOCALE off
int   _controlfp_s(unsigned* cur, unsigned newv, unsigned mask)
{
    (void)newv; (void)mask;
    if (cur) *cur = 0x0008001F;                 // the default control word
    return 0;
}
// Everything after argv[0], quoted program name included.
char* _get_narrow_winmain_command_line(void)
{
    static char* cached;
    if (cached) return cached;
    char* p = GetCommandLineA();
    if (!p) { static char empty[1]; return empty; }
    if (*p == '"') { for (++p; *p && *p != '"'; ++p) {} if (*p) ++p; }
    else for (; *p && *p != ' ' && *p != '\t'; ++p) {}
    while (*p == ' ' || *p == '\t') ++p;
    cached = p;
    return p;
}
void terminate(void)
{
    dbg("ucrt: terminate() called");
    abort();
}
void _invalid_parameter_noinfo(void) { dbg("ucrt: invalid parameter"); }
void _invalid_parameter_noinfo_noreturn(void)
{
    dbg("ucrt: invalid parameter (noreturn)");
    abort();
}
void _wassert(const wchar* msg, const wchar* file, unsigned line)
{
    char m[256], fl[256];
    size_t i = 0;
    for (; msg && msg[i] && i < 255; ++i) m[i] = (char)msg[i];
    m[i] = 0;
    for (i = 0; file && file[i] && i < 255; ++i) fl[i] = (char)file[i];
    fl[i] = 0;
    dbgf("ucrt: assertion failed: %s (%s:%u)", m, fl, line);
    abort();
}
// The CRT's top-level __except filter. Declining is deliberate: the loader
// reports an unhandled guest exception with a stack, and swallowing the fault
// here would turn a crash into a silent exit.
int _seh_filter_exe(u32 code, void* ptrs)
{
    (void)ptrs;
    dbgf("ucrt: _seh_filter_exe code=%08X -- not handling", code);
    return 0;                                   // EXCEPTION_CONTINUE_SEARCH
}
int _seh_filter_dll(u32 code, void* ptrs) { return _seh_filter_exe(code, ptrs); }

int _spawnv(int mode, const char* path, const char* const* argv)
{
    (void)mode; (void)argv;
    dbgf("ucrt: _spawnv(%s) refused -- no process creation in the container",
         path ? path : "(null)");
    errno = EINVAL;
    return -1;
}

// ---- vcruntime140: exceptions ---------------------------------------------
typedef struct {
    u32 code, flags;
    void* record;
    void* address;
    u32 nparams;
    u32 info[15];
} EXC_RECORD;
typedef struct { EXC_RECORD* rec; void* ctx; } EXC_POINTERS;

typedef struct { const char* what; char do_free; } std_exc;

void __std_exception_copy(const std_exc* from, std_exc* to)
{
    if (!from || !to) return;
    if (from->do_free && from->what) {
        size_t n = strlen(from->what) + 1;
        char* p = (char*)malloc(n);
        if (p) { memcpy(p, from->what, n); to->what = p; to->do_free = 1; return; }
    }
    to->what = from->what;
    to->do_free = 0;
}
void __std_exception_destroy(std_exc* e)
{
    if (!e) return;
    if (e->do_free && e->what) free((void*)e->what);
    e->what = NULL;
    e->do_free = 0;
}
static void* g_cur_exc;
static void* g_cur_exc_ctx;
void** __current_exception(void)         { return &g_cur_exc; }
void** __current_exception_context(void) { return &g_cur_exc_ctx; }
void   __std_terminate(void)             { terminate(); }
void   __std_type_info_destroy_list(void* l) { (void)l; }
void   _purecall(void) { dbg("ucrt: pure virtual call"); abort(); }
void*  _set_se_translator(void* f) { (void)f; return NULL; }

// A C++ throw. RaiseException is the right shape -- the loader walks FS:[0]
// and offers it to each guest frame -- but the frames that would catch it
// register __CxxFrameHandler3, and that one is not implemented (below), so a
// throw that was meant to be caught will end the guest with a report instead
// of being swallowed silently.
void _CxxThrowException(void* object, void* throw_info)
{
    u32 args[3] = { 0x19930520u, (u32)(uintptr_t)object, (u32)(uintptr_t)throw_info };
    dbgf("ucrt: _CxxThrowException object=%08X info=%08X",
         (unsigned)(uintptr_t)object, (unsigned)(uintptr_t)throw_info);
    RaiseException(0xE06D7363u, 1 /*NONCONTINUABLE*/, 3, args);
}
// Not implemented: MSVC's C++ EH tables (FuncInfo, TryBlockMap, catch type
// matching, the unwind map) are a separate job from getting a title to boot,
// and nothing in Half-Life 1 is known to throw on a working path. Declining is
// honest -- the throw becomes an unhandled exception the loader reports by
// name -- where guessing would corrupt the guest's stack.
static int g_cxx_n;
int __CxxFrameHandler3(void* rec, void* frame, void* ctx, void* disp)
{
    (void)frame; (void)ctx; (void)disp;
    // Say WHICH exception walked past. Declining is only interesting when the
    // exception is a C++ throw (0xE06D7363) that a frame here meant to catch:
    // for any other code this handler is just one more frame on the FS:[0]
    // chain and has nothing to do with the failure being chased. Half-Life 1
    // reached this line on its boot path, and "reached" alone did not say
    // whether that was the cause or a bystander.
    const u32* r = (const u32*)rec;
    if (g_cxx_n++ < 8 && r)
        dbgf("ucrt: __CxxFrameHandler3 code=%08X flags=%08X at=%08X nparam=%u"
             " -- C++ EH is not implemented, declining%s",
             (unsigned)r[0], (unsigned)r[1], (unsigned)r[3], (unsigned)r[4],
             r[0] == 0xE06D7363u ? " (this IS a C++ throw: it will not be caught)"
                                 : " (not a C++ throw: this frame is a bystander)");
    return 1;                                   // ExceptionContinueSearch
}
static int g_rtti_once;
void* __RTDynamicCast(void* p, long delta, void* src, void* target, int is_ref)
{
    (void)delta; (void)src; (void)target; (void)is_ref;
    if (once(&g_rtti_once))
        dbg("ucrt: __RTDynamicCast is not implemented -- returning NULL "
            "(a dynamic_cast will read as a type mismatch)");
    return NULL;
}

// ---- vcruntime140: SEH4, which does run ------------------------------------
// MSVC's /GS + __try frames register __except_handler4, a per-module thunk
// that calls this with the module's security cookie. The loader's FS:[0] walk
// calls that thunk, so this is the code that actually decides whether a guest
// __except block runs.
//
// The registration record, from the handler's point of view:
//   +0 prev  +4 handler  +8 scopetable ^ cookie  +0xC trylevel  +0x10 saved ebp
// and the scopetable has a four-int cookie header before its records.
//
// **EXCEPTION_POINTERS lives BELOW the record, at EstablisherFrame-4**, not
// above it. Read it off __SEH_prolog4: it does `lea ebp,[esp+0x10]` and then
// `lea eax,[ebp-0x10]; mov fs:[0],eax`, so EstablisherFrame == ebp-0x10, the
// saved ebp at +0x10 IS the frame pointer, and +0x14 is the protected
// function's RETURN ADDRESS. Half-Life 1's `GetExceptionInformation()` reads
// `[ebp-0x14]`, which is EstablisherFrame-4.
//
// Writing the pointers at +0x14 therefore did two things at once: it left the
// slot the filter reads holding whatever was last on that stack (0x23, a
// leftover compat-mode CS), and it overwrote the return address. The filter
// dereferenced 0x23, faulted, re-entered this handler, and the run spun until
// the fault-loop guard ended it. HL1 is the first guest to use SEH4 at all --
// Half-Life 2's CRT is old enough to use __except_handler3 -- so the mistake
// had never been exercised.
typedef struct { int enclosing; void* filter; void* handler; } ScopeRec;
typedef struct { int gs_off, gs_xor, eh_off, eh_xor; ScopeRec rec[1]; } ScopeTable4;
typedef struct {
    void* prev;
    void* handler;
    u32   scopetable;
    int   trylevel;
    int   ebp;
} EH4_FRAME;

#define TRYLEVEL_NONE (-2)

// Both funclets run with EBP restored to the protected function's frame: that
// is how they reach its locals. The filter also gets the pointers pushed, as
// the compiler's own __except_handler4 does.
static int call_filter4(void* func, void* ptrs, void* ebp)
{
    int ret;
    __asm__ __volatile__("pushl %%ebp\n\t"
                         "pushl %3\n\t"
                         "movl %2, %%ebp\n\t"
                         "call *%%eax\n\t"
                         "popl %%ebp\n\t"
                         "popl %%ebp"
                         : "=a"(ret)
                         : "0"(func), "r"(ebp), "r"(ptrs)
                         : "ecx", "edx", "memory");
    return ret;
}
static int call_funclet4(void* func, void* ebp)
{
    int ret;
    __asm__ __volatile__("pushl %%ebp\n\t"
                         "movl %2, %%ebp\n\t"
                         "call *%%eax\n\t"
                         "popl %%ebp"
                         : "=a"(ret)
                         : "0"(func), "r"(ebp)
                         : "ecx", "edx", "memory");
    return ret;
}
static ScopeTable4* scopetable_of(EH4_FRAME* f, u32 cookie)
{
    return (ScopeTable4*)(uintptr_t)(f->scopetable ^ cookie);
}
// Runs the __finally blocks from the frame's current trylevel down to `stop`.
static void local_unwind4(u32* cookie, EH4_FRAME* f, int stop)
{
    ScopeTable4* st = scopetable_of(f, *cookie);
    int level = f->trylevel;
    while (level != stop && level != TRYLEVEL_NONE) {
        if (level < 0) break;
        ScopeRec* r = &st->rec[level];
        int enclosing = r->enclosing;
        f->trylevel = enclosing;
        if (!r->filter && r->handler) call_funclet4(r->handler, (void*)&f->ebp);
        level = enclosing;
    }
}
void _local_unwind4(u32* cookie, EH4_FRAME* f, int stop) { local_unwind4(cookie, f, stop); }

int _except_handler4_common(u32* cookie, void* check, EXC_RECORD* rec,
                            EH4_FRAME* f, void* ctx, void* dispatch)
{
    (void)check; (void)dispatch;
    if (rec->flags & 6) {                       // UNWINDING | EXIT_UNWIND
        local_unwind4(cookie, f, TRYLEVEL_NONE);
        return 1;                               // ExceptionContinueSearch
    }
    ScopeTable4* st = scopetable_of(f, *cookie);
    EXC_POINTERS ptrs = { rec, ctx };
    ((EXC_POINTERS**)f)[-1] = &ptrs;            // EstablisherFrame-4; see above

    for (int level = f->trylevel; level != TRYLEVEL_NONE; ) {
        if (level < 0) break;
        ScopeRec* r = &st->rec[level];
        if (r->filter) {
            int disp = call_filter4(r->filter, &ptrs, (void*)&f->ebp);
            if (disp == -1) return 0;           // CONTINUE_EXECUTION
            if (disp == 1) {                    // EXECUTE_HANDLER
                RtlUnwind(f, NULL, NULL, NULL); // outer frames run their finallys
                local_unwind4(cookie, f, level);
                f->trylevel = r->enclosing;
                call_funclet4(r->handler, (void*)&f->ebp);   // does not return
                return 0;
            }
        }
        level = r->enclosing;
    }
    return 1;                                   // ExceptionContinueSearch
}

// ---- msvcp140: the handful of names sw.dll and tier0.dll pull in ----------
void _Mtx_init_in_situ(void* m, int type)  { (void)type; InitializeCriticalSection((CRITICAL_SECTION*)m); }
void _Mtx_destroy_in_situ(void* m)         { DeleteCriticalSection((CRITICAL_SECTION*)m); }
int  _Mtx_lock(void* m)                    { EnterCriticalSection((CRITICAL_SECTION*)m); return 0; }
int  _Mtx_unlock(void* m)                  { LeaveCriticalSection((CRITICAL_SECTION*)m); return 0; }

void gh_xlength_error(const char* what)
{
    dbgf("ucrt: std::length_error: %s", what ? what : "(null)");
    abort();
}
void gh_throw_c_error(int code)
{
    dbgf("ucrt: std::_Throw_C_error(%d)", code);
    abort();
}

// ---- the api-set anchor ---------------------------------------------------
// Each api-ms-win-crt-*.dll (and vcruntime140/msvcp140) is a forwarder-only
// DLL whose exports all point here, so there is exactly one copy of the CRT's
// state no matter which set a module imports from. A forwarder is only
// resolvable once its target is mapped, and a DLL with no imports would never
// pull this one in -- so each of them imports this function and calls it from
// its DllMain.
int _gh_ucrt_anchor(void) { return 1; }
