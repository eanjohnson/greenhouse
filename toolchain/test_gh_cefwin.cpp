// Host test for src/loader/gh_cefwin.h (run by test_gh_cefwin.sh under wine).
// Plays the parts of Chromium that broke in the container: WindowImpl's
// WM_NCCREATE handshake, MessagePumpForUI's PostMessage(kMsgHaveWork) wake-up
// from another thread, its MsgWaitForMultipleObjectsEx sleep, and SetTimer.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

static void Diag(const std::string&) {}
static std::string Hex(uintptr_t v){ char b[32]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); return b; }
// The loader's window beds; nothing in this test reaches them.
static BOOL WINAPI Bed_DestroyWindow(HWND){ return FALSE; }
static LRESULT WINAPI Bed_DefWindowProcW(HWND, UINT, WPARAM, LPARAM){ return 0; }
static LONG_PTR WINAPI Bed_GetWindowLongPtrW(HWND, int){ return 0; }
static LONG_PTR WINAPI Bed_SetWindowLongPtrW(HWND, int, LONG_PTR){ return 0; }
static BOOL WINAPI Bed_IsWindow(HWND){ return FALSE; }
static DWORD WINAPI Bed_GetWindowThreadProcessId(HWND, LPDWORD){ return 0; }
static HWND WINAPI Bed_GetAncestor(HWND, UINT){ return nullptr; }
static BOOL WINAPI Bed_GetWindowRect(HWND, LPRECT){ return FALSE; }
static BOOL WINAPI Bed_GetClientRect(HWND, LPRECT){ return FALSE; }
static HWND WINAPI Bed_GetWindow(HWND, UINT){ return nullptr; }
static LRESULT WINAPI Bed_DispatchMessageW(const MSG*){ return 0; }
#include "../src/loader/gh_cefwin.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

template <class F> static F Fn(const char* n){ return (F)cefwin::Lookup(n); }

// WindowImpl: the HWND arrives in WM_NCCREATE, `this` rides in lpCreateParams.
struct Impl { HWND hwnd = nullptr; int ncc = 0, create = 0, destroy = 0, work = 0, timer = 0; };
static LRESULT CALLBACK ImplProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto setl = Fn<decltype(&SetWindowLongPtrW)>("SetWindowLongPtrW");
    auto getl = Fn<decltype(&GetWindowLongPtrW)>("GetWindowLongPtrW");
    if (m == WM_NCCREATE){
        Impl* p = (Impl*)((CREATESTRUCTW*)l)->lpCreateParams;
        p->hwnd = h; ++p->ncc; setl(h, GWLP_USERDATA, (LONG_PTR)p);
    }
    Impl* p = (Impl*)getl(h, GWLP_USERDATA);
    if (!p) return Fn<decltype(&DefWindowProcW)>("DefWindowProcW")(h, m, w, l);
    if (m == WM_CREATE) ++p->create;
    if (m == WM_DESTROY) ++p->destroy;
    if (m == WM_USER + 1) ++p->work;
    if (m == WM_TIMER) ++p->timer;
    return m == WM_NCCREATE ? TRUE : 0;
}

int main()
{
    auto reg   = Fn<decltype(&RegisterClassExW)>("RegisterClassExW");
    auto make  = Fn<decltype(&CreateWindowExW)>("CreateWindowExW");
    auto kill  = Fn<decltype(&DestroyWindow)>("DestroyWindow");
    auto isw   = Fn<decltype(&IsWindow)>("IsWindow");
    auto post  = Fn<decltype(&PostMessageW)>("PostMessageW");
    auto peek  = Fn<decltype(&PeekMessageW)>("PeekMessageW");
    auto disp  = Fn<decltype(&DispatchMessageW)>("DispatchMessageW");
    auto mwait = Fn<decltype(&MsgWaitForMultipleObjectsEx)>("MsgWaitForMultipleObjectsEx");
    auto stimer= Fn<decltype(&SetTimer)>("SetTimer");
    auto ktimer= Fn<decltype(&KillTimer)>("KillTimer");
    auto tid   = Fn<decltype(&GetWindowThreadProcessId)>("GetWindowThreadProcessId");
    CHECK(reg && make && kill && isw && post && peek && disp && mwait && stimer && ktimer && tid);

    WNDCLASSEXW wc{ sizeof wc }; wc.lpfnWndProc = ImplProc; wc.lpszClassName = L"Chrome_MessageWindow";
    const ATOM a = reg(&wc);
    CHECK(a != 0);

    // 1. Creation handshake (hwnd_util.cc(81) / window_impl.cc(148)).
    Impl impl;
    HWND h = make(0, (LPCWSTR)(ULONG_PTR)a, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, &impl);
    CHECK(h && h == impl.hwnd && impl.ncc == 1 && impl.create == 1 && isw(h));
    CHECK(tid(h, nullptr) == GetCurrentThreadId());
    CHECK(make(0, L"NoSuchClass", L"", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr) == nullptr);

    // 2. An idle pump sleeps: 300 ms with nothing queued costs one wait, not a spin.
    MSG m;
    while (peek(&m, nullptr, 0, 0, PM_REMOVE)) {}
    DWORD t0 = GetTickCount();
    CHECK(mwait(0, nullptr, 300, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_TIMEOUT);
    DWORD dt = GetTickCount() - t0;
    CHECK(dt >= 250 && dt < 1000);

    // 3. ScheduleWork from another thread wakes the sleeping pump at once.
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        Sleep(100);
        Fn<decltype(&PostMessageW)>("PostMessageW")((HWND)p, WM_USER + 1, 0, 0);
        return 0;
    }, h, 0, nullptr);
    t0 = GetTickCount();
    CHECK(mwait(0, nullptr, 5000, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0);
    dt = GetTickCount() - t0;
    CHECK(dt < 1000);
    CHECK(peek(&m, h, 0, 0, PM_REMOVE) && m.message == WM_USER + 1);
    disp(&m);
    CHECK(impl.work == 1);
    WaitForSingleObject(th, INFINITE); CloseHandle(th);

    // 4. A waited-on handle is reported as itself, not as the queue.
    HANDLE ev = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    CHECK(mwait(1, &ev, 1000, QS_ALLINPUT, 0) == WAIT_OBJECT_0);
    CloseHandle(ev);

    // 5. Delayed work: a window timer wakes the wait and dispatches WM_TIMER.
    CHECK(stimer(h, 7, 50, nullptr) != 0);   // a window timer returns nonzero
    int ticks = 0; t0 = GetTickCount();
    while (ticks < 3 && GetTickCount() - t0 < 3000){
        if (mwait(0, nullptr, 2000, QS_ALLINPUT, 0) != WAIT_OBJECT_0) continue;
        while (peek(&m, nullptr, 0, 0, PM_REMOVE)){ if (m.message == WM_TIMER) ++ticks; disp(&m); }
    }
    CHECK(ticks == 3 && impl.timer == 3);
    dt = GetTickCount() - t0;
    CHECK(dt >= 100 && dt < 1000);
    CHECK(ktimer(h, 7));

    // 6. Thread messages (hwnd null) and the PeekMessage(-1) filter.
    CHECK(post(nullptr, WM_USER + 2, 0, 0));
    CHECK(peek(&m, (HWND)-1, 0, 0, PM_REMOVE) && m.message == WM_USER + 2 && !m.hwnd);

    // 7. Teardown sends WM_DESTROY and forgets the window.
    CHECK(kill(h) && impl.destroy == 1 && !isw(h));

    printf(g_fail ? "test_gh_cefwin: %d FAILED\n" : "test_gh_cefwin: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
