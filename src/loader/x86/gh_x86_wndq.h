// gh_x86_wndq.h -- the messages Windows itself posts to a guest window: WM_TIMER
// for SetTimer, WM_PAINT for InvalidateRect, and WM_DEVICECHANGE for
// RegisterDeviceNotification.
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

#include <deque>
#include <unordered_map>
#include <map>
#include <string>
#include <tuple>

namespace ghx86 {

struct WndQ {
    struct Timer  { uint32_t hwnd, id, elapse, proc; DWORD tid; ULONGLONG due; };
    struct Notify { uint32_t handle, hwnd; DWORD tid; uint32_t epoch; };
    std::mutex mu;
    std::vector<Timer>  timers;
    std::vector<Notify> notes;
    // PostMessage / PostThreadMessage / PostQuitMessage. Our windows are not
    // real HWNDs, so a forward handed them to user32, which refused them and
    // the message was gone: a player whose main loop runs on messages it
    // posts to itself (Adobe AIR) never saw one. They wait here for the
    // thread that owns the window, in order, ahead of timers.
    struct Posted { uint32_t hwnd, msg, wp, lp; DWORD tid; };
    std::deque<Posted> posted;
    std::unordered_map<uint32_t, DWORD> owner;   // hwnd -> thread that created it
    // The update region, reduced to "the whole window or nothing": hwnd -> the
    // generation of its latest InvalidateRect. A window with one is owed a
    // WM_PAINT until something validates it (BeginPaint, ValidateRect,
    // DefWindowProc). Adobe AIR renders only from WM_PAINT: its frame tick
    // invalidates the stage and draws when the paint arrives, so with
    // InvalidateRect a no-op the stage froze after its first frame.
    std::unordered_map<uint32_t, uint32_t> dirty;
    uint32_t paint_gen = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // something was posted
    uint32_t next_handle = 0x7E000001;
    uint32_t broadcast = 0;      // guest DEV_BROADCAST_DEVICEINTERFACE_W, made once
};
inline WndQ& wndq() { static WndQ q; return q; }

// What the guest's message loops actually did, for the periodic sample line
// (gh_x86_sample.txt): a player whose frames stop is either not dispatching
// its tick, not being painted, or painting without presenting.
struct WndQStats {
    std::atomic<uint32_t> tick{0}, paint{0}, timer{0}, other{0}, inval{0}, steam{0};
};
inline WndQStats& wndq_stats() { static WndQStats s; return s; }

inline void wndq_note_window(uint32_t hwnd)
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    q.owner[hwnd] = GetCurrentThreadId();
}

// The thread that created `hwnd`, 0 when it is not one of ours.
inline DWORD wndq_owner(uint32_t hwnd)
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    auto it = q.owner.find(hwnd);
    return it == q.owner.end() ? 0 : it->second;
}

// Queues a message for the thread that owns `hwnd` (`tid` for a thread
// message, hwnd 0). False for a window nobody created here.
inline bool wndq_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, DWORD tid = 0)
{
    WndQ& q = wndq();
    {
        std::lock_guard<std::mutex> lk(q.mu);
        if (hwnd) {
            auto it = q.owner.find(hwnd);
            if (it == q.owner.end()) return false;
            tid = it->second;
        } else if (!tid) {
            tid = GetCurrentThreadId();
        }
        if (q.posted.size() >= 10000) return false;   // Windows' per-queue limit
        q.posted.push_back({ hwnd, msg, wp, lp, tid });
    }
    SetEvent(q.wake);
    return true;
}

// For the sampler: what is waiting in the posted queue, by window, message
// and the thread it waits for, plus which thread owns each window. AIR's tick
// thread stops for good while a 0x401 it posted is still queued, so a stall
// shows up here as a message nobody reads.
inline std::string wndq_describe()
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    std::map<std::tuple<uint32_t, uint32_t, DWORD>, uint32_t> n;
    for (const auto& p : q.posted) ++n[{ p.hwnd, p.msg, p.tid }];
    std::string s = "posted=" + std::to_string(q.posted.size());
    int k = 0;
    for (const auto& [key, c] : n) {
        if (++k > 12) { s += " ..."; break; }
        s += " " + hex(std::get<0>(key)) + "/" + hex(std::get<1>(key)) + "@" + std::to_string(std::get<2>(key)) + "x" + std::to_string(c);
    }
    s += " owners:";
    for (const auto& [h, t] : q.owner) s += " " + hex(h) + "@" + std::to_string(t);
    return s;
}

// InvalidateRect / RedrawWindow(RDW_INVALIDATE). hwnd 0 means every window,
// as in user32. False for a window nobody created here.
inline bool wndq_invalidate(uint32_t hwnd)
{
    ++wndq_stats().inval;
    WndQ& q = wndq();
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(q.mu);
        ++q.paint_gen;
        for (auto& [h, tid] : q.owner)
            if (!hwnd || h == hwnd) { q.dirty[h] = q.paint_gen; any = true; }
    }
    if (any) SetEvent(q.wake);
    return any;
}

inline void wndq_validate(uint32_t hwnd)
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    if (hwnd) q.dirty.erase(hwnd); else q.dirty.clear();
}

// The generation of the window's pending paint, 0 when it has none.
inline uint32_t wndq_paint_gen(uint32_t hwnd)
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    auto it = q.dirty.find(hwnd);
    return it == q.dirty.end() ? 0 : it->second;
}

// After the window procedure has had its WM_PAINT. A procedure that neither
// called BeginPaint nor passed the message on would get WM_PAINT again for
// ever, ahead of its timers; Windows does that too, but a guest written
// against a real window tree may validate through a call we do not see.
// So the paint it was given counts as done -- unless it invalidated again
// while painting, which is how a player asks for its next frame.
inline void wndq_painted(uint32_t hwnd, uint32_t gen_before)
{
    WndQ& q = wndq();
    std::lock_guard<std::mutex> lk(q.mu);
    auto it = q.dirty.find(hwnd);
    if (it != q.dirty.end() && it->second == gen_before) q.dirty.erase(it);
}

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
    {
        std::lock_guard<std::mutex> lk(q.mu);
        for (auto it = q.posted.begin(); it != q.posted.end(); ++it)
            if (it->tid == GetCurrentThreadId() && (it->msg == WM_QUIT || admits(it->msg)) &&
                (!hwnd_filter || hwnd_filter == it->hwnd || (hwnd_filter == 0xFFFFFFFFu && !it->hwnd))) {
                out[0] = it->hwnd; out[1] = it->msg; out[2] = it->wp; out[3] = it->lp;
                if (remove) q.posted.erase(it);
                return true;
            }
    }
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
    // WM_PAINT ranks below posted messages and input and above WM_TIMER, and
    // is not removed by reading it: it stays until the window is validated.
    if (admits(WM_PAINT) && hwnd_filter != 0xFFFFFFFFu) {
        std::lock_guard<std::mutex> lk(q.mu);
        for (auto& [h, gen] : q.dirty) {
            auto o = q.owner.find(h);
            if (o != q.owner.end() && for_us(h, o->second)) {
                out[0] = h; out[1] = WM_PAINT; out[2] = 0; out[3] = 0;
                return true;
            }
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
