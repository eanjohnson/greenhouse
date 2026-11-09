// gh_art.h -- Steam key art: fetch it once, keep it inside a budget, hand the
// UI a D2D bitmap.
//
// THE RULE THIS FILE EXISTS TO ENFORCE
// ------------------------------------
// Game art lives in the launcher only, and only art downloaded through Steam:
// Steam's own store CDN, by app id, for a title the signed-in account owns.
// Never a substitute service, never a scrape, never a picture that ships with
// the app, the corpus, a theme or a package. If Steam has no asset for a title,
// the title has no art and the typographic plate is drawn instead -- the
// fallback is never another picture.
//
// So the app id alone is not enough to ask for a fetch: Allow() is handed the
// set of app ids the library actually reported for the signed-in account, and
// anything else is answered "absent" without a request ever leaving the
// console. That keeps the rule in the one place it cannot be bypassed by a page
// that happens to know an app id.
//
// WHY A WORKER THREAD AND A THREE-STATE LADDER
// --------------------------------------------
// The expensive part of a wall of covers is not the GET, it is everything that
// must NOT happen on the UI thread: a TLS handshake, a 90 KB download and a
// JPEG decode, eighteen times, before the first frame. So:
//
//   UI thread            worker thread
//   ---------            -------------
//   Provide() ->  queue  ->  disk cache hit, else HTTPS GET + write to cache
//                            -> WIC decode, scaled down, to BGRA pixels
//   Provide() <- pixels  <-
//   CreateBitmap (cheap), free the pixels, draw
//
// A std::thread in a packaged app comes up MTA (see the UCRT/_beginthreadex
// note in the project log), which is what makes WIC usable off the UI thread at
// all. D2D bitmaps are NOT made here: a bitmap belongs to the device that made
// it, so the last hop stays on the thread that owns the render target, and it
// is rate-limited (kDecodePerFrame) so a freshly-scrolled wall costs a few
// milliseconds a frame instead of one long stall.
//
// THE BUDGET, AND WHY THE CACHE IS NOT IN LocalState
// --------------------------------------------------
// `U:` -- the volume behind every package's LocalState -- is 3.0 GB for the
// WHOLE console, and it has been measured with 4 MiB free. When it fills, the
// loader's diag and the game's own log both stop reaching disk while the
// process runs on, which is the worst failure mode we have: the instrument
// disappears before the measurement does. A wall of 171 capsules is tens of
// megabytes. So the cache belongs on `D:\DevelopmentFiles`, which has gigabytes
// and no quota, and the caller passes both the directory and the cap -- a
// LocalState fallback gets a cap small enough to be harmless rather than the
// full one.
//
// Beside the games' tree, never inside it: `D:\DevelopmentFiles\gh_games` is
// mapped into the guest's VFS, and this is ours, not the game's.
//
// In memory there is a second, separate cap: decoded bitmaps are video memory,
// and dropping the least recently drawn is what keeps a long browse flat.
#pragma once

#include <wincodec.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gh_repo.h"   // ghrepo::HttpGet / ReadFileAll / Widen -- the loader's own WinHTTP path
#include "gh_ui.h"     // GhCom, g_ghUi, GH_ART_COVER / GH_ART_HERO

namespace ghart {

// Steam's store CDN. One host and one path shape, measured rather than
// guessed: the `shared.cloudflare.steamstatic.com/store_item_assets/...` form
// answers 301 and this one answers 200.
constexpr const char* kHost = "https://cdn.cloudflare.steamstatic.com/steam/apps/";

// The two assets the Cabinet layout draws. `logo.png` is deliberately not here:
// the title screen sets the name in type over the hero, so a wordmark PNG would
// be a second download for a worse result on a television.
inline const char* AssetName(int kind)
{
    return kind == GH_ART_HERO ? "library_hero.jpg" : "library_600x900.jpg";
}

// What we decode to, NOT what Steam sends. A capsule is 600x900 and is drawn
// about 240 wide on the wall and 250 wide on the title screen; decoding it at
// 600 wide would cost 2.1 MB of video memory per title to throw five sixths of
// it away at draw time. The hero is 1920x620 and is drawn across the whole
// window, so it keeps most of its resolution.
constexpr int kCoverW = 360;
constexpr int kHeroW  = 1536;

inline int TargetW(int kind){ return kind == GH_ART_HERO ? kHeroW : kCoverW; }

// Caps. The disk figure is the caller's (see the budget note above); these are
// how many decoded bitmaps may be live at once -- a cover at 360x540 is 777 KB,
// a hero at 1536x496 is 3.0 MB.
constexpr uint64_t kDiskCapDev     = 48ull * 1024 * 1024;   // D:\DevelopmentFiles: no quota
constexpr uint64_t kDiskCapLocal   =  6ull * 1024 * 1024;   // LocalState: a shared 3.0 GB volume
constexpr int      kMaxCovers      = 44;
constexpr int      kMaxHeroes      = 3;
constexpr int      kDecodePerFrame = 2;      // D2D bitmaps created per frame
constexpr int      kQueueMax       = 48;
constexpr uint64_t kMaxFetchBytes  = 6ull * 1024 * 1024;   // a capsule is ~90 KB; a hero ~400 KB
// A title Steam has no asset for is remembered, so the console does not ask
// again every launch -- but not forever, because art does get added.
constexpr uint64_t kAbsentDays = 14;

enum State : uint8_t {
    kUnknown = 0,   // never asked for
    kQueued  = 1,   // the worker has it
    kPixels  = 2,   // decoded; waiting for a D2D bitmap on the UI thread
    kReady   = 3,   // `bmp` is drawable
    kAbsent  = 4    // Steam has no such asset, or the fetch failed for good
};

struct Item {
    uint32_t             app   = 0;
    int                  kind  = 0;
    uint8_t              state = kUnknown;
    GhCom<ID2D1Bitmap>   bmp;            // UI thread only
    std::vector<uint8_t> px;             // BGRA, worker -> UI thread
    uint32_t             w = 0, h = 0;
    uint64_t             used = 0;       // frame number of the last draw (LRU)
};

struct Ctx {
    std::wstring dir;                         // where the fetched JPEGs live
    uint64_t     diskCap = kDiskCapLocal;     // the smaller default: safe anywhere
    void (*log)(const std::string&) = nullptr;

    std::mutex              mu;               // guards items, queue, allowed
    std::condition_variable cv;
    std::vector<Item>       items;
    std::deque<std::pair<uint32_t, int>> queue;
    std::vector<uint32_t>   allowed;          // app ids the library reported
    bool                    allowAll = false; // only the preview harness sets this

    std::thread      worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    uint64_t         frame = 0;
    int              decodedThisFrame = 0;
    std::atomic<int> fetched{0}, missing{0}, failed{0};
    int              saidFetched = -1, saidMissing = -1, saidFailed = -1;  // worker thread only
};

inline Ctx g_art;

inline void Log(const std::string& s){ if (g_art.log) g_art.log(s); }

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
inline std::wstring CachePath(uint32_t app, int kind)
{
    return g_art.dir + std::to_wstring(app) + (kind == GH_ART_HERO ? L".hero.jpg" : L".cover.jpg");
}
// An empty marker beside the asset: "asked, Steam has nothing".
inline std::wstring AbsentPath(uint32_t app, int kind)
{
    return g_art.dir + std::to_wstring(app) + (kind == GH_ART_HERO ? L".hero.none" : L".cover.none");
}

inline bool FileAgeDays(const std::wstring& p, uint64_t* days)
{
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    ULARGE_INTEGER t{}, now{};
    t.LowPart = fa.ftLastWriteTime.dwLowDateTime; t.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    FILETIME nf{}; GetSystemTimeAsFileTime(&nf);
    now.LowPart = nf.dwLowDateTime; now.HighPart = nf.dwHighDateTime;
    const uint64_t d = (now.QuadPart > t.QuadPart) ? (now.QuadPart - t.QuadPart) : 0;
    if (days) *days = d / (10000000ull * 60 * 60 * 24);
    return true;
}

// ---------------------------------------------------------------------------
// The disk budget
// ---------------------------------------------------------------------------
// Oldest write time first, because the alternative -- a true LRU -- means
// touching a file every time a cover is drawn, and a wall draws eighteen of
// them sixty times a second. Write time is a good enough proxy on a cache
// nothing rewrites.
inline void PruneDisk()
{
    struct F { std::wstring name; uint64_t bytes; uint64_t when; };
    std::vector<F> f;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_art.dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER sz{}; sz.LowPart = fd.nFileSizeLow; sz.HighPart = fd.nFileSizeHigh;
        ULARGE_INTEGER w{};  w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        f.push_back({ fd.cFileName, sz.QuadPart, w.QuadPart });
        total += sz.QuadPart;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= g_art.diskCap) return;

    for (size_t i = 1; i < f.size(); ++i)          // insertion sort, oldest first
        for (size_t j = i; j > 0 && f[j - 1].when > f[j].when; --j) std::swap(f[j - 1], f[j]);

    int gone = 0;
    for (const auto& e : f) {
        if (total <= g_art.diskCap) break;
        if (DeleteFileW((g_art.dir + e.name).c_str())) { total -= e.bytes; ++gone; }
    }
    if (gone) Log("cache pruned: " + std::to_string(gone) + " file(s), now " +
                  std::to_string(total / 1024) + " KB");
}

// ---------------------------------------------------------------------------
// Decode (worker thread)
// ---------------------------------------------------------------------------
// JPEG bytes -> 32-bit BGRA, scaled to the width the UI actually draws at.
// Scaled in WIC rather than at draw time for the reason in the kCoverW note,
// and converted here rather than on the UI thread because this is the
// expensive half.
inline bool DecodeToPixels(const std::vector<uint8_t>& jpeg, int targetW,
                           std::vector<uint8_t>& out, uint32_t& ow, uint32_t& oh)
{
    out.clear(); ow = oh = 0;
    if (jpeg.size() < 64) return false;

    GhCom<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IWICImagingFactory), (void**)wic.put())) || !wic)
        return false;

    GhCom<IWICStream> st;
    if (FAILED(wic->CreateStream(st.put())) || !st) return false;
    if (FAILED(st->InitializeFromMemory((BYTE*)jpeg.data(), (DWORD)jpeg.size()))) return false;

    GhCom<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(st.get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                            dec.put())) || !dec)
        return false;
    GhCom<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put())) || !frame) return false;

    UINT sw = 0, sh = 0;
    if (FAILED(frame->GetSize(&sw, &sh)) || !sw || !sh) return false;

    // Scale first, convert second: scaling 8-bit YCbCr is cheaper than scaling
    // the 32-bit result, and WIC's scaler takes the decoder's own format.
    GhCom<IWICBitmapSource> src;
    src.p = (IWICBitmapSource*)frame.get(); src.p->AddRef();
    GhCom<IWICBitmapScaler> scaler;
    if ((int)sw > targetW && targetW > 0) {
        const UINT tw = (UINT)targetW;
        const UINT th = (UINT)((uint64_t)sh * tw / sw);
        if (th >= 1 && SUCCEEDED(wic->CreateBitmapScaler(scaler.put())) && scaler &&
            SUCCEEDED(scaler->Initialize(src.get(), tw, th, WICBitmapInterpolationModeFant))) {
            src.reset();
            src.p = (IWICBitmapSource*)scaler.get(); src.p->AddRef();
            sw = tw; sh = th;
        }
    }

    GhCom<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put())) || !conv) return false;
    if (FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.f,
                                WICBitmapPaletteTypeCustom)))
        return false;

    const size_t stride = (size_t)sw * 4;
    out.resize(stride * sh);
    if (FAILED(conv->CopyPixels(nullptr, (UINT)stride, (UINT)out.size(), out.data()))) {
        out.clear();
        return false;
    }
    ow = sw; oh = sh;
    return true;
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------
inline Item* Find(uint32_t app, int kind)        // caller holds the lock
{
    for (auto& it : g_art.items) if (it.app == app && it.kind == kind) return &it;
    return nullptr;
}

inline bool Allowed(uint32_t app)                // caller holds the lock
{
    if (g_art.allowAll) return true;
    for (uint32_t a : g_art.allowed) if (a == app) return true;
    return false;
}

inline void WorkerOne(uint32_t app, int kind)
{
    const std::wstring path = CachePath(app, kind);
    std::vector<uint8_t> bytes;
    bool fromDisk = ghrepo::ReadFileAll(path, bytes, kMaxFetchBytes) && bytes.size() > 64;

    if (!fromDisk) {
        // Remembered absence, while it is still fresh.
        uint64_t days = 0;
        if (FileAgeDays(AbsentPath(app, kind), &days) && days < kAbsentDays) {
            std::lock_guard<std::mutex> lk(g_art.mu);
            if (Item* it = Find(app, kind)) it->state = kAbsent;
            return;
        }
        const std::string url = std::string(kHost) + std::to_string(app) + "/" + AssetName(kind);
        std::string why;
        uint64_t got = 0;
        bytes.clear();
        const bool ok = ghrepo::HttpGet(url, kMaxFetchBytes, &bytes, nullptr, nullptr, &got, why);
        if (!ok || bytes.size() <= 64) {
            // "Steam has no such asset" and "the console could not reach Steam"
            // are NOT the same answer, and treating them the same was a bug: a
            // dropped Wi-Fi association wrote the absent marker and blanked a
            // title's art for fourteen days. Only the server's own 4xx is an
            // absence; anything else is this attempt failing, and the next
            // launch asks again.
            const bool absent = why.compare(0, 6, "HTTP 4") == 0;
            if (absent) {
                // A miss is normal (not every title has a capsule) and is not an
                // error worth a line per title: count them and say so once.
                g_art.missing.fetch_add(1);
                HANDLE h = CreateFileW(AbsentPath(app, kind).c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            } else {
                // A failure IS worth a line, but only the first one: the reason
                // is the same for all 171 titles when the network is the cause.
                if (g_art.failed.fetch_add(1) == 0)
                    Log("fetch failed (app " + std::to_string(app) + "): " + why);
            }
            std::lock_guard<std::mutex> lk(g_art.mu);
            if (Item* it = Find(app, kind)) it->state = kAbsent;
            return;
        }
        g_art.fetched.fetch_add(1);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
            CloseHandle(h);
            DeleteFileW(AbsentPath(app, kind).c_str());
            PruneDisk();
        }
    }

    std::vector<uint8_t> px; uint32_t w = 0, h = 0;
    if (!DecodeToPixels(bytes, TargetW(kind), px, w, h)) {
        g_art.failed.fetch_add(1);
        // A file that will not decode is worse than no file: delete it, so the
        // next launch fetches again rather than reading the same bad bytes.
        if (fromDisk) DeleteFileW(path.c_str());
        std::lock_guard<std::mutex> lk(g_art.mu);
        if (Item* it = Find(app, kind)) it->state = kAbsent;
        return;
    }

    std::lock_guard<std::mutex> lk(g_art.mu);
    if (Item* it = Find(app, kind)) {
        it->px = std::move(px);
        it->w = w; it->h = h;
        it->state = kPixels;
    }
}

inline void WorkerLoop()
{
    for (;;) {
        uint32_t app = 0; int kind = 0;
        {
            std::unique_lock<std::mutex> lk(g_art.mu);
            g_art.cv.wait(lk, []{ return g_art.stop.load() || !g_art.queue.empty(); });
            if (g_art.stop.load()) return;
            // LIFO: the newest request is the one the user is looking at. A
            // queue drained in order would spend its first second on the covers
            // that scrolled off the top.
            app = g_art.queue.back().first;
            kind = g_art.queue.back().second;
            g_art.queue.pop_back();
        }
        WorkerOne(app, kind);

        // Say once per drained queue what the wall is actually made of. Without
        // this a plate on the TV has no explanation in the diag at all: nothing
        // distinguished "Steam has no capsule for this title" from "the fetch
        // failed", and the counters existed but were never printed.
        bool drained = false;
        {
            std::lock_guard<std::mutex> lk(g_art.mu);
            drained = g_art.queue.empty();
        }
        if (drained) {
            const int f = g_art.fetched.load(), m = g_art.missing.load(),
                      x = g_art.failed.load();
            if (f != g_art.saidFetched || m != g_art.saidMissing || x != g_art.saidFailed) {
                g_art.saidFetched = f; g_art.saidMissing = m; g_art.saidFailed = x;
                Log("queue drained: " + std::to_string(f) + " fetched, " +
                    std::to_string(m) + " with no asset, " + std::to_string(x) + " failed");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The UI side
// ---------------------------------------------------------------------------
// Drop the least recently drawn bitmaps once there are too many live. Covers
// and heroes are counted separately: three heroes is already 9 MB, and letting
// them share one budget with the wall would evict the whole wall whenever the
// title screen was opened.
inline void EvictLocked(int kind, int cap)
{
    for (;;) {
        int live = 0;
        Item* oldest = nullptr;
        for (auto& it : g_art.items) {
            if (it.kind != kind || it.state != kReady || !it.bmp) continue;
            ++live;
            if (!oldest || it.used < oldest->used) oldest = &it;
        }
        if (live <= cap || !oldest) return;
        oldest->bmp.reset();
        oldest->state = kUnknown;      // ask again if it comes back on screen
    }
}

// Called once per frame by the page that draws art, before drawing it.
inline void BeginFrame()
{
    std::lock_guard<std::mutex> lk(g_art.mu);
    ++g_art.frame;
    g_art.decodedThisFrame = 0;
}

// The GhUiArtFn the UI system calls. Never blocks, never touches the network,
// and answers nullptr for everything it does not already hold -- which is the
// cue for the caller to draw the typographic plate.
inline ID2D1Bitmap* Provide(void* /*ctx*/, uint32_t app, int kind)
{
    if (!app || !g_art.running.load() || !g_ghUi.dc) return nullptr;
    if (kind < 0 || kind >= GH_ART_KINDS) return nullptr;

    std::unique_lock<std::mutex> lk(g_art.mu);
    Item* it = Find(app, kind);
    if (!it) {
        if (!Allowed(app)) return nullptr;        // not the signed-in account's: never fetched
        g_art.items.push_back(Item{});
        it = &g_art.items.back();
        it->app = app; it->kind = kind; it->state = kUnknown;
    }
    it->used = g_art.frame;

    switch (it->state) {
    case kReady:
        return it->bmp.get();
    case kAbsent:
    case kQueued:
        return nullptr;
    case kPixels: {
        if (g_art.decodedThisFrame >= kDecodePerFrame) return nullptr;
        ++g_art.decodedThisFrame;
        // Copy out and unlock: CreateBitmap can take a moment and the worker
        // should not be waiting on the UI thread's D2D call.
        std::vector<uint8_t> px = std::move(it->px);
        const uint32_t w = it->w, h = it->h;
        const uint32_t a = it->app; const int k = it->kind;
        it->px.clear();
        lk.unlock();

        GhCom<ID2D1Bitmap> bmp;
        const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        const HRESULT hr = g_ghUi.dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, bmp.put());

        lk.lock();
        Item* again = Find(a, k);                 // the vector may have grown meanwhile
        if (!again) return nullptr;
        if (FAILED(hr) || !bmp) { again->state = kAbsent; return nullptr; }
        again->bmp = bmp;
        again->state = kReady;
        again->used = g_art.frame;
        // Eviction cannot take the one just made -- it is the most recently
        // used of its kind -- but it can move the vector, so re-find.
        EvictLocked(k, k == GH_ART_HERO ? kMaxHeroes : kMaxCovers);
        again = Find(a, k);
        return again ? again->bmp.get() : nullptr;
    }
    case kUnknown:
    default:
        if (!Allowed(app)) { it->state = kAbsent; return nullptr; }
        if ((int)g_art.queue.size() >= kQueueMax) {
            // The oldest request loses its place. Put it back to kUnknown as it
            // goes, or it would sit in kQueued for ever with nothing coming:
            // the page asks once per frame, so it will simply ask again.
            const auto drop = g_art.queue.front();
            g_art.queue.pop_front();
            if (Item* d = Find(drop.first, drop.second))
                if (d->state == kQueued) d->state = kUnknown;
            it = Find(app, kind);                 // Find may not have moved it, but be exact
            if (!it) return nullptr;
        }
        it->state = kQueued;
        g_art.queue.push_back({ app, kind });
        g_art.cv.notify_one();
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
// `apps` is the app ids the library reported for the signed-in account. Called
// again after every enumeration; a title that leaves the list keeps whatever is
// already cached (it is still the same account's) but is not re-fetched.
inline void Allow(const std::vector<uint32_t>& apps)
{
    std::lock_guard<std::mutex> lk(g_art.mu);
    g_art.allowed = apps;
}

inline void AllowAll(bool on)                      // the preview harness only
{
    std::lock_guard<std::mutex> lk(g_art.mu);
    g_art.allowAll = on;
}

// `dir` is a directory for the cache, with its trailing separator; `diskCap` is
// what it may occupy (kDiskCapDev on D:\DevelopmentFiles, kDiskCapLocal in
// LocalState -- see the budget note at the top). Failing to make the directory
// is not fatal to anything: art is simply off, and every title draws its plate.
inline bool Init(const std::wstring& dir, uint64_t diskCap, void (*log)(const std::string&))
{
    if (g_art.running.load()) return true;
    g_art.log = log;
    g_art.dir = dir;
    g_art.diskCap = diskCap;
    {   // CreateDirectoryW wants the path without its trailing separator.
        std::wstring d = dir;
        while (!d.empty() && d.back() == L'\\') d.pop_back();
        CreateDirectoryW(d.c_str(), nullptr);
        const DWORD at = GetFileAttributesW(d.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {
            Log("no cache directory (" + std::to_string(GetLastError()) + "); art is off");
            return false;
        }
    }
    PruneDisk();
    g_art.stop.store(false);
    g_art.running.store(true);
    g_art.worker = std::thread(&WorkerLoop);
    GhUiSetArtProvider(&Provide, nullptr);
    Log("ready, cache cap " + std::to_string(diskCap / (1024 * 1024)) + " MB");
    return true;
}

inline void Shutdown()
{
    if (!g_art.running.load()) return;
    g_art.stop.store(true);
    g_art.cv.notify_all();
    if (g_art.worker.joinable()) g_art.worker.join();
    g_art.running.store(false);
    GhUiSetArtProvider(nullptr, nullptr);
    std::lock_guard<std::mutex> lk(g_art.mu);
    g_art.items.clear();
    g_art.queue.clear();
}

// One line for the diag, on demand: what the session actually fetched.
inline std::string Stats()
{
    int ready = 0, queued = 0, absent = 0;
    {
        std::lock_guard<std::mutex> lk(g_art.mu);
        for (const auto& it : g_art.items) {
            if (it.state == kReady) ++ready;
            else if (it.state == kQueued || it.state == kPixels) ++queued;
            else if (it.state == kAbsent) ++absent;
        }
    }
    return "live=" + std::to_string(ready) + " pending=" + std::to_string(queued) +
           " none=" + std::to_string(absent) + " fetched=" + std::to_string(g_art.fetched.load()) +
           " miss=" + std::to_string(g_art.missing.load()) +
           " baddecode=" + std::to_string(g_art.failed.load());
}

}  // namespace ghart
