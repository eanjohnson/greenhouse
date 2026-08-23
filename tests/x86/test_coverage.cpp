// Which instructions in a real guest binary can the interpreter not decode?
//
// The x86 long tail was being discovered one console deploy at a time: run,
// read "unimplemented opcode NN" out of loader_diag.txt, implement it, build,
// pack, sign, deploy, launch, wait two minutes, find the next one. That is a
// twenty-minute round trip to learn a single byte, and §30.x already records
// what it cost. objdump knows every instruction in the image, so the whole
// question can be answered here, in one pass, before anything is deployed.
//
// toolchain/x86_opcode_corpus.py turns a binary into one line per distinct
// encoding -- real bytes lifted out of the image, not hand-encoded from a
// manual -- with the number of times it occurs. This steps each one and reports
// the ones the decoder rejects, most frequent first.
//
// A memory fault is NOT a failure here: an instruction lifted out of context
// has whatever operands it had in the image, and most of those addresses mean
// nothing in this harness. Only "unimplemented opcode" counts.
//
// Build/run:
//   python3 toolchain/x86_opcode_corpus.py game.exe mono.dll > /tmp/corpus.txt
//   g++ -O2 -std=c++20 -I src/loader/x86 tests/x86/test_coverage.cpp -o /tmp/cov
//   /tmp/cov /tmp/corpus.txt

#include "gh_x86_cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ghx86;

static constexpr uint32_t kGuestBase = 0x10000;
static constexpr uint32_t kGuestSize = 0x40000;
static constexpr uint32_t kCodeAddr  = 0x11000;
static constexpr uint32_t kScratch   = 0x20000;   // every register points here

struct Row {
    std::vector<uint8_t> bytes;
    uint64_t             count = 0;
    std::string          mnem, where;
};

static bool parse(const char* path, std::vector<Row>& out)
{
    FILE* f = std::fopen(path, "r");
    if (!f) { std::perror(path); return false; }
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char hex[256] = {0}, mnem[64] = {0}, where[128] = {0};
        unsigned long long n = 0;
        if (std::sscanf(line, "%255s %llu %63s %127s", hex, &n, mnem, where) < 3) continue;
        Row r; r.count = n; r.mnem = mnem; r.where = where;
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
            auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
            r.bytes.push_back((uint8_t)((nib(hex[i]) << 4) | nib(hex[i + 1])));
        }
        if (!r.bytes.empty()) out.push_back(std::move(r));
    }
    std::fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <corpus.txt>\n", argv[0]); return 2; }
    std::vector<Row> rows;
    if (!parse(argv[1], rows)) return 2;

    std::vector<uint8_t> mem(kGuestSize, 0);
    std::vector<const Row*> missing;
    uint64_t missing_weight = 0, total_weight = 0;

    for (const Row& r : rows) {
        total_weight += r.count;

        Cpu cpu;
        cpu.mem_base  = (uintptr_t)mem.data() - kGuestBase;
        cpu.mem_limit = kGuestBase + kGuestSize;
        cpu.fs_base   = kScratch;                 // FS:[n] must land somewhere real
        for (int i = 0; i < 8; ++i) cpu.gpr[i] = kScratch;
        cpu.gpr[REG_ESP] = kScratch + 0x1000;     // room to push and to pop
        cpu.eflags = FLAG_IF | 0x2;
        cpu.eip = kCodeAddr;

        std::memset(mem.data(), 0, mem.size());
        std::memcpy(mem.data() + (kCodeAddr - kGuestBase), r.bytes.data(), r.bytes.size());

        cpu.step();
        if (cpu.fault_why.rfind("unimplemented", 0) == 0) {
            missing.push_back(&r);
            missing_weight += r.count;
        }
    }

    std::printf("%zu distinct encodings, %llu instructions total\n",
                rows.size(), (unsigned long long)total_weight);
    if (missing.empty()) {
        std::printf("all decode. \n");
        return 0;
    }
    std::printf("%zu NOT DECODED (%llu instructions, %.3f%% of the image):\n\n",
                missing.size(), (unsigned long long)missing_weight,
                100.0 * (double)missing_weight / (double)total_weight);
    std::printf("  %-24s %10s  %-12s %s\n", "bytes", "count", "mnemonic", "seen in");
    for (const Row* r : missing) {
        std::string hex;
        for (uint8_t b : r->bytes) { char t[4]; std::snprintf(t, 4, "%02x", b); hex += t; }
        std::printf("  %-24s %10llu  %-12s %s\n", hex.c_str(),
                    (unsigned long long)r->count, r->mnem.c_str(), r->where.c_str());
    }
    // Not a test failure: this is a report, and the caller decides what is worth
    // implementing. Returning non-zero would make it useless in a build script.
    return 0;
}
