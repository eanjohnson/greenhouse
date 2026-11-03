// Assembles the native-x86 gates at a fixed base and writes them out so they
// can be disassembled on the build host:  ./gates_dump out_dir
#include "../../src/loader/x86/gh_x86_native_gates.h"
#include <cstdio>
using namespace ghx86;
int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    std::vector<uint8_t> code; NativeGates g;
    const uint32_t base = 0x10000000;
    if (!EmitNativeGates(base, 0x1122334455667788ull, 0x99AABBCCDDEEFF00ull, code, g)) return 1;
    char p[512]; snprintf(p, sizeof p, "%s/gates.bin", dir);
    FILE* f = fopen(p, "wb"); fwrite(code.data(), 1, code.size(), f); fclose(f);
    uint8_t slot[16]; EmitNativeSlot(slot, 0x20000010, 1, g.common32);
    snprintf(p, sizeof p, "%s/slot.bin", dir);
    f = fopen(p, "wb"); fwrite(slot, 1, 16, f); fclose(f);
    printf("resume_tail %X enter64 %X exit64 %X hc64 %X fault64 %X resume64 %X common32 %X exit32 %X end %X\n",
           g.resume_tail - base, g.enter64 - base, g.exit64 - base, g.hc64 - base, g.fault64 - base,
           g.resume64 - base, g.common32 - base, g.exit32 - base, g.end - base);
    return 0;
}
