// sample_addon.cpp -- the smallest Greenhouse add-on (gh_addon_abi.h).
//
// A library source that reports one row. It exists to prove the add-on path end
// to end on the console -- signed package -> install -> hash check -> LoadLibrary
// -> GhAddonInit -> picker row -- with nothing else moving. Pressing A on the row
// runs acquire(), which fakes a 2 s "download" through host->progress and leaves
// host->status on screen, so the host's drawing callbacks are exercised too. Build: toolchain/build-addon.sh corpus/addons/sample/sample_addon.cpp OUT.dll
#include "../../../src/loader/gh_addon_abi.h"
#include <windows.h>
#include <stdio.h>

static const GhHostApi* g_host = nullptr;

static void Enumerate(void* ctx, void (*emit)(void*, const GhAddonRow*))
{
    GhAddonRow r = {};
    r.size = sizeof r;
    r.kind = GH_ROW_ACQUIRE;
    r.display = L"Sample add-on";
    r.sub = L"A: test progress";
    emit(ctx, &r);
    if (g_host && g_host->log) g_host->log("ADDON sample: enumerate -> 1 row");
}

static int Acquire(const GhAddonRow*, int mode)
{
    if (!GH_HOST_HAS(g_host, progress) || !GH_HOST_HAS(g_host, status)) {
        if (g_host->log) g_host->log("ADDON sample: host has no progress/status");
        return 0;
    }
    const uint64_t total = 20ull << 20;               // a pretend 20 MB
    for (int i = 0; i <= 40; ++i) {
        g_host->progress(L"sample transfer", total * i / 40, total);
        Sleep(50);
    }
    g_host->status(L"Sample add-on: progress callback OK");
    Sleep(1000);
    char line[96];
    _snprintf_s(line, sizeof line, _TRUNCATE, "ADDON sample: acquire mode=%d drew 41 progress frames + status", mode);
    g_host->log(line);
    return 0;                                         // nothing installed: the row stays
}

extern "C" __declspec(dllexport) int GhAddonInit(const GhHostApi* host, uint32_t hostAbi, GhAddonApi* out)
{
    if (!host || !out || hostAbi != GH_ADDON_ABI || out->size < sizeof(GhAddonApi)) return 1;
    g_host = host;
    out->size = sizeof(GhAddonApi);
    out->abi = GH_ADDON_ABI;
    out->id = "sample";
    out->service = nullptr;
    out->enumerate = Enumerate;
    out->acquire = Acquire;
    if (host->size >= sizeof(GhHostApi) && host->log) host->log("ADDON sample: GhAddonInit ok");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
