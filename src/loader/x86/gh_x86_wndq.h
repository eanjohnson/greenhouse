// gh_x86_wndq.h -- the messages Windows itself posts to a guest window: WM_TIMER
// for SetTimer, and WM_DEVICECHANGE for RegisterDeviceNotification.
//
// The guest's queue is the shared input queue plus the activation sequence
// (peek_message32), so nothing the system raises on its own ever arrived. That
// is how HL2 lost its pad: SDL2 finds XInput pads once, at SDL_Init, and again
// only after its notify window sees DBT_DEVICEARRIVAL and then the WM_TIMER it
// sets in reply. The console pad (or a synthetic press) shows up after SDL_Init,
// and RegisterDeviceNotification failed outright on our surrogate HWND.
//
// Both kinds are answered here, on the thread that asked for them (a window's
// messages belong to the thread that owns it), at the lowest priority -- after
// input, as in Windows:
//   * a window timer fires every `elapse` ms until KillTimer; a thread timer
//     (hwnd 0) still never fires, since no window procedure would see it;
//   * each registration gets one WM_DEVICECHANGE per pad connect/disconnect,
//     DBT_DEVICEARRIVAL / DBT_DEVICEREMOVECOMPLETE with a HID device-interface
//     broadcast. The pad behind it is the XInput beds' pad.

#pragma once

#include "gh_x86_win.h"

namespace ghx86 {

struct WndQ {
    struct Timer  { uint32_t hwnd, id, elapse, proc; DWORD tid; ULONGLONG due; };
    struct Notify { uint32_t handle, hwnd; DWORD tid; uint32_t epoch; };
    std::mutex mu;
    std::vector<Timer>  timers;
    std::vector<Notify> notes;
    uint32_t next_handle = 0x7E000001;
    uint32_t broadcast = 0;      // guest DEV_BROADCAST_DEVICEINTERFACE_W, made once
};
inline WndQ& wndq() { static WndQ q; return q; }

// Bumped by the host each time the pad the XInput beds serve comes or goes.
inline uint32_t wndq_pad_epoch(bool& up)
{
#ifdef GH_LAUNCHER
    { std::lock_guard<std::mutex> lk(g_padMutex); up = g_padConnected || g_synSticky; }
    return g_padEpoch.load();
#else
    up = false;
    return 0;
#endif
}

inline uint32_t wndq_set_timer(uint32_t hwnd, uint32_t id, uint32_t ms, uint32_t proc)
{
    WndQ& q = wndq();
    const ULONGLONG now = GetTickCount64();
    ms = ms < 10 ? 10 : ms;                                          // USER_TIMER_MINIMUM
    std::lock_guard<std::mutex> lk(q.mu);
    for (auto& t : q.timers)
        if (t.hwnd == hwnd && t.id == id) { t.elapse = ms; t.proc = proc; t.due = now + ms; return id ? id : 1; }
    q.timers.push_back({ hwnd, id, ms, proc, GetCurrentThreadId(), now + ms });
    static int n = 0;
    if (++n <= 8) log("X86 SetTimer hwnd=" + hex(hwnd) + " id=" + hex(id) + " " + std::to_string(ms) + " ms");
    return id ? id : 1;
}

inline void wndq_kill_timer(uint32_t hwnd, uint32_t id)
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    std::erase_if(q.timers, [&](const WndQ::Timer& t) { return t.hwnd == hwnd && t.id == id; });
}

inline uint32_t wndq_register_notify(uint32_t hwnd)
{
    WndQ& q = wndq();
    bool up;
    const uint32_t epoch = wndq_pad_epoch(up);
    std::lock_guard<std::mutex> lk(q.mu);
    const uint32_t h = q.next_handle++;
    q.notes.push_back({ h, hwnd, GetCurrentThreadId(), epoch });
    log("X86 RegisterDeviceNotification hwnd=" + hex(hwnd) + " -> " + hex(h));
    return h;
}

inline bool wndq_unregister_notify(uint32_t h)
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    return std::erase_if(q.notes, [&](const WndQ::Notify& n) { return n.handle == h; }) != 0;
}

inline uint32_t wndq_broadcast(Machine& m)
{
    static const wchar_t kName[] =
        L"\\\\?\\HID#VID_045E&PID_02FF&IG_00#7&1&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    WndQ& q = wndq();
    if (q.broadcast) return q.broadcast;
    const uint32_t bytes = 28 + (uint32_t)sizeof kName;
    const uint32_t p = m.heap_alloc(bytes, true);
    if (!p) return 0;
    uint8_t* b = (uint8_t*)(uintptr_t)p;
    const uint32_t hdr[3] = { bytes, 5 /*DBT_DEVTYP_DEVICEINTERFACE*/, 0 };
    const GUID hid = { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88,0xCB,0x00,0x11,0x11,0x00,0x00,0x30 } };
    std::memcpy(b, hdr, 12);
    std::memcpy(b + 12, &hid, 16);
    std::memcpy(b + 28, kName, sizeof kName);
    return q.broadcast = p;
}

// The next system message for this thread that the PeekMessage filter admits,
// as {hwnd, message, wParam, lParam}. Consumed only when `remove`.
inline bool wndq_next(Machine& m, uint32_t hwnd_filter, uint32_t mn, uint32_t mx, bool remove, uint32_t out[4])
{
    WndQ& q = wndq();
    auto admits = [&](UINT msg) { return (!mn && !mx) || (msg >= mn && msg <= mx); };
    auto for_us = [&](uint32_t hwnd, DWORD tid) {
        return tid == GetCurrentThreadId() && (!hwnd_filter || hwnd_filter == hwnd);
    };
    if (admits(WM_DEVICECHANGE)) {
        bool up;
        const uint32_t epoch = wndq_pad_epoch(up);
        uint32_t bc = 0;
        { std::lock_guard<std::mutex> lk(q.mu); bc = q.broadcast; }
        if (!bc) bc = wndq_broadcast(m);
        std::lock_guard<std::mutex> lk(q.mu);
        for (auto& n : q.notes)
            if (n.epoch != epoch && for_us(n.hwnd, n.tid)) {
                out[0] = n.hwnd; out[1] = WM_DEVICECHANGE;
                out[2] = up ? 0x8000u /*DBT_DEVICEARRIVAL*/ : 0x8004u /*DBT_DEVICEREMOVECOMPLETE*/;
                out[3] = bc;
                if (remove) {
                    n.epoch = epoch;
                    log(std::string("X86 WM_DEVICECHANGE ") + (up ? "arrival" : "removal") + " -> " + hex(n.hwnd));
                }
                return true;
            }
    }
    if (admits(WM_TIMER)) {
        const ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lk(q.mu);
        for (auto& t : q.timers)
            if (t.due <= now && for_us(t.hwnd, t.tid)) {
                out[0] = t.hwnd; out[1] = WM_TIMER; out[2] = t.id; out[3] = t.proc;
                if (remove) t.due = now + t.elapse;
                return true;
            }
    }
    return false;
}

}   // namespace ghx86
