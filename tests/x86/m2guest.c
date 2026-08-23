// The M2 test guest: a real 32-bit PE that does a genuine file round-trip
// through imported Win32 APIs.
//
// This is the mirror of P1's 7-Zip milestone -- "a real CLI app does real work
// through the bed layer" -- but hand-built so it exercises exactly the boundary
// under test and verifies its own result byte-for-byte instead of needing the
// output compared out-of-band.
//
// No CRT: the imports are declared here and satisfied by an import library
// generated with llvm-dlltool, so no 32-bit Windows SDK is needed. That also
// keeps the import list small and exactly known, which is the point at this
// milestone -- every name here is a thunk the layer has to get right.

#define GH_STDCALL __stdcall

typedef unsigned       u32;
typedef unsigned char  u8;
typedef void*          HANDLE;
typedef int            BOOL;

#define NULLPTR              ((void*)0)
#define GENERIC_READ         0x80000000u
#define GENERIC_WRITE        0x40000000u
#define CREATE_ALWAYS        2
#define OPEN_EXISTING        3
#define FILE_ATTR_NORMAL     0x80
#define INVALID_HANDLE       ((HANDLE)(long)-1)
#define HEAP_ZERO            0x00000008u

__declspec(dllimport) HANDLE GH_STDCALL GetProcessHeap(void);
__declspec(dllimport) void*  GH_STDCALL HeapAlloc(HANDLE, u32, u32);
__declspec(dllimport) BOOL   GH_STDCALL HeapFree(HANDLE, u32, void*);
__declspec(dllimport) HANDLE GH_STDCALL CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) BOOL   GH_STDCALL WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) BOOL   GH_STDCALL ReadFile(HANDLE, void*, u32, u32*, void*);
__declspec(dllimport) BOOL   GH_STDCALL CloseHandle(HANDLE);
__declspec(dllimport) u32    GH_STDCALL GetFileSize(HANDLE, u32*);
__declspec(dllimport) BOOL   GH_STDCALL DeleteFileA(const char*);
__declspec(dllimport) void   GH_STDCALL OutputDebugStringA(const char*);
__declspec(dllimport) u32    GH_STDCALL GetLastError(void);
__declspec(dllimport) void   GH_STDCALL ExitProcess(u32);

// Host -> guest direction: the bed calls this back twice, feeding the first
// result into the second call.
typedef u32 (GH_STDCALL *GhCb)(u32);
__declspec(dllimport) u32 GH_STDCALL GhTestCallback(GhCb, u32);

static u32 GH_STDCALL triple(u32 x) { return x * 3u + 1u; }

// Freestanding: no libc, and nothing large enough for clang to emit a memcpy.
static void put(char* d, const char* s)
{
    while (*s) *d++ = *s++;
    *d = 0;
}

#define N 4096

// Result is a bitmask so a partial failure says WHICH step broke rather than
// just "wrong number".
enum {
    OK_HEAP   = 1 << 0,
    OK_CREATE = 1 << 1,
    OK_WRITE  = 1 << 2,
    OK_REOPEN = 1 << 3,
    OK_SIZE   = 1 << 4,
    OK_READ   = 1 << 5,
    OK_MATCH  = 1 << 6,
    OK_DELETE = 1 << 7,
    OK_CBACK  = 1 << 8,
    OK_ALL    = 0x1FF,
};

u32 GH_STDCALL ghmain(void)
{
    char path[64];
    put(path, "gh_m2_roundtrip.bin");

    u32 status = 0;

    HANDLE heap = GetProcessHeap();
    u8* out = (u8*)HeapAlloc(heap, 0, N);
    u8* back = (u8*)HeapAlloc(heap, HEAP_ZERO, N);
    if (!out || !back) { OutputDebugStringA("m2: heap alloc failed"); return status; }
    status |= OK_HEAP;

    // A pattern that catches byte-order and off-by-one mistakes in the thunk
    // layer far better than a constant fill would.
    for (u32 i = 0; i < N; i++) out[i] = (u8)((i * 31u + (i >> 5)) & 0xFF);

    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULLPTR, CREATE_ALWAYS, FILE_ATTR_NORMAL, NULLPTR);
    if (h == INVALID_HANDLE) { OutputDebugStringA("m2: create failed"); return status; }
    status |= OK_CREATE;

    u32 wrote = 0;
    if (WriteFile(h, out, N, &wrote, NULLPTR) && wrote == N) status |= OK_WRITE;
    CloseHandle(h);

    h = CreateFileA(path, GENERIC_READ, 0, NULLPTR, OPEN_EXISTING, FILE_ATTR_NORMAL, NULLPTR);
    if (h == INVALID_HANDLE) { OutputDebugStringA("m2: reopen failed"); return status; }
    status |= OK_REOPEN;

    if (GetFileSize(h, NULLPTR) == N) status |= OK_SIZE;

    u32 got = 0;
    if (ReadFile(h, back, N, &got, NULLPTR) && got == N) status |= OK_READ;
    CloseHandle(h);

    u32 diff = 0;
    for (u32 i = 0; i < N; i++) if (out[i] != back[i]) diff++;
    if (diff == 0) status |= OK_MATCH;

    if (DeleteFileA(path)) status |= OK_DELETE;

    // triple(7)=22, triple(22)=67
    if (GhTestCallback(triple, 7) == 67) status |= OK_CBACK;

    HeapFree(heap, 0, out);
    HeapFree(heap, 0, back);

    OutputDebugStringA(status == OK_ALL ? "m2: round-trip OK" : "m2: round-trip INCOMPLETE");
    return status;
}
