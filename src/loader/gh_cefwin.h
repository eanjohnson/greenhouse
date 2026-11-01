// gh_cefwin.h -- the hidden windows and message queues of an in-process
// Chromium (GMod's libcef, §57). Included by guest_loader.cpp after the window
// beds, which it falls back to for any HWND that is not one of its own.
//
// The container's user32 refuses RegisterClassExW (0x78), and the game's window
// surrogate gives every window the one kFakeHwnd without ever sending
// WM_NCCREATE. Chromium needs neither a screen nor input from these windows --
// GMod renders it offscreen -- but it does need what a window MEANS to it:
//   * WindowImpl / MessageWindow learn their HWND only from WM_NCCREATE
//     (FATAL hwnd_util.cc(81) / window_impl.cc(148) without it);
//   * MessagePumpForUI wakes on PostMessage(kMsgHaveWork) to its message window
//     and sleeps in MsgWaitForMultipleObjectsEx, with SetTimer for delayed work.
// So libcef's user32 imports get this: unique HWNDs, per-window longs and props,
// and a queue per thread with Post/Send/Peek/Get/Dispatch, timers and a real wait.
#pragma once
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace cefwin {

struct Cls   { std::wstring name; WNDPROC proc; int extra; ATOM atom; };
struct Wnd   {
    WNDPROC proc; DWORD tid; HWND parent; DWORD style, ex;
    LONG_PTR user = 0, id = 0; std::vector<uint8_t> extra;
    std::map<std::wstring, HANDLE> props; RECT rc{}; bool visible = false; std::wstring cls;
};
struct Timer { HWND h; UINT_PTR id; UINT ms; ULONGLONG due; TIMERPROC fn; };
struct Queue { std::deque<MSG> q; HANDLE ev = nullptr; std::vector<Timer> timers; };

static std::mutex                        g_mx;       // never held across a WndProc
static std::vector<Cls>                  g_cls;
static ATOM                              g_nextAtom = 0xC1F0;
static std::unordered_map<HWND, Wnd>     g_wnd;
static uintptr_t                         g_nextH = 0x0CEF0002;   // 32-bit, like real HWNDs
static std::unordered_map<DWORD, Queue>  g_q;
static UINT_PTR                          g_nextTimer = 0x7E000;
static std::map<std::wstring, UINT>      g_wndMsgs;

static bool Ours(HWND h){ std::lock_guard<std::mutex> lk(g_mx); return g_wnd.count(h) != 0; }
static Queue& QueueFor(DWORD tid)          // g_mx held
{
    Queue& q = g_q[tid];
    if (!q.ev) q.ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return q;
}
static const Cls* FindCls(LPCWSTR name)    // g_mx held
{
    for (auto& c : g_cls){
        if ((uintptr_t)name <= 0xFFFF ? c.atom == (ATOM)(uintptr_t)name : 0 == _wcsicmp(c.name.c_str(), name))
            return &c;
    }
    return nullptr;
}
static void Push(DWORD tid, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Queue& q = QueueFor(tid);
    MSG m{}; m.hwnd = h; m.message = msg; m.wParam = wp; m.lParam = lp; m.time = GetTickCount();
    q.q.push_back(m);
    SetEvent(q.ev);
}
// Due timers become WM_TIMER, one outstanding per (hwnd,id) as on Windows.
// Returns the ms until the next one is due (INFINITE if none). g_mx held.
static DWORD FireTimers(Queue& q)
{
    const ULONGLONG now = GetTickCount64();
    DWORD next = INFINITE;
    for (auto& t : q.timers){
        if (t.due <= now){
            bool queued = false;
            for (auto& m : q.q) if (m.message == WM_TIMER && m.hwnd == t.h && m.wParam == t.id){ queued = true; break; }
            if (!queued){
                MSG m{}; m.hwnd = t.h; m.message = WM_TIMER; m.wParam = t.id; m.lParam = (LPARAM)t.fn; m.time = (DWORD)now;
                q.q.push_back(m);
            }
            t.due = now + (t.ms ? t.ms : 1);
        }
        const ULONGLONG d = t.due > now ? t.due - now : 0;
        if (d < next) next = (DWORD)d;
    }
    if (!q.q.empty()) SetEvent(q.ev);
    return next;
}
static LRESULT CallProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC p = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_wnd.find(h);
        if (it != g_wnd.end()) p = it->second.proc;
    }
    return p ? p(h, msg, wp, lp) : 0;
}

// ---- classes ---------------------------------------------------------------
static ATOM Register(LPCWSTR name, WNDPROC proc, int extra)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!name){ SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (FindCls(name)){ SetLastError(ERROR_CLASS_ALREADY_EXISTS); return 0; }
    Cls c; c.name = (uintptr_t)name <= 0xFFFF ? L"#" + std::to_wstring((uintptr_t)name) : name;
    c.proc = proc; c.extra = extra; c.atom = g_nextAtom++;
    g_cls.push_back(c);
    Diag("CEFWIN class " + std::string(c.name.begin(), c.name.end()) + " atom=" + Hex(c.atom));
    return c.atom;
}
static ATOM WINAPI RegisterClassExW_(const WNDCLASSEXW* wc){ return wc ? Register(wc->lpszClassName, wc->lpfnWndProc, wc->cbWndExtra) : 0; }
static ATOM WINAPI RegisterClassW_(const WNDCLASSW* wc){ return wc ? Register(wc->lpszClassName, wc->lpfnWndProc, wc->cbWndExtra) : 0; }
static BOOL WINAPI UnregisterClassW_(LPCWSTR name, HINSTANCE)
{
    std::lock_guard<std::mutex> lk(g_mx);
    for (size_t i = 0; i < g_cls.size(); ++i){
        const Cls& c = g_cls[i];
        if ((uintptr_t)name <= 0xFFFF ? c.atom == (ATOM)(uintptr_t)name : 0 == _wcsicmp(c.name.c_str(), name)){
            g_cls.erase(g_cls.begin() + i); return TRUE;
        }
    }
    SetLastError(ERROR_CLASS_DOES_NOT_EXIST); return FALSE;
}
static BOOL WINAPI GetClassInfoExW_(HINSTANCE inst, LPCWSTR name, WNDCLASSEXW* wc)
{
    std::lock_guard<std::mutex> lk(g_mx);
    const Cls* c = FindCls(name);
    if (!c || !wc){ SetLastError(ERROR_CLASS_DOES_NOT_EXIST); return FALSE; }
    UINT cb = wc->cbSize;
    ZeroMemory(wc, sizeof(*wc)); wc->cbSize = cb;
    wc->lpfnWndProc = c->proc; wc->cbWndExtra = c->extra; wc->hInstance = inst;
    wc->lpszClassName = name;
    return (BOOL)c->atom;
}

// ---- windows ---------------------------------------------------------------
static HWND WINAPI CreateWindowExW_(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style,
                                    int x, int y, int w, int h, HWND parent, HMENU menu,
                                    HINSTANCE inst, LPVOID param)
{
    HWND hw; WNDPROC proc; std::wstring cname;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const Cls* c = FindCls(cls);
        if (!c){ SetLastError(ERROR_CANNOT_FIND_WND_CLASS); return nullptr; }
        hw = (HWND)g_nextH; g_nextH += 2;
        proc = c->proc; cname = c->name;
        Wnd wd; wd.proc = proc; wd.tid = GetCurrentThreadId();
        wd.parent = parent == HWND_MESSAGE ? nullptr : parent;
        wd.style = style; wd.ex = ex; wd.id = (LONG_PTR)menu;
        wd.extra.assign((size_t)(c->extra > 0 ? c->extra : 0), 0);
        if (x == CW_USEDEFAULT) x = 0;
        if (y == CW_USEDEFAULT) y = 0;
        if (w == CW_USEDEFAULT) w = 0;
        if (h == CW_USEDEFAULT) h = 0;
        wd.rc = RECT{ x, y, x + w, y + h };
        wd.visible = (style & WS_VISIBLE) != 0;
        wd.cls = cname;
        g_wnd[hw] = std::move(wd);
    }
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 32)
        Diag("CEFWIN create " + std::string(cname.begin(), cname.end()) + " -> " + Hex((uintptr_t)hw) +
             " style=" + Hex(style) + (parent == HWND_MESSAGE ? " message-only" : "") +
             " tid=" + std::to_string(GetCurrentThreadId()));
    CREATESTRUCTW cs{ param, inst, menu, parent, h, w, y, x, (LONG)style, title, cls, ex };
    if (!proc || !proc(hw, WM_NCCREATE, 0, (LPARAM)&cs)){
        std::lock_guard<std::mutex> lk(g_mx); g_wnd.erase(hw);
        Diag("CEFWIN WM_NCCREATE refused " + Hex((uintptr_t)hw));
        return nullptr;
    }
    if (proc(hw, WM_CREATE, 0, (LPARAM)&cs) == -1){
        CallProc(hw, WM_NCDESTROY, 0, 0);
        std::lock_guard<std::mutex> lk(g_mx); g_wnd.erase(hw);
        return nullptr;
    }
    return hw;
}
static BOOL WINAPI DestroyWindow_(HWND h)
{
    if (!Ours(h)) return Bed_DestroyWindow(h);
    CallProc(h, WM_DESTROY, 0, 0);
    CallProc(h, WM_NCDESTROY, 0, 0);
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it != g_wnd.end()){
        auto qi = g_q.find(it->second.tid);
        if (qi != g_q.end()){
            auto& tm = qi->second.timers;
            tm.erase(std::remove_if(tm.begin(), tm.end(), [h](const Timer& t){ return t.h == h; }), tm.end());
            auto& q = qi->second.q;
            q.erase(std::remove_if(q.begin(), q.end(), [h](const MSG& m){ return m.hwnd == h; }), q.end());
        }
        g_wnd.erase(it);
    }
    return TRUE;
}
static LRESULT WINAPI DefWindowProcW_(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!Ours(h)) return Bed_DefWindowProcW(h, msg, wp, lp);
    switch (msg){
    case WM_NCCREATE: return TRUE;
    case WM_CLOSE:    DestroyWindow_(h); return 0;
    }
    return 0;
}
static LRESULT WINAPI CallWindowProcW_(WNDPROC p, HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return p ? p(h, msg, wp, lp) : 0;
}
static LONG_PTR GetLong(HWND h, int i)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end()){ SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    Wnd& w = it->second;
    switch (i){
    case GWLP_WNDPROC:    return (LONG_PTR)w.proc;
    case GWLP_USERDATA:   return w.user;
    case GWLP_ID:         return w.id;
    case GWL_STYLE:       return w.style;
    case GWL_EXSTYLE:     return w.ex;
    case GWLP_HWNDPARENT: return (LONG_PTR)w.parent;
    case GWLP_HINSTANCE:  return 0;
    }
    if (i >= 0 && (size_t)i + sizeof(LONG_PTR) <= w.extra.size()){
        LONG_PTR v; memcpy(&v, w.extra.data() + i, sizeof(v)); return v;
    }
    if (i >= 0 && (size_t)i + sizeof(LONG) <= w.extra.size()){
        LONG v; memcpy(&v, w.extra.data() + i, sizeof(v)); return v;
    }
    SetLastError(ERROR_INVALID_INDEX); return 0;
}
static LONG_PTR SetLong(HWND h, int i, LONG_PTR v, size_t width)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end()){ SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    Wnd& w = it->second; LONG_PTR old = 0;
    switch (i){
    case GWLP_WNDPROC:    old = (LONG_PTR)w.proc; w.proc = (WNDPROC)v; return old;
    case GWLP_USERDATA:   old = w.user;   w.user = v;   return old;
    case GWLP_ID:         old = w.id;     w.id = v;     return old;
    case GWL_STYLE:       old = w.style;  w.style = (DWORD)v; return old;
    case GWL_EXSTYLE:     old = w.ex;     w.ex = (DWORD)v;    return old;
    case GWLP_HWNDPARENT: old = (LONG_PTR)w.parent; w.parent = (HWND)v; return old;
    }
    if (i >= 0 && (size_t)i + width <= w.extra.size()){
        memcpy(&old, w.extra.data() + i, width);
        memcpy(w.extra.data() + i, &v, width);
        return old;
    }
    SetLastError(ERROR_INVALID_INDEX); return 0;
}
static LONG_PTR WINAPI GetWindowLongPtrW_(HWND h, int i){ return Ours(h) ? GetLong(h, i) : Bed_GetWindowLongPtrW(h, i); }
static LONG_PTR WINAPI SetWindowLongPtrW_(HWND h, int i, LONG_PTR v){ return Ours(h) ? SetLong(h, i, v, sizeof(LONG_PTR)) : Bed_SetWindowLongPtrW(h, i, v); }
static LONG WINAPI GetWindowLongW_(HWND h, int i){ return Ours(h) ? (LONG)GetLong(h, i) : (LONG)Bed_GetWindowLongPtrW(h, i); }
static LONG WINAPI SetWindowLongW_(HWND h, int i, LONG v){ return Ours(h) ? (LONG)SetLong(h, i, v, sizeof(LONG)) : (LONG)Bed_SetWindowLongPtrW(h, i, v); }
static BOOL WINAPI IsWindow_(HWND h){ return Ours(h) || Bed_IsWindow(h); }
static DWORD WINAPI GetWindowThreadProcessId_(HWND h, LPDWORD pid)
{
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_wnd.find(h);
        if (it != g_wnd.end()){ if (pid) *pid = GetCurrentProcessId(); return it->second.tid; }
    }
    return Bed_GetWindowThreadProcessId(h, pid);
}
static HWND WINAPI GetParent_(HWND h)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    return it != g_wnd.end() ? it->second.parent : nullptr;
}
static HWND WINAPI GetAncestor_(HWND h, UINT how)
{
    if (!Ours(h)) return Bed_GetAncestor(h, how);
    HWND p = GetParent_(h);
    if (how == GA_PARENT) return p;
    while (p && Ours(p)){ h = p; p = GetParent_(h); }
    return h;
}
static BOOL WINAPI IsWindowVisible_(HWND h)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    return it != g_wnd.end() && it->second.visible;
}
static BOOL WINAPI ShowWindow_(HWND h, int cmd)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end()) return FALSE;
    const BOOL was = it->second.visible;
    it->second.visible = cmd != SW_HIDE;
    return was;
}
static BOOL WINAPI SetWindowPos_(HWND h, HWND, int x, int y, int w, int hh, UINT fl)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end()) return TRUE;
    RECT& r = it->second.rc;
    if (!(fl & SWP_NOMOVE)){ const LONG cw = r.right - r.left, ch = r.bottom - r.top; r = RECT{ x, y, x + cw, y + ch }; }
    if (!(fl & SWP_NOSIZE)){ r.right = r.left + w; r.bottom = r.top + hh; }
    if (fl & SWP_SHOWWINDOW) it->second.visible = true;
    if (fl & SWP_HIDEWINDOW) it->second.visible = false;
    return TRUE;
}
static BOOL WINAPI MoveWindow_(HWND h, int x, int y, int w, int hh, BOOL){ return SetWindowPos_(h, nullptr, x, y, w, hh, SWP_NOZORDER); }
static BOOL WINAPI GetWindowRect_(HWND h, LPRECT r)
{
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_wnd.find(h);
        if (it != g_wnd.end()){ if (r) *r = it->second.rc; return TRUE; }
    }
    return Bed_GetWindowRect(h, r);
}
static BOOL WINAPI GetClientRect_(HWND h, LPRECT r)
{
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_wnd.find(h);
        if (it != g_wnd.end()){
            if (r){ *r = RECT{ 0, 0, it->second.rc.right - it->second.rc.left, it->second.rc.bottom - it->second.rc.top }; }
            return TRUE;
        }
    }
    return Bed_GetClientRect(h, r);
}
static int WINAPI GetClassNameW_(HWND h, LPWSTR buf, int n)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end() || !buf || n <= 0){ SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    const int len = (int)std::min<size_t>(it->second.cls.size(), (size_t)n - 1);
    wmemcpy(buf, it->second.cls.c_str(), len); buf[len] = 0;
    return len;
}
static HWND WINAPI GetWindow_(HWND h, UINT cmd)
{
    if (!Ours(h)) return Bed_GetWindow(h, cmd);
    return cmd == GW_OWNER ? GetParent_(h) : nullptr;
}
static BOOL WINAPI IsChild_(HWND parent, HWND h)
{
    for (HWND p = GetParent_(h); p; p = GetParent_(p)) if (p == parent) return TRUE;
    return FALSE;
}
static HWND WINAPI SetParent_(HWND h, HWND np)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end()) return nullptr;
    HWND old = it->second.parent;
    it->second.parent = np == HWND_MESSAGE ? nullptr : np;
    return old;
}
static std::wstring PropKey(LPCWSTR k){ return (uintptr_t)k <= 0xFFFF ? L"#" + std::to_wstring((uintptr_t)k) : std::wstring(k); }
static BOOL WINAPI SetPropW_(HWND h, LPCWSTR k, HANDLE v)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end() || !k) return FALSE;
    it->second.props[PropKey(k)] = v; return TRUE;
}
static HANDLE WINAPI GetPropW_(HWND h, LPCWSTR k)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end() || !k) return nullptr;
    auto p = it->second.props.find(PropKey(k));
    return p != it->second.props.end() ? p->second : nullptr;
}
static HANDLE WINAPI RemovePropW_(HWND h, LPCWSTR k)
{
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wnd.find(h);
    if (it == g_wnd.end() || !k) return nullptr;
    auto p = it->second.props.find(PropKey(k));
    if (p == it->second.props.end()) return nullptr;
    HANDLE v = p->second; it->second.props.erase(p); return v;
}

// ---- messages --------------------------------------------------------------
static BOOL WINAPI PostThreadMessageW_(DWORD tid, UINT msg, WPARAM wp, LPARAM lp){ Push(tid, nullptr, msg, wp, lp); return TRUE; }
static BOOL WINAPI PostMessageW_(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!h){ Push(GetCurrentThreadId(), nullptr, msg, wp, lp); return TRUE; }
    DWORD tid = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_wnd.find(h);
        if (it == g_wnd.end()){ SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
        tid = it->second.tid;
    }
    Push(tid, h, msg, wp, lp);
    return TRUE;
}
// Cross-thread sends run on the caller. Chromium sends only to its own windows
// from their own threads; a real cross-thread send would block for the owner.
static LRESULT WINAPI SendMessageW_(HWND h, UINT msg, WPARAM wp, LPARAM lp){ return Ours(h) ? CallProc(h, msg, wp, lp) : 0; }
static LRESULT WINAPI SendMessageTimeoutW_(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT, UINT, PDWORD_PTR res)
{
    if (!Ours(h)){ SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    LRESULT r = CallProc(h, msg, wp, lp);
    if (res) *res = (DWORD_PTR)r;
    return 1;
}
static BOOL WINAPI SendNotifyMessageW_(HWND h, UINT msg, WPARAM wp, LPARAM lp){ return PostMessageW_(h, msg, wp, lp); }
static bool Match(const MSG& m, HWND h, UINT mn, UINT mx)
{
    if (h == (HWND)-1){ if (m.hwnd) return false; }
    else if (h && m.hwnd != h) return false;
    if ((mn || mx) && (m.message < mn || m.message > mx)) return false;
    return true;
}
static BOOL WINAPI PeekMessageW_(LPMSG out, HWND h, UINT mn, UINT mx, UINT fl)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Queue& q = QueueFor(GetCurrentThreadId());
    FireTimers(q);
    for (auto it = q.q.begin(); it != q.q.end(); ++it){
        if (!Match(*it, h, mn, mx)) continue;
        if (out) *out = *it;
        if (fl & PM_REMOVE) q.q.erase(it);
        if (q.q.empty()) ResetEvent(q.ev);
        return TRUE;
    }
    if (q.q.empty()) ResetEvent(q.ev);
    return FALSE;
}
static DWORD NextTimerMs()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return FireTimers(QueueFor(GetCurrentThreadId()));
}
static HANDLE QueueEvent()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return QueueFor(GetCurrentThreadId()).ev;
}
static BOOL WINAPI GetMessageW_(LPMSG out, HWND h, UINT mn, UINT mx)
{
    for (;;){
        if (PeekMessageW_(out, h, mn, mx, PM_REMOVE)) return out->message != WM_QUIT;
        WaitForSingleObject(QueueEvent(), NextTimerMs());
    }
}
static BOOL WINAPI WaitMessage_()
{
    WaitForSingleObject(QueueEvent(), NextTimerMs());
    return TRUE;
}
static BOOL WINAPI TranslateMessage_(const MSG*){ return FALSE; }
static LRESULT WINAPI DispatchMessageW_(const MSG* m)
{
    if (!m) return 0;
    if (m->message == WM_TIMER && m->lParam){
        ((TIMERPROC)m->lParam)(m->hwnd, WM_TIMER, m->wParam, GetTickCount());
        return 0;
    }
    if (Ours(m->hwnd)) return CallProc(m->hwnd, m->message, m->wParam, m->lParam);
    if (!m->hwnd) return 0;
    return Bed_DispatchMessageW(m);
}
static DWORD WINAPI MsgWaitForMultipleObjectsEx_(DWORD n, const HANDLE* hs, DWORD ms, DWORD, DWORD fl)
{
    if (n >= MAXIMUM_WAIT_OBJECTS){ SetLastError(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    HANDLE all[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < n; ++i) all[i] = hs[i];
    all[n] = QueueEvent();
    const ULONGLONG start = GetTickCount64();
    for (;;){
        const DWORD timer = NextTimerMs();
        if (WaitForSingleObject(all[n], 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + n;
        DWORD left = ms;
        if (ms != INFINITE){
            const ULONGLONG gone = GetTickCount64() - start;
            left = gone >= ms ? 0 : (DWORD)(ms - gone);
        }
        const DWORD wait = timer < left ? timer : left;
        const DWORD r = WaitForMultipleObjectsEx(n + 1, all, FALSE, wait,
                                                 (fl & MWMO_ALERTABLE) ? TRUE : FALSE);
        if (r != WAIT_TIMEOUT) return r;
        if (wait == left) return WAIT_TIMEOUT;     // the caller's time ran out, not a timer's
    }
}
static DWORD WINAPI MsgWaitForMultipleObjects_(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, DWORD mask)
{
    return MsgWaitForMultipleObjectsEx_(n, hs, ms, mask, all ? MWMO_WAITALL : 0);
}
static DWORD WINAPI GetQueueStatus_(UINT flags)
{
    std::lock_guard<std::mutex> lk(g_mx);
    Queue& q = QueueFor(GetCurrentThreadId());
    FireTimers(q);
    if (q.q.empty()) return 0;
    const UINT s = (QS_POSTMESSAGE | QS_TIMER) & flags;
    return MAKELONG(s, s);
}
static UINT_PTR WINAPI SetTimer_(HWND h, UINT_PTR id, UINT ms, TIMERPROC fn)
{
    std::lock_guard<std::mutex> lk(g_mx);
    DWORD tid = GetCurrentThreadId();
    if (h){
        auto it = g_wnd.find(h);
        if (it == g_wnd.end()){ SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
        tid = it->second.tid;
    } else id = g_nextTimer++;
    if (ms < USER_TIMER_MINIMUM) ms = USER_TIMER_MINIMUM;
    Queue& q = QueueFor(tid);
    for (auto& t : q.timers) if (t.h == h && t.id == id){ t.ms = ms; t.fn = fn; t.due = GetTickCount64() + ms; SetEvent(q.ev); return h ? 1 : id; }
    q.timers.push_back(Timer{ h, id, ms, GetTickCount64() + ms, fn });
    SetEvent(q.ev);                    // wake the owner so its wait picks up the new deadline
    return h ? 1 : id;
}
static BOOL WINAPI KillTimer_(HWND h, UINT_PTR id)
{
    std::lock_guard<std::mutex> lk(g_mx);
    DWORD tid = GetCurrentThreadId();
    if (h){
        auto it = g_wnd.find(h);
        if (it == g_wnd.end()) return FALSE;
        tid = it->second.tid;
    }
    Queue& q = QueueFor(tid);
    auto& tm = q.timers;
    const size_t before = tm.size();
    tm.erase(std::remove_if(tm.begin(), tm.end(), [&](const Timer& t){ return t.h == h && t.id == id; }), tm.end());
    q.q.erase(std::remove_if(q.q.begin(), q.q.end(), [&](const MSG& m){ return m.message == WM_TIMER && m.hwnd == h && m.wParam == id; }), q.q.end());
    return tm.size() != before;
}
static void WINAPI PostQuitMessage_(int code){ Push(GetCurrentThreadId(), nullptr, WM_QUIT, (WPARAM)code, 0); }
static UINT WINAPI RegisterWindowMessageW_(LPCWSTR name)
{
    if (!name) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_wndMsgs.find(name);
    if (it != g_wndMsgs.end()) return it->second;
    const UINT v = 0xC000 + (UINT)g_wndMsgs.size();
    g_wndMsgs[name] = v;
    return v;
}

static const std::unordered_map<std::string, void*>& Table()
{
    static const std::unordered_map<std::string, void*> t = {
        {"RegisterClassExW", (void*)&RegisterClassExW_}, {"RegisterClassW", (void*)&RegisterClassW_},
        {"UnregisterClassW", (void*)&UnregisterClassW_}, {"GetClassInfoExW", (void*)&GetClassInfoExW_},
        {"CreateWindowExW", (void*)&CreateWindowExW_}, {"DestroyWindow", (void*)&DestroyWindow_},
        {"DefWindowProcW", (void*)&DefWindowProcW_}, {"CallWindowProcW", (void*)&CallWindowProcW_},
        {"GetWindowLongPtrW", (void*)&GetWindowLongPtrW_}, {"SetWindowLongPtrW", (void*)&SetWindowLongPtrW_},
        {"GetWindowLongW", (void*)&GetWindowLongW_}, {"SetWindowLongW", (void*)&SetWindowLongW_},
        {"IsWindow", (void*)&IsWindow_}, {"GetWindowThreadProcessId", (void*)&GetWindowThreadProcessId_},
        {"GetParent", (void*)&GetParent_}, {"GetAncestor", (void*)&GetAncestor_},
        {"IsWindowVisible", (void*)&IsWindowVisible_}, {"ShowWindow", (void*)&ShowWindow_},
        {"SetWindowPos", (void*)&SetWindowPos_}, {"MoveWindow", (void*)&MoveWindow_},
        {"GetWindowRect", (void*)&GetWindowRect_}, {"GetClientRect", (void*)&GetClientRect_},
        {"GetClassNameW", (void*)&GetClassNameW_}, {"GetWindow", (void*)&GetWindow_},
        {"IsChild", (void*)&IsChild_}, {"SetParent", (void*)&SetParent_},
        {"SetPropW", (void*)&SetPropW_}, {"GetPropW", (void*)&GetPropW_}, {"RemovePropW", (void*)&RemovePropW_},
        {"PostMessageW", (void*)&PostMessageW_}, {"PostThreadMessageW", (void*)&PostThreadMessageW_},
        {"SendMessageW", (void*)&SendMessageW_}, {"SendMessageTimeoutW", (void*)&SendMessageTimeoutW_},
        {"SendNotifyMessageW", (void*)&SendNotifyMessageW_},
        {"PeekMessageW", (void*)&PeekMessageW_}, {"GetMessageW", (void*)&GetMessageW_},
        {"WaitMessage", (void*)&WaitMessage_}, {"TranslateMessage", (void*)&TranslateMessage_},
        {"DispatchMessageW", (void*)&DispatchMessageW_},
        {"MsgWaitForMultipleObjectsEx", (void*)&MsgWaitForMultipleObjectsEx_},
        {"MsgWaitForMultipleObjects", (void*)&MsgWaitForMultipleObjects_},
        {"GetQueueStatus", (void*)&GetQueueStatus_},
        {"SetTimer", (void*)&SetTimer_}, {"KillTimer", (void*)&KillTimer_},
        {"PostQuitMessage", (void*)&PostQuitMessage_}, {"RegisterWindowMessageW", (void*)&RegisterWindowMessageW_},
    };
    return t;
}
static void* Lookup(const char* fn)
{
    auto& t = Table();
    auto it = t.find(fn);
    return it != t.end() ? it->second : nullptr;
}

} // namespace cefwin
