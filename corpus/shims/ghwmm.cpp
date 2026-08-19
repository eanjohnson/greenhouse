// ghwmm.dll -- a tiny WINMM replacement for Greenhouse guests whose runtime
// imports the multimedia-timer functions that the Xbox container's slimmed
// WINMM.dll does not export.
//
// Unity's Mono runtime (mono-2.0-bdwgc.dll) imports five WINMM functions:
//   timeGetDevCaps, timeBeginPeriod, timeEndPeriod  -- present in the container
//   timeSetEvent, timeKillEvent                     -- ABSENT (0x7F on load)
//
// We cannot ship a drop-in "winmm.dll" because the real (slim) WINMM is already
// resident (UnityPlayer.dll imports it), so a same-named preload would be
// ignored. Instead we ship this DLL under a unique name and binary-patch mono's
// import descriptor "WINMM.dll" -> "ghwmm.dll" (same length). All five functions
// then resolve here. timeSetEvent/timeKillEvent are implemented for real on top
// of the kernel32 timer-queue APIs, which the container does provide.
#define WIN32_LEAN_AND_MEAN   // exclude mmsystem.h so its winmm prototypes don't clash
#include <windows.h>

typedef void (CALLBACK *LPTIMECALLBACK)(UINT uID, UINT uMsg, DWORD_PTR dwUser,
                                        DWORD_PTR dw1, DWORD_PTR dw2);

#define TIMERR_NOERROR 0
#define TIMERR_NOCANDO 97
#define TIME_PERIODIC  0x0001

namespace {
struct TimerSlot {
    HANDLE         h;      // timer-queue timer handle
    LPTIMECALLBACK cb;
    DWORD_PTR      user;
    UINT           id;
    volatile LONG  used;
};
TimerSlot g_slots[64];

void CALLBACK TimerQueueThunk(PVOID param, BOOLEAN /*fired*/)
{
    auto* s = (TimerSlot*)param;
    if (s->used && s->cb) s->cb(s->id, 0, s->user, 0, 0);
}
}

extern "C" {

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

// TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; }
__declspec(dllexport) UINT WINAPI timeGetDevCaps(void* ptc, UINT cbtc)
{
    if (ptc && cbtc >= 8){ ((UINT*)ptc)[0] = 1; ((UINT*)ptc)[1] = 1000000; }
    return TIMERR_NOERROR;
}
__declspec(dllexport) UINT WINAPI timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
__declspec(dllexport) UINT WINAPI timeEndPeriod(UINT)   { return TIMERR_NOERROR; }

// Periodic (or one-shot) timer that invokes `cb` every `delay` ms. Returns a
// nonzero timer id used by timeKillEvent; 0 on failure (as the real API does).
__declspec(dllexport) UINT WINAPI timeSetEvent(UINT delay, UINT /*res*/,
                                               LPTIMECALLBACK cb, DWORD_PTR user, UINT flags)
{
    for (int i = 0; i < 64; ++i){
        if (InterlockedCompareExchange(&g_slots[i].used, 1, 0) == 0){
            TimerSlot* s = &g_slots[i];
            s->cb = cb; s->user = user; s->id = (UINT)(i + 1); s->h = nullptr;
            DWORD period = (flags & TIME_PERIODIC) ? delay : 0;   // 0 => one-shot
            if (!CreateTimerQueueTimer(&s->h, nullptr, &TimerQueueThunk, s,
                                       delay, period, WT_EXECUTEDEFAULT)){
                s->used = 0; return 0;
            }
            return s->id;
        }
    }
    return 0;
}
__declspec(dllexport) UINT WINAPI timeKillEvent(UINT id)
{
    if (id < 1 || id > 64) return TIMERR_NOCANDO;
    TimerSlot* s = &g_slots[id - 1];
    if (s->used){
        if (s->h) DeleteTimerQueueTimer(nullptr, s->h, nullptr);
        s->h = nullptr; s->cb = nullptr; s->used = 0;
    }
    return TIMERR_NOERROR;
}

}
