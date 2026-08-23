// Maps a real PE32 and executes it under the interpreter -- the M1 exit criterion,
// proven natively before any of it goes near the console.
//
// The guest (tests/x86/pe32guest.c) is a genuine PE32 from clang-cl + lld-link
// with .text/.rdata/.data/.reloc and no CRT. It is loaded AWAY from its preferred
// ImageBase on purpose so pe32_relocate's HIGHLOW path runs for real, and the
// same source compiled natively supplies the expected answer.
//
// Also usable as a triage tool for any 32-bit binary:
//   ./test_pe32 /path/to/some.exe          -- parse + report, run if it has no imports
//
// Build:
//   bash tests/x86/build_pe32.sh
//   gcc -O2 -c tests/x86/pe32guest.c -o /tmp/pe32guest_native.o
//   g++ -O2 -std=c++20 -I src/loader/x86 tests/x86/test_pe32.cpp /tmp/pe32guest_native.o -o /tmp/test_pe32

#include "gh_pe32.h"
#include "gh_x86_cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ghx86;

extern "C" unsigned ghmain(void);          // the native oracle

// Guest address space, inside a host buffer (mem_base != 0). Chosen so the image
// lands well away from its preferred 0x400000.
static constexpr uint32_t kGuestBase  = 0x00010000;
static constexpr uint32_t kGuestSize  = 0x00200000;
static constexpr uint32_t kTebAddr    = 0x00020000;
static constexpr uint32_t kImageBase  = 0x00100000;
static constexpr uint32_t kStackTop   = 0x00200000;
static constexpr uint32_t kStackSize  = 0x00040000;

static bool read_file(const char* path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)(n > 0 ? n : 0));
    bool ok = (n <= 0) || std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

// Minimal 32-bit TEB. Only the fields a guest actually reads early matter here;
// the rest arrives with the thunk layer in M2.
static void build_teb32(uint8_t* mem, uint32_t teb, uint32_t stack_base, uint32_t stack_limit)
{
    uint8_t* t = mem + (teb - kGuestBase);
    auto put = [&](uint32_t off, uint32_t v) { std::memcpy(t + off, &v, 4); };
    std::memset(t, 0, 0x1000);
    put(0x00, 0xFFFFFFFFu);      // ExceptionList: end of the SEH chain
    put(0x04, stack_base);       // StackBase (high)
    put(0x08, stack_limit);      // StackLimit (low)
    put(0x18, teb);              // Self
    put(0x2C, 0);                // ThreadLocalStoragePointer (M2)
    put(0x30, 0);                // PEB (M2)
    put(0x34, 0);                // LastErrorValue
}

int main(int argc, char** argv)
{
    std::string path = (argc > 1) ? argv[1] : "/tmp/ghx86_cases/pe32guest.exe";

    std::vector<uint8_t> file;
    if (!read_file(path.c_str(), file)) {
        std::printf("cannot read %s\n", path.c_str());
        return 2;
    }

    Pe32Info info;
    std::string err;
    if (!pe32_parse(file.data(), file.size(), info, err)) {
        std::printf("parse failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("%s\n", path.c_str());
    std::printf("  ImageBase=0x%08X SizeOfImage=0x%X entry=+0x%X subsystem=%u %s\n",
                info.image_base, info.image_size, info.entry_rva, info.subsystem,
                info.is_dll ? "DLL" : "EXE");
    for (const auto& s : info.sections)
        std::printf("    %-8s rva=0x%06X vsize=0x%06X raw=0x%06X/0x%06X %s%s%s\n",
                    s.name, s.vaddr, s.vsize, s.raw_ptr, s.raw_size,
                    (s.characteristics & pe::SCN_MEM_READ) ? "R" : "-",
                    (s.characteristics & pe::SCN_MEM_WRITE) ? "W" : "-",
                    (s.characteristics & pe::SCN_MEM_EXECUTE) ? "X" : "-");

    std::vector<Pe32Import> imports;
    if (!pe32_imports(file.data(), info, imports, err)) {   // headers-only walk is fine pre-map
        std::printf("  (import walk on raw file: %s)\n", err.c_str());
    }

    if (info.image_size > kGuestSize - (kImageBase - kGuestBase)) {
        std::printf("  image too big for the test address space\n");
        return 1;
    }

    // -- map --------------------------------------------------------------
    std::vector<uint8_t> mem(kGuestSize, 0);
    uint8_t* image = mem.data() + (kImageBase - kGuestBase);

    if (!pe32_map_into(file.data(), file.size(), info, image, err)) {
        std::printf("  map failed: %s\n", err.c_str());
        return 1;
    }
    int nrelocs = 0;
    if (!pe32_relocate(image, info, kImageBase, err, &nrelocs)) {
        std::printf("  relocate failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("  mapped at 0x%08X (delta %+d), %d relocations applied\n",
                kImageBase, (int)(kImageBase - info.image_base), nrelocs);

    // Imports must be walked on the MAPPED image (RVAs are image-relative).
    imports.clear();
    pe32_imports(image, info, imports, err);
    if (!imports.empty()) {
        std::printf("  %zu imports -- M2 territory, not running:\n", imports.size());
        std::string last;
        for (size_t i = 0; i < imports.size() && i < 12; ++i) {
            if (imports[i].dll != last) { std::printf("    %s\n", imports[i].dll.c_str()); last = imports[i].dll; }
            std::printf("      %s\n", imports[i].name.empty()
                        ? ("#" + std::to_string(imports[i].ordinal)).c_str()
                        : imports[i].name.c_str());
        }
        return 0;
    }

    Pe32Tls tls;
    if (pe32_tls(image, info, tls))
        std::printf("  TLS: raw 0x%08X-0x%08X index@0x%08X callbacks@0x%08X\n",
                    tls.raw_start, tls.raw_end, tls.index_va, tls.callbacks_va);

    // -- run ---------------------------------------------------------------
    build_teb32(mem.data(), kTebAddr, kStackTop, kStackTop - kStackSize);

    Cpu cpu;
    cpu.mem_base  = (uintptr_t)mem.data() - kGuestBase;
    cpu.mem_limit = kGuestBase + kGuestSize;
    cpu.fs_base   = kTebAddr;

    uint32_t entry = kImageBase + info.entry_rva;
    cpu.call_guest(entry, kStackTop - 0x100);

    Status s = cpu.run(50000000);
    uint32_t got = cpu.gpr[REG_EAX];

    if (s != Status::Halt) {
        std::printf("  RUN FAILED: %s (eip=0x%08X, fault addr=0x%08X, %llu insns)\n",
                    cpu.fault_why.c_str(), cpu.eip, cpu.fault_addr,
                    (unsigned long long)cpu.insn_count);
        return 1;
    }

    std::printf("  entry 0x%08X returned %u in %llu instructions\n",
                entry, got, (unsigned long long)cpu.insn_count);

    if (argc > 1) return 0;                    // triage mode: no oracle to compare against

    unsigned want = ghmain();
    if (got != want) {
        std::printf("  MISMATCH: interpreter %u, native %u\n", got, want);
        return 1;
    }
    std::printf("  PASS result=%u (matches native)\n", got);
    return 0;
}
