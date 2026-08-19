// P0 spike (GREENHOUSE.md §10 / Appendix H.3 #4): prove the exec-memory sequence
// the whole Loader design depends on. This build targets plain Win32 so it can be
// compiled and run entirely off-console (Wine/local Windows) to validate the code
// itself; the real question -- whether Xbox Dev Mode's UWP container permits this
// same sequence -- can only be answered by running the UWP-wrapped equivalent on
// the console (tracked separately once the Host shell exists).
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

// x64 machine code for: mov eax, 42 ; ret
static const uint8_t kStub[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };

int main(void) {
    printf("[p0_spike] allocating RW page...\n");
    void* mem = VirtualAlloc(nullptr, sizeof(kStub), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        printf("[p0_spike] FAIL: VirtualAlloc error=%lu\n", GetLastError());
        return 1;
    }

    memcpy(mem, kStub, sizeof(kStub));

    printf("[p0_spike] switching page to PAGE_EXECUTE_READ...\n");
    DWORD oldProtect = 0;
    if (!VirtualProtect(mem, sizeof(kStub), PAGE_EXECUTE_READ, &oldProtect)) {
        printf("[p0_spike] FAIL: VirtualProtect error=%lu\n", GetLastError());
        VirtualFree(mem, 0, MEM_RELEASE);
        return 1;
    }

    printf("[p0_spike] calling into allocated memory...\n");
    typedef int (*StubFn)(void);
    StubFn fn = reinterpret_cast<StubFn>(mem);
    int result = fn();

    printf("[p0_spike] result = %d (expected 42)\n", result);

    VirtualFree(mem, 0, MEM_RELEASE);

    if (result == 42) {
        printf("[p0_spike] PASS\n");
        return 0;
    }
    printf("[p0_spike] FAIL: unexpected result\n");
    return 1;
}
