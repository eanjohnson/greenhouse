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

// Steam's store art lives at TWO path shapes, and a console needs both.
//
// The old one is flat and guessable: /steam/apps/<appid>/library_600x900.jpg.
// It still answers 200 for most of a library and costs nothing to try, so it
// is still what we ask for first.
//
// The new one is not guessable at all. Steam now files each asset under a
// per-asset content hash --
//   store_item_assets/steam/apps/<appid>/<40 hex>/library_capsule.jpg
// -- and the filename moved too: `library_capsule.jpg` for most recent items,
// still `library_600x900.jpg` for some, and `portrait.png` for a handful of
// older republished ones. Measured on this library, 2026-11-10: 134 of 156
// owned titles answer on the flat path and 22 only exist behind a hash, which
// is why Dead as Disco, Cheese Rolling, RV There Yet?, Scratch Creek, PEAK,
// Forza Horizon 6 and all three Disney Infinity Gold Editions came out as
// typographic plates. No amount of guessing finds a 40-hex directory, so for
// those we ASK: IStoreBrowseService/GetItems answers with `asset_url_format`
// (the path, hash included) and the real filename per asset.
//
// Note the host differs too. `shared.cloudflare.steamstatic.com` 301s to
// `shared.steamstatic.com`; we name the latter and save the hop.
constexpr const char* kHost      = "https://cdn.cloudflare.steamstatic.com/steam/apps/";
constexpr const char* kAssetHost = "https://shared.steamstatic.com/store_item_assets/";
constexpr const char* kStoreApi  =
    "https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?input_json=";
// A GetItems reply for one title is a few KB. This is a ceiling, not a size.
constexpr uint64_t kMaxJsonBytes = 256ull * 1024;

// The two assets the Cabinet layout draws. `logo.png` is deliberately not here:
// the title screen sets the name in type over the hero, so a wordmark PNG would
// be a second download for a worse result on a television.
inline const char* AssetName(int kind)
{
    return kind == GH_ART_HERO ? "library_hero.jpg" : "library_600x900.jpg";
}

// The same two assets, under the names the store API files them by. The API
// gives the extension as part of the filename, so .png cases come out right
// without us knowing which they are.
inline const char* AssetKey(int kind)
{
    return kind == GH_ART_HERO ? "library_hero" : "library_capsule";
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
// How wide the warmed margin is lives with the seam, as kGhUiArtWarmRows in
// gh_ui.h: it is the layout that knows what a row is. What matters HERE is that
// it fits. The wall is 6 wide and 2 rows tall, so two rows of margin is 24
// covers warmed around 12 drawn -- 36 live, inside the 44 a cover cache holds,
// which is why kMaxCovers did not have to move. A third row would not fit.
//
// The warm lane's own bound. It is re-asked every frame for whatever is beside
// the viewport, so it only ever holds one viewport's worth plus whatever the
// last scroll position left behind.
constexpr int      kWarmMax         = 64;
// Bitmaps created per frame for covers that are NOT on screen yet. Separate
// from kDecodePerFrame on purpose: a wall that is busy drawing must not starve
// the warming that stops it being busy next time, and a wall that is idle
// (which is most frames while reading the shelf) gets through a row in six
// frames. Worst case the two budgets together are four uploads in a frame.
constexpr int      kPromotePerFrame = 2;
// The prefetch list is the whole owned library, not a screenful, so it gets a
// bound of its own: 171 titles today, and a number big enough that growing the
// library does not quietly truncate it.
constexpr int      kPrefetchMax    = 512;
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
    // Covers just outside the viewport: fetched and decoded exactly like the
    // drawn ones, but drained only when `queue` is empty, so a tile the user is
    // looking at is never behind one they might scroll to. See Warm().
    std::deque<std::pair<uint32_t, int>> warm;
    std::vector<uint32_t>   allowed;          // app ids the library reported
    bool                    allowAll = false; // only the preview harness sets this
    // Covers to put on disk whether or not anything is drawing them, in the
    // order the library reported -- which is the order the wall is in. Drained
    // only when `queue` is empty, so a tile on screen is never behind a
    // prefetch. See Prefetch() for why this is separate from `queue`.
    std::deque<uint32_t>    prefetch;

    std::thread      worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    uint64_t         frame = 0;
    int              decodedThisFrame = 0;
    std::atomic<int> fetched{0}, missing{0}, failed{0};
    // Of `fetched`, how many only existed behind the store API's hashed path.
    std::atomic<int> resolved{0};
    int              saidFetched = -1, saidMissing = -1, saidFailed = -1;  // worker thread only
    // FRAMES in which a cover was drawn as a typographic plate while its art
    // was still on its way -- one cover plated for ten frames counts ten. This
    // is the number the warming exists to hold still: it climbs once as a page
    // fills and must then stop, because a cover that is ready before it is on
    // screen never lands here. If it keeps climbing while someone scrolls, the
    // margin is too small or the worker is not keeping up.
    std::atomic<int> late{0};
    int              saidLate = -1;           // UI thread only
    uint64_t         saidLateFrame = 0;
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
// Returns how many files it deleted, which the prefetch below reads: a prune
// during a prefetch means the cache is too small for the library, and carrying
// on would delete the covers the same pass just wrote.
inline int PruneDisk()
{
    struct F { std::wstring name; uint64_t bytes; uint64_t when; };
    std::vector<F> f;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_art.dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER sz{}; sz.LowPart = fd.nFileSizeLow; sz.HighPart = fd.nFileSizeHigh;
        ULARGE_INTEGER w{};  w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        f.push_back({ fd.cFileName, sz.QuadPart, w.QuadPart });
        total += sz.QuadPart;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total <= g_art.diskCap) return 0;

    for (size_t i = 1; i < f.size(); ++i)          // insertion sort, oldest first
        for (size_t j = i; j > 0 && f[j - 1].when > f[j].when; --j) std::swap(f[j - 1], f[j]);

    int gone = 0;
    for (const auto& e : f) {
        if (total <= g_art.diskCap) break;
        if (DeleteFileW((g_art.dir + e.name).c_str())) { total -= e.bytes; ++gone; }
    }
    if (gone) Log("cache pruned: " + std::to_string(gone) + " file(s), now " +
                  std::to_string(total / 1024) + " KB");
    return gone;
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

// ---------------------------------------------------------------------------
// Where the art actually is
// ---------------------------------------------------------------------------
inline std::string UrlEncode(const std::string& in)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

// Ask Steam where this title's asset lives, for the titles whose art is behind
// a content hash. Returns "" for "Steam does not have one", which the caller
// treats exactly as a 404 on the flat path -- an absence, not a failure.
//
// One request per title, and only for titles the flat path has already refused,
// so a library of 171 costs about 20 of these once and nothing afterwards: the
// bytes land in the disk cache and neither path is walked again. GetItems does
// take a batch of ids, and if this ever runs at a size where that matters, the
// place to batch it is the prefetch queue, not here.
inline std::string ResolveAssetUrl(uint32_t app, int kind)
{
    const std::string body =
        "{\"ids\":[{\"appid\":" + std::to_string(app) + "}],"
        "\"context\":{\"language\":\"english\",\"country_code\":\"US\"},"
        "\"data_request\":{\"include_assets\":true}}";
    std::vector<uint8_t> b;
    std::string why;
    uint64_t got = 0;
    if (!ghrepo::HttpGet(std::string(kStoreApi) + UrlEncode(body), kMaxJsonBytes,
                         &b, nullptr, nullptr, &got, why) || b.size() < 16)
        return {};

    ghjson::Value j;
    std::string err;
    if (!ghjson::Parse((const char*)b.data(), b.size(), j, err)) return {};
    const ghjson::Value* r  = j.get("response");            if (!r)  return {};
    const ghjson::Value* si = r->get("store_items");
    if (!si || !si->isArr() || si->arr.empty()) return {};
    const ghjson::Value* as = si->arr[0].get("assets");     if (!as) return {};
    const ghjson::Value* fm = as->get("asset_url_format");
    const ghjson::Value* nm = as->get(AssetKey(kind));
    if (!fm || !fm->isStr() || !nm || !nm->isStr() || nm->s.empty()) return {};

    // asset_url_format is "steam/apps/<id>/${FILENAME}?t=<stamp>" and the asset
    // name is "<hash>/<file>". The stamp is Steam's own cache buster: keep it,
    // so a title that changes its capsule is not served the old one forever.
    const std::string tok = "${FILENAME}";
    std::string path = fm->s;
    const size_t at = path.find(tok);
    if (at == std::string::npos) return {};
    path.replace(at, tok.size(), nm->s);
    return std::string(kAssetHost) + path;
}

// Fetch one asset: the flat path, then the hashed one, then give up.
//
// `absent` separates "Steam has no such asset" from "this console could not
// ask", because only the first may be remembered -- a dropped association that
// wrote absent markers would blank a wall for fourteen days. A 4xx from BOTH
// paths is an absence; anything else is this attempt failing.
inline bool FetchAsset(uint32_t app, int kind, std::vector<uint8_t>& bytes,
                       bool& absent, std::string& why)
{
    absent = false;
    bytes.clear();
    uint64_t got = 0;
    const std::string flat = std::string(kHost) + std::to_string(app) + "/" + AssetName(kind);
    if (ghrepo::HttpGet(flat, kMaxFetchBytes, &bytes, nullptr, nullptr, &got, why) &&
        bytes.size() > 64)
        return true;

    // Not a 4xx: the network, not the asset. Do not spend a second request on it.
    if (why.compare(0, 6, "HTTP 4") != 0) return false;

    const std::string url = ResolveAssetUrl(app, kind);
    if (url.empty()) { absent = true; return false; }

    bytes.clear();
    if (ghrepo::HttpGet(url, kMaxFetchBytes, &bytes, nullptr, nullptr, &got, why) &&
        bytes.size() > 64) {
        g_art.resolved.fetch_add(1);
        return true;
    }
    absent = (why.compare(0, 6, "HTTP 4") == 0);
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
        std::string why;
        bool absent = false;
        // The flat path, then the hashed one. "Steam has no such asset" and
        // "the console could not reach Steam" are NOT the same answer, and
        // treating them the same was a bug: a dropped Wi-Fi association wrote
        // absent markers and blanked a wall for fourteen days.
        const bool ok = FetchAsset(app, kind, bytes, absent, why);
        if (!ok) {
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

// ---------------------------------------------------------------------------
// The prefetch (worker thread)
// ---------------------------------------------------------------------------
// One title's cover, and only as far as the disk.
//
// Deliberately NOT WorkerOne: this neither decodes nor creates an Item. The
// whole library decoded at 360x540 would be 133 MB of pixel buffers with
// nothing to consume them -- Provide() turns pixels into a bitmap only for rows
// a layout is actually drawing, two a frame, and holds 44 of them at most. The
// point of the prefetch is that the BYTES are local, so that when a row does
// come on screen its fetch is a file read instead of a round trip to Steam.
//
// Everything that decides WHICH titles may be fetched has already happened:
// Prefetch() is handed the list the library reported, which is the same list
// Allowed() checks against.
inline void StopPrefetch(const char* why)
{
    size_t left = 0;
    {
        std::lock_guard<std::mutex> lk(g_art.mu);
        left = g_art.prefetch.size();
        g_art.prefetch.clear();
    }
    if (left) Log(std::string(why) + ": prefetch stopped with " + std::to_string(left) +
                  " title(s) left, which stay on demand");
}

inline void PrefetchOne(uint32_t app)
{
    const int kind = GH_ART_COVER;
    // Already here, or a fresh "Steam has nothing for this one". Both are a
    // file-attribute call and no network at all, which is what makes it safe to
    // re-queue the whole library after every enumeration.
    if (GetFileAttributesW(CachePath(app, kind).c_str()) != INVALID_FILE_ATTRIBUTES) return;
    uint64_t days = 0;
    if (FileAgeDays(AbsentPath(app, kind), &days) && days < kAbsentDays) return;

    std::string why;
    std::vector<uint8_t> bytes;
    bool absent = false;
    if (!FetchAsset(app, kind, bytes, absent, why)) {
        if (absent) {
            // A miss is normal and permanent-ish: remember it exactly as the
            // on-demand path does, so neither path asks again for 14 days.
            g_art.missing.fetch_add(1);
            HANDLE h = CreateFileW(AbsentPath(app, kind).c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            return;
        }
        // Not an absence -- the console could not reach Steam. One title
        // failing this way means the rest will too, and a prefetch is a
        // convenience: 171 doomed round trips on a dropped association would
        // hold the worker for minutes and starve the tiles that ARE on screen.
        if (g_art.failed.fetch_add(1) == 0)
            Log("prefetch failed (app " + std::to_string(app) + "): " + why);
        StopPrefetch("unreachable");
        return;
    }

    HANDLE h = CreateFileW(CachePath(app, kind).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
    CloseHandle(h);
    DeleteFileW(AbsentPath(app, kind).c_str());
    g_art.fetched.fetch_add(1);

    // A prune here means the cache is smaller than the library, so the pass is
    // now deleting its own head -- and the next launch would start the same
    // treadmill. Stop instead, and let what is left be fetched on demand.
    if (PruneDisk() > 0) StopPrefetch("cache full");
}

inline void WorkerLoop()
{
    for (;;) {
        uint32_t app = 0; int kind = 0; bool pre = false;
        {
            std::unique_lock<std::mutex> lk(g_art.mu);
            g_art.cv.wait(lk, []{ return g_art.stop.load() || !g_art.queue.empty() ||
                                         !g_art.warm.empty() || !g_art.prefetch.empty(); });
            if (g_art.stop.load()) return;
            if (!g_art.queue.empty()) {
                // LIFO: the newest request is the one the user is looking at. A
                // queue drained in order would spend its first second on the covers
                // that scrolled off the top.
                app = g_art.queue.back().first;
                kind = g_art.queue.back().second;
                g_art.queue.pop_back();
            } else if (!g_art.warm.empty()) {
                // Nothing on screen is waiting, so get ahead of the scroll.
                // LIFO here too: the newest warm request is the margin beside
                // where the user is NOW, and the oldest is beside where they
                // were several rows ago.
                app  = g_art.warm.back().first;
                kind = g_art.warm.back().second;
                g_art.warm.pop_back();
            } else {
                // Nothing is waiting to be drawn or scrolled to, so fill the
                // disk cache.
                // FIFO, because unlike the on-screen queue this list has no
                // "most recent" end: it is the library in library order.
                app = g_art.prefetch.front();
                g_art.prefetch.pop_front();
                pre = true;
            }
        }
        if (pre) PrefetchOne(app);
        else     WorkerOne(app, kind);

        // Say once per drained queue what the wall is actually made of. Without
        // this a plate on the TV has no explanation in the diag at all: nothing
        // distinguished "Steam has no capsule for this title" from "the fetch
        // failed", and the counters existed but were never printed.
        bool drained = false;
        size_t left = 0;
        {
            std::lock_guard<std::mutex> lk(g_art.mu);
            drained = g_art.queue.empty();
            left    = g_art.prefetch.size();
        }
        // The prefetch moves these counters on every title, and a line each
        // would be 171 of them in a diag that has a size budget. Say it every
        // 25th, and always on the last one.
        if (drained && (left == 0 || (g_art.fetched.load() % 25) == 0)) {
            const int f = g_art.fetched.load(), m = g_art.missing.load(),
                      x = g_art.failed.load();
            if (f != g_art.saidFetched || m != g_art.saidMissing || x != g_art.saidFailed) {
                g_art.saidFetched = f; g_art.saidMissing = m; g_art.saidFailed = x;
                const int rv = g_art.resolved.load();
                Log("queue drained: " + std::to_string(f) + " fetched" +
                    (rv ? (" (" + std::to_string(rv) + " via the store API)") : std::string()) +
                    ", " + std::to_string(m) + " with no asset, " +
                    std::to_string(x) + " failed" +
                    (left ? (", " + std::to_string(left) + " still to prefetch")
                          : std::string()));
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

// Turn a decoded item's pixels into a drawable D2D bitmap. The caller holds
// `lk`; this unlocks around CreateBitmap, because that call can take a moment
// and the worker should not be waiting on the UI thread's D2D.
//
// Both the drawn path and the warm path come through here, so there is one
// implementation of "pixels become a bitmap" rather than two that could drift.
// Returns the bitmap, or nullptr if it could not be made.
inline ID2D1Bitmap* MakeReady(Item* it, std::unique_lock<std::mutex>& lk)
{
    if (!it || it->state != kPixels || !g_ghUi.dc) return nullptr;

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
    // Eviction cannot take the one just made -- it is the most recently used of
    // its kind -- but it can move the vector, so re-find.
    EvictLocked(k, k == GH_ART_HERO ? kMaxHeroes : kMaxCovers);
    again = Find(a, k);
    return again ? again->bmp.get() : nullptr;
}

// Called once per frame by the page that draws art, before drawing it.
inline void BeginFrame()
{
    std::unique_lock<std::mutex> lk(g_art.mu);
    ++g_art.frame;
    g_art.decodedThisFrame = 0;
    if (!g_ghUi.dc) return;

    // Make bitmaps for covers that are decoded but not on screen yet, so the
    // row below the fold is drawable the moment it scrolls up rather than
    // starting its upload then. Most recently wanted first, which is the margin
    // nearest the viewport; a warm item the user has scrolled away from falls
    // behind on `used` and waits, which is what we want.
    for (int n = 0; n < kPromotePerFrame; ++n) {
        Item* best = nullptr;
        for (auto& it : g_art.items)
            if (it.state == kPixels && (!best || it.used > best->used)) best = &it;
        if (!best) break;
        MakeReady(best, lk);                  // unlocks and relocks; `best` dies here
    }

    // What the wall is costing, in frames where a cover that DOES exist was
    // drawn as its plate because the art had not caught up. That is the one
    // fault this file is here to prevent, and without a number the only
    // instrument is somebody's eye on a television.
    //
    // It counts frames, not covers: one cover plated for ten frames is ten.
    // That is deliberate -- what is noticeable is how long the wrong thing was
    // on screen, not how many tiles were involved.
    //
    // Said when it moves, and otherwise every 1800 frames (~30 s) so there is
    // always a current reading. An earlier version spoke ONLY when the number
    // moved, which meant it went silent exactly when the warming was working:
    // a healthy wall and a closed page looked identical in the diag, and the
    // only reading on file was the one from before the margin had filled.
    // Built under the lock but logged outside it: the worker should not wait on
    // the diag's disk write.
    std::string line;
    const int lt = g_art.late.load();
    const bool moved = (lt != g_art.saidLate);
    if (g_art.frame >= g_art.saidLateFrame + (moved ? 300 : 1800)) {
        int ready = 0, pend = 0, gone = 0;
        for (const auto& it : g_art.items) {
            if (it.state == kReady) ++ready;
            else if (it.state == kQueued || it.state == kPixels) ++pend;
            else if (it.state == kAbsent) ++gone;
        }
        line = "wall: late=" + std::to_string(lt) + " frame(s)" +
               (moved ? " (+" + std::to_string(lt - (g_art.saidLate < 0 ? 0 : g_art.saidLate)) + ")"
                      : std::string(" (steady)")) +
               ", live=" + std::to_string(ready) +
               " pending=" + std::to_string(pend) +
               " warm=" + std::to_string(g_art.warm.size()) +
               " none=" + std::to_string(gone);
        g_art.saidLate = lt;
        g_art.saidLateFrame = g_art.frame;
    }
    lk.unlock();
    if (!line.empty()) Log(line);
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
        return nullptr;                       // no art exists; the plate IS the answer
    case kQueued:
        if (kind == GH_ART_COVER) g_art.late.fetch_add(1);
        return nullptr;
    case kPixels: {
        if (g_art.decodedThisFrame >= kDecodePerFrame) {
            if (kind == GH_ART_COVER) g_art.late.fetch_add(1);
            return nullptr;
        }
        ++g_art.decodedThisFrame;
        return MakeReady(it, lk);
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
        if (kind == GH_ART_COVER) g_art.late.fetch_add(1);
        return nullptr;
    }
}

// The GhUiArtWarmFn the UI system calls for tiles that are NOT being drawn --
// the rows just outside the viewport. Same pipeline as Provide, one lane down:
// fetch, decode, and (via BeginFrame) a bitmap, so that by the time the row
// scrolls up there is nothing left to do but draw it.
//
// Called for every off-screen tile of every frame, so the common case -- we
// already have it, or already asked -- has to be a lock and two compares.
inline void Warm(void* /*ctx*/, uint32_t app, int kind)
{
    if (!app || !g_art.running.load()) return;
    if (kind < 0 || kind >= GH_ART_KINDS) return;

    std::lock_guard<std::mutex> lk(g_art.mu);
    Item* it = Find(app, kind);
    if (!it) {
        if (!Allowed(app)) return;            // not the signed-in account's: never fetched
        g_art.items.push_back(Item{});
        it = &g_art.items.back();
        it->app = app; it->kind = kind; it->state = kUnknown;
    }
    // Being beside the viewport counts as being used, or the LRU would drop the
    // margin we just paid for. A cover the user scrolls away from stops being
    // warmed, goes stale on `used` by itself, and is evicted in its turn.
    it->used = g_art.frame;
    if (it->state != kUnknown) return;        // ready, queued, decoding, or absent

    if (!Allowed(app)) { it->state = kAbsent; return; }
    if ((int)g_art.warm.size() >= kWarmMax) {
        // Oldest loses its place, exactly as in Provide: back to kUnknown as it
        // goes, or it would sit in kQueued for ever with nothing coming.
        const auto drop = g_art.warm.front();
        g_art.warm.pop_front();
        if (Item* d = Find(drop.first, drop.second))
            if (d->state == kQueued) d->state = kUnknown;
        it = Find(app, kind);
        if (!it) return;
    }
    it->state = kQueued;
    g_art.warm.push_back({ app, kind });
    g_art.cv.notify_one();
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

// Put every owned title's cover on disk, whether or not anything is drawing it.
//
// Provide() is demand-driven, and is only ever asked about the rows a layout is
// DRAWING. That is right for memory -- 44 live bitmaps, not 171 -- but it meant
// the BYTES were fetched at the moment a tile appeared, so scrolling the wall
// showed a column of plates that filled in a beat later, and a title the user
// never scrolled to had no art at all. Installed titles sort first, so what it
// looked like from the sofa was "art only for the games that are installed".
//
// The disk is the right place to fix it and video memory is not: a capsule is
// ~90 KB, so the whole library is ~17 MB against a 48 MB cap, while the same
// library decoded would be 133 MB. So this fills the disk cache and stops
// there; the in-memory LRU is untouched and still demand-driven.
//
// Called with the same list as Allow(), after every enumeration. Re-queueing a
// title already on disk costs one file-attribute call and no network, which is
// what makes calling it again cheap -- and is also how a title bought since the
// last enumeration gets picked up.
inline void Prefetch(const std::vector<uint32_t>& apps)
{
    if (!g_art.running.load()) return;
    size_t added = 0;
    {
        std::lock_guard<std::mutex> lk(g_art.mu);
        for (uint32_t a : apps) {
            if (!a) continue;
            if ((int)g_art.prefetch.size() >= kPrefetchMax) break;
            bool dup = false;
            for (uint32_t q : g_art.prefetch) if (q == a) { dup = true; break; }
            if (!dup) { g_art.prefetch.push_back(a); ++added; }
        }
    }
    if (added) {
        Log("prefetching " + std::to_string(added) + " cover(s) to disk");
        g_art.cv.notify_one();
    }
}

inline void AllowAll(bool on)                      // the preview harness only
{
    std::lock_guard<std::mutex> lk(g_art.mu);
    g_art.allowAll = on;
}

// An absent marker means "Steam has no such asset". Until 0.0.944.0 it meant
// something weaker and wrong -- "the flat CDN path has no such asset" -- and
// that is exactly the case the hashed path now answers. Every marker written
// by an older build is therefore a claim this build can disprove, so they go,
// once, the first time a build that knows about the store API starts. Without
// this a console that already has the covers blanked keeps them blanked for
// the fourteen days the markers are honoured for.
//
// The stamp is a file and not a setting because the cache is the only state
// this decision concerns: delete the cache directory and the question is moot.
inline void ForgetStaleAbsences()
{
    const std::wstring stamp = g_art.dir + L"absence.rule2";
    if (GetFileAttributesW(stamp.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    int gone = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((g_art.dir + L"*.none").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (DeleteFileW((g_art.dir + fd.cFileName).c_str())) ++gone;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    HANDLE t = CreateFileW(stamp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
    if (gone)
        Log("forgot " + std::to_string(gone) + " remembered absence(s): they were "
            "recorded before the store API path existed");
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
    ForgetStaleAbsences();
    PruneDisk();
    g_art.stop.store(false);
    g_art.running.store(true);
    g_art.worker = std::thread(&WorkerLoop);
    GhUiSetArtProvider(&Provide, nullptr);
    GhUiSetArtWarmer(&Warm, nullptr);
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
    GhUiSetArtWarmer(nullptr, nullptr);
    std::lock_guard<std::mutex> lk(g_art.mu);
    g_art.items.clear();
    g_art.queue.clear();
    g_art.warm.clear();
    g_art.prefetch.clear();
}

// One line for the diag, on demand: what the session actually fetched.
inline std::string Stats()
{
    int ready = 0, queued = 0, absent = 0;
    size_t pre = 0;
    {
        std::lock_guard<std::mutex> lk(g_art.mu);
        pre = g_art.prefetch.size();
        for (const auto& it : g_art.items) {
            if (it.state == kReady) ++ready;
            else if (it.state == kQueued || it.state == kPixels) ++queued;
            else if (it.state == kAbsent) ++absent;
        }
    }
    return "live=" + std::to_string(ready) + " pending=" + std::to_string(queued) +
           " prefetch=" + std::to_string(pre) +
           " none=" + std::to_string(absent) + " fetched=" + std::to_string(g_art.fetched.load()) +
           " miss=" + std::to_string(g_art.missing.load()) +
           " baddecode=" + std::to_string(g_art.failed.load()) +
           " late=" + std::to_string(g_art.late.load());
}

}  // namespace ghart
