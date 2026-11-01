// cm_workshop.h -- Steam Workshop over our own CM client (namespace ghdl).
//
// Workshop items for SteamPipe games are depot manifests: the item's
// hcontent_file is a manifest id in the app's own workshop depot (depot id ==
// app id), keyed with that depot's ordinary depot key. So an item downloads
// exactly like a game depot -- DownloadManifestOn over the signed-in session --
// into <contentRoot>\<publishedfileid>\, which is Steam's own
// steamapps\workshop\content\<app>\<id> layout.
//
// Everything goes through PublishedFile.* unified messages on the user's own
// session (the same logon the library and downloads use): their subscriptions,
// item details, browsing, subscribe/unsubscribe. Nothing is hosted or mirrored by
// us; every byte comes from Steam's CDN to the console.
//
// Field numbers: steammessages_publishedfile.steamclient.proto.
//   CPublishedFile_GetUserFiles_Request { fixed64 steamid=1; uint32 appid=2;
//     uint32 page=4; uint32 numperpage=5; string type=6; }  -> Response
//     { uint32 total=1; repeated PublishedFileDetails publishedfiledetails=3; }
//   CPublishedFile_GetDetails_Request { repeated fixed64 publishedfileids=1;
//     uint32 appid=14; } -> Response { repeated PublishedFileDetails = 1; }
//   CPublishedFile_QueryFiles_Request { uint32 query_type=1; uint32 page=2;
//     uint32 numperpage=3; uint32 appid=5; string search_text=11;
//     bool return_details=37; } -> Response { uint32 total=1; details=2; }
//   CPublishedFile_Subscribe/Unsubscribe_Request { uint64 publishedfileid=1;
//     uint32 list_type=2; int32 appid=3; bool notify_client=4; }
//   PublishedFileDetails { uint32 result=1; uint64 publishedfileid=2;
//     uint32 consumer_appid=5; string filename=7; uint64 file_size=8;
//     string file_url=10; string preview_url=11; fixed64 hcontent_file=14;
//     string title=16; string short_description=18; uint32 time_updated=20;
//     bool banned=28; uint32 subscriptions=36; }
#pragma once
#include "cm_downloader.h"
#include <map>
#include <set>

namespace ghdl {

struct WsItem {
    uint64_t id = 0, manifest = 0, size = 0;
    uint32_t app = 0, result = 0, updated = 0, subs = 0;
    bool banned = false;
    std::string title, filename, file_url, preview_url, desc;
};

inline void ws_parse_details(const Bytes& b, WsItem& it){
    PbReader r(b.data(), b.size()); uint32_t f, w;
    auto str = [&](std::string& s){ Bytes x = r.lendelim(); s.assign(x.begin(), x.end()); };
    while (r.next(f, w)){
        if      (f == 1  && w == 0) it.result   = (uint32_t)r.varint();
        else if (f == 2  && w == 0) it.id       = r.varint();
        else if (f == 5  && w == 0) it.app      = (uint32_t)r.varint();
        else if (f == 7  && w == 2) str(it.filename);
        else if (f == 8  && w == 0) it.size     = r.varint();
        else if (f == 10 && w == 2) str(it.file_url);
        else if (f == 11 && w == 2) str(it.preview_url);
        else if (f == 14 && w == 1) it.manifest = r.fix64();
        else if (f == 16 && w == 2) str(it.title);
        else if (f == 18 && w == 2) str(it.desc);
        else if (f == 20 && w == 0) it.updated  = (uint32_t)r.varint();
        else if (f == 28 && w == 0) it.banned   = r.varint() != 0;
        else if (f == 36 && w == 0) it.subs     = (uint32_t)r.varint();
        else r.skip(w);
    }
}

// The account's subscriptions for one app, every page. false = the call failed
// (not "no subscriptions": that is true with out empty).
inline bool ws_subscribed(CmClient& c, uint64_t sid, uint32_t app, std::vector<WsItem>& out){
    for (uint32_t page = 1; page <= 50; ++page){
        Bytes body; pb_fix64(body, 1, sid); pb_u32(body, 2, app); pb_u32(body, 4, page);
        pb_u32(body, 5, 100); pb_str(body, 6, "mysubscriptions");
        Bytes resp; int32_t er = 0;
        if (!c.um("PublishedFile.GetUserFiles#1", body, resp, &er) || er != 1){
            ghlog("ws subscribed page=" + std::to_string(page) + " er=" + std::to_string(er));
            return false;
        }
        uint32_t total = 0; size_t got = 0;
        PbReader r(resp.data(), resp.size()); uint32_t f, w;
        while (r.next(f, w)){
            if (f == 1 && w == 0) total = (uint32_t)r.varint();
            else if (f == 3 && w == 2){ WsItem it; ws_parse_details(r.lendelim(), it); if (it.id){ out.push_back(it); ++got; } }
            else r.skip(w);
        }
        if (!got || out.size() >= total) break;
    }
    ghlog("ws subscribed app=" + std::to_string(app) + " n=" + std::to_string(out.size()));
    return true;
}

inline bool ws_details(CmClient& c, uint32_t app, const std::vector<uint64_t>& ids, std::vector<WsItem>& out){
    for (size_t i = 0; i < ids.size(); i += 100){
        Bytes body;
        for (size_t k = i; k < ids.size() && k < i + 100; ++k) pb_fix64(body, 1, ids[k]);
        if (app) pb_u32(body, 14, app);
        Bytes resp; int32_t er = 0;
        if (!c.um("PublishedFile.GetDetails#1", body, resp, &er) || er != 1) return false;
        PbReader r(resp.data(), resp.size()); uint32_t f, w;
        while (r.next(f, w)){
            if (f == 1 && w == 2){ WsItem it; ws_parse_details(r.lendelim(), it); if (it.id) out.push_back(it); }
            else r.skip(w);
        }
    }
    return true;
}

// EPublishedFileQueryType: 0 by vote, 1 newest, 3 trend, 9 most subscribed,
// 12 text search. page is 1-based.
inline bool ws_query(CmClient& c, uint32_t app, uint32_t qtype, uint32_t page, uint32_t perPage,
                     const std::string& search, std::vector<WsItem>& out, uint32_t* total){
    Bytes body; pb_u32(body, 1, search.empty() ? qtype : 12); pb_u32(body, 2, page ? page : 1);
    pb_u32(body, 3, perPage ? perPage : 20); pb_u32(body, 5, app);
    if (!search.empty()) pb_str(body, 11, search);
    if (qtype == 3) pb_u32(body, 14, 7);           // trend window: a week
    pb_bool(body, 22, true);                       // return_short_description
    pb_bool(body, 37, true);                       // return_details
    Bytes resp; int32_t er = 0;
    if (!c.um("PublishedFile.QueryFiles#1", body, resp, &er) || er != 1){
        ghlog("ws query er=" + std::to_string(er)); return false;
    }
    PbReader r(resp.data(), resp.size()); uint32_t f, w;
    while (r.next(f, w)){
        if (f == 1 && w == 0){ uint32_t t = (uint32_t)r.varint(); if (total) *total = t; }
        else if (f == 2 && w == 2){ WsItem it; ws_parse_details(r.lendelim(), it); if (it.id) out.push_back(it); }
        else r.skip(w);
    }
    return true;
}

// list_type 1 = k_EUCMListTypeSubscribed. Returns the EResult (1 = OK).
inline int32_t ws_subscribe(CmClient& c, uint32_t app, uint64_t id, bool sub){
    Bytes body; pb_u64(body, 1, id); pb_u32(body, 2, 1); pb_u32(body, 3, app); pb_bool(body, 4, true);
    Bytes resp; int32_t er = 0;
    if (!c.um(sub ? "PublishedFile.Subscribe#1" : "PublishedFile.Unsubscribe#1", body, resp, &er)) return 0;
    return er;
}

// ---- the installed-items state ---------------------------------------------------
// One line per installed item, tab-separated, the contract with the steam_api64
// shim (which answers the game's ISteamUGC from it):
//   <publishedfileid> <manifest> <size> <time_updated> <folder> <title>
struct WsState { uint64_t manifest = 0, size = 0; uint32_t updated = 0; std::wstring folder; std::string title; };

inline std::string ws_utf8(const std::wstring& w){
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline std::map<uint64_t, WsState> ws_state_read(const std::wstring& path){
    std::map<uint64_t, WsState> m;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return m;
    std::string s; DWORD sz = GetFileSize(h, nullptr), rd = 0; s.resize(sz);
    if (sz && !ReadFile(h, &s[0], sz, &rd, nullptr)) rd = 0;
    CloseHandle(h); s.resize(rd);
    size_t p = 0;
    while (p < s.size()){
        size_t e = s.find('\n', p); std::string line = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? s.size() : e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> col; size_t q = 0;
        for (;;){ size_t t = line.find('\t', q); col.push_back(line.substr(q, t == std::string::npos ? std::string::npos : t - q)); if (t == std::string::npos) break; q = t + 1; }
        if (col.size() < 5) continue;
        uint64_t id = _strtoui64(col[0].c_str(), nullptr, 10); if (!id) continue;
        WsState st; st.manifest = _strtoui64(col[1].c_str(), nullptr, 10); st.size = _strtoui64(col[2].c_str(), nullptr, 10);
        st.updated = (uint32_t)strtoul(col[3].c_str(), nullptr, 10); st.folder = towide(col[4]);
        if (col.size() > 5) st.title = col[5];
        m[id] = st;
    }
    return m;
}

inline bool ws_state_write(const std::wstring& path, const std::map<uint64_t, WsState>& m){
    std::string s;
    for (auto& kv : m){
        std::string t = kv.second.title; for (char& ch : t) if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
        s += std::to_string((unsigned long long)kv.first) + "\t" + std::to_string((unsigned long long)kv.second.manifest) + "\t" +
             std::to_string((unsigned long long)kv.second.size) + "\t" + std::to_string(kv.second.updated) + "\t" +
             ws_utf8(kv.second.folder) + "\t" + t + "\n";
    }
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0; BOOL ok = WriteFile(h, s.data(), (DWORD)s.size(), &wr, nullptr); CloseHandle(h);
    if (!ok || wr != s.size()) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// Files directly in one item folder (items are flat: a .gma or a legacy .bin).
inline void ws_clear_folder(const std::wstring& dir){
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); }
    while (FindNextFileW(f, &fd));
    FindClose(f);
}

// Bytes already in an item folder. A first fetch resumes them (write_or_repair_file)
// and an update deletes them first, so either way they need no new space.
inline uint64_t ws_folder_bytes(const std::wstring& dir){
    uint64_t n = 0;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += ws_folder_bytes(dir + L"\\" + fd.cFileName);
        else n += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

// Free space WorkshopSync leaves on the content drive: games and the pager's
// spill file live there too. The add-on may lower it (gh_ws_reserve_mb.txt).
inline uint64_t g_wsReserve = 4ull << 30;
struct WsSyncResult { int subscribed = 0, installed = 0, fetched = 0, failed = 0, removed = 0; bool cancelled = false; };

// Per-item progress: the item's own percent and bytes under "workshop: <title>",
// files = items done / subscribed.
struct WsItemProg { ProgressFn cb; void* ctx; std::string phase; int fdone, ftot; };
inline void ws_item_prog(void* p, const char*, int pct, int, int, unsigned long long bytes){
    WsItemProg* w = (WsItemProg*)p;
    if (w->cb) w->cb(w->ctx, w->phase.c_str(), pct, w->fdone, w->ftot, bytes);
}

// Bring <contentRoot>\<id>\ in line with the account's subscriptions for app:
// fetch new and updated items, drop folders of items no longer subscribed, and
// rewrite statePath. One logon for the whole pass. cb sees phase "workshop",
// fdone/ftot = items, bytes = item bytes of the current download. Returns 0 on
// success, >0 = items that failed, <0 = setup failure.
inline int WorkshopSync(uint32_t app, uint64_t sid, const std::string& token, const std::string& account,
                        const std::wstring& contentRoot, const std::wstring& statePath,
                        ProgressFn cb, void* ctx, WsSyncResult* res = nullptr){
    WsSyncResult r;
    if (!sid || token.empty()) return -10;
    if (cb) cb(ctx, "connecting", 0, 0, 0, 0);
    CmClient c; if (!c.connect_and_secure()) return -1;
    if (!c.login_token(sid, token, account)){ if (c.s != INVALID_SOCKET) closesocket(c.s); return -2; }
    std::vector<WsItem> subs;
    if (!ws_subscribed(c, sid, app, subs)){ closesocket(c.s); return -3; }
    r.subscribed = (int)subs.size();
    std::map<uint64_t, WsState> st = ws_state_read(statePath), next;
    std::set<uint64_t> want;
    for (auto& it : subs) want.insert(it.id);
    Bytes key; std::vector<std::string> servers; bool haveKey = false;
    bool storageFull = false;
    int idx = 0;
    for (auto& it : subs){
        ++idx;
        std::wstring dir = contentRoot + L"\\" + std::to_wstring((unsigned long long)it.id);
        auto old = st.find(it.id);
        const bool current = old != st.end() && old->second.manifest == it.manifest && it.manifest &&
                             GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (current){ next[it.id] = old->second; next[it.id].title = it.title; ++r.installed; continue; }
        if (cancelled()){
            // B on the sync screen: launch with what is installed. Items not reached
            // keep their old state (an outdated copy beats none).
            r.cancelled = true;
            for (size_t k = idx - 1; k < subs.size(); ++k){
                auto o = st.find(subs[k].id);
                if (o != st.end()){ next[o->first] = o->second; ++r.installed; }
            }
            break;
        }
        if (it.result != 1 || it.banned || (!it.manifest && it.file_url.empty())){
            ghlog("ws skip id=" + std::to_string((unsigned long long)it.id) + " result=" + std::to_string(it.result) +
                  (it.banned ? " banned" : "") + (it.manifest ? "" : " no-manifest"));
            continue;
        }
        if (cb) cb(ctx, ("workshop: " + it.title).c_str(), (int)(idx * 100 / (subs.size() + 1)), idx - 1, (int)subs.size(), 0);
        {   // Leave g_wsReserve on the drive. An item that does not fit is skipped
            // (an older copy is kept) and the pass goes on: smaller items, and items
            // whose bytes are already on disk, may still fit.
            ULARGE_INTEGER fr{}; mkdirs(contentRoot + L"\\");
            const bool haveFree = GetDiskFreeSpaceExW(contentRoot.c_str(), &fr, nullptr, nullptr) != 0;
            const uint64_t have = ws_folder_bytes(dir);
            const uint64_t need = it.size > have ? it.size - have : 0;
            if (haveFree && need && fr.QuadPart < need + g_wsReserve){
                const bool under = fr.QuadPart < g_wsReserve;
                if (!under)
                    ghlog("ws too big id=" + std::to_string((unsigned long long)it.id) + " size=" +
                          std::to_string((unsigned long long)(it.size >> 20)) + "MB need=" +
                          std::to_string((unsigned long long)(need >> 20)) + "MB free=" +
                          std::to_string((unsigned long long)(fr.QuadPart >> 20)) + "MB");
                else if (!storageFull){
                    ghlog("ws storage full before id=" + std::to_string((unsigned long long)it.id) +
                          " free=" + std::to_string((unsigned long long)(fr.QuadPart >> 20)) + "MB");
                    if (cb) cb(ctx, "workshop: storage full", 100, idx - 1, (int)subs.size(), 0);
                }
                storageFull = storageFull || under;
                if (old != st.end()){ next[it.id] = old->second; ++r.installed; }
                ++r.failed;
                continue;
            }
            if (have) ghlog("ws resume id=" + std::to_string((unsigned long long)it.id) + " have=" +
                            std::to_string((unsigned long long)(have >> 20)) + "MB need=" +
                            std::to_string((unsigned long long)(need >> 20)) + "MB");
        }
        mkdirs(dir + L"\\");
        // An update replaces the old version's files; a first fetch keeps whatever
        // an earlier, interrupted pass left (write_or_repair_file resumes it).
        if (old != st.end()) ws_clear_folder(dir);
        ghlog("ws fetch id=" + std::to_string((unsigned long long)it.id) + " " + std::to_string(idx) + "/" +
              std::to_string(subs.size()) + " size=" + std::to_string((unsigned long long)it.size));
        int rc;
        if (it.manifest){
            if (!haveKey){
                if (!c.get_depot_key(app, app, key) || key.size() != 32){ ghlog("ws no workshop depot key app=" + std::to_string(app)); closesocket(c.s); return -4; }
                if (!c.get_content_servers(servers)){ closesocket(c.s); return -5; }
                haveKey = true;
            }
            WsItemProg wp{ cb, ctx, "workshop: " + it.title, idx - 1, (int)subs.size() };
            rc = DownloadManifestOn(c, key, servers, app, app, it.manifest, dir, "public", ws_item_prog, &wp);
            if (rc == -5 && !cancelled()){
                // The manifest code comes over the CM session, which a long CDN
                // download leaves without heartbeats: log on again once and retry.
                ghlog("ws relogon before id=" + std::to_string((unsigned long long)it.id));
                closesocket(c.s); c = CmClient();
                if (!c.connect_and_secure() || !c.login_token(sid, token, account)){ ws_state_write(statePath, next); return -2; }
                rc = DownloadManifestOn(c, key, servers, app, app, it.manifest, dir, "public", ws_item_prog, &wp);
            }
        } else {
            // Pre-SteamPipe item: one file on Steam's UGC CDN.
            std::string u = it.file_url; size_t hs = u.find("://"); hs = hs == std::string::npos ? 0 : hs + 3;
            size_t ps = u.find('/', hs);
            Bytes body = ps == std::string::npos ? Bytes() : http_fetch(towide(u.substr(hs, ps - hs)), towide(u.substr(ps)));
            std::string fn = it.filename.empty() ? std::to_string((unsigned long long)it.id) + ".gma" : it.filename;
            for (char& ch : fn) if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
            rc = 1;
            if (!body.empty()){
                HANDLE h = CreateFileW((dir + L"\\" + towide(fn)).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE){ DWORD wr = 0; if (WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr) && wr == body.size()) rc = 0; CloseHandle(h); }
            }
        }
        ghlog("ws item id=" + std::to_string((unsigned long long)it.id) + " manifest=" + std::to_string((unsigned long long)it.manifest) +
              " size=" + std::to_string((unsigned long long)it.size) + " rc=" + std::to_string(rc));
        if (rc == 0){
            WsState s; s.manifest = it.manifest; s.size = it.size; s.updated = it.updated; s.folder = dir; s.title = it.title;
            next[it.id] = s; ++r.fetched; ++r.installed;
            ws_state_write(statePath, [&]{ auto m = st; for (auto& kv : next) m[kv.first] = kv.second; return m; }());
        } else if (!cancelled()) ++r.failed;
    }
    // Unsubscribed since the last sync: Steam removes the content, and so do we.
    for (auto& kv : st)
        if (!want.count(kv.first) && !kv.second.folder.empty()){
            ws_clear_folder(kv.second.folder); RemoveDirectoryW(kv.second.folder.c_str()); ++r.removed;
        }
    ws_state_write(statePath, next);
    closesocket(c.s);
    if (cb) cb(ctx, r.cancelled ? "workshop: skipped" : r.failed ? "workshop: some items failed" : "workshop: up to date",
               100, r.installed, r.subscribed, 0);
    ghlog("ws sync app=" + std::to_string(app) + " subscribed=" + std::to_string(r.subscribed) + " installed=" + std::to_string(r.installed) +
          " fetched=" + std::to_string(r.fetched) + " failed=" + std::to_string(r.failed) + " removed=" + std::to_string(r.removed) +
          (r.cancelled ? " cancelled" : ""));
    if (res) *res = r;
    return r.failed;
}

// One-shot browse / subscribe for the Mods screen: own logon each call.
inline int WorkshopQuery(uint32_t app, uint64_t sid, const std::string& token, const std::string& account,
                         uint32_t qtype, uint32_t page, const std::string& search, std::vector<WsItem>& out, uint32_t* total){
    if (!sid || token.empty()) return -10;
    CmClient c; if (!c.connect_and_secure()) return -1;
    if (!c.login_token(sid, token, account)){ if (c.s != INVALID_SOCKET) closesocket(c.s); return -2; }
    bool ok = ws_query(c, app, qtype, page, 20, search, out, total);
    closesocket(c.s);
    return ok ? 0 : -3;
}

inline int WorkshopSubscribe(uint32_t app, uint64_t sid, const std::string& token, const std::string& account,
                             uint64_t id, bool sub){
    if (!sid || token.empty()) return -10;
    CmClient c; if (!c.connect_and_secure()) return -1;
    if (!c.login_token(sid, token, account)){ if (c.s != INVALID_SOCKET) closesocket(c.s); return -2; }
    int32_t er = ws_subscribe(c, app, id, sub);
    closesocket(c.s);
    ghlog(std::string("ws ") + (sub ? "subscribe" : "unsubscribe") + " id=" + std::to_string((unsigned long long)id) + " er=" + std::to_string(er));
    return er == 1 ? 0 : -3;
}

} // namespace ghdl
