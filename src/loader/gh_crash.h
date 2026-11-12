// gh_crash.h -- the console half of the one-button crash report.
//
// The site has had POST /api/crash-report, the CrashReport table and /admin/crashes
// since the first scaffold, and NOTHING in the launcher ever called it. This is that
// caller. It exists so a stranger's failure becomes a compatibility result instead of
// a shrug, which is the only way corpus/compat/compat.json keeps up once people who
// are not us are running games.
//
// ---------------------------------------------------------------------------------
// THE SCRUB IS THE WHOLE POINT OF THIS FILE.
// ---------------------------------------------------------------------------------
// A diag tail is not safe to upload. Measured, not assumed -- launcher_ui.h logs
//
//     ADDON <pkg> signIn -> 1 in 812 ms label='<the Steam account name>'
//
// so the account name is already sitting in the log we would be posting. Other lines
// carry absolute paths, the console's device id, package key ids and serials.
//
// So this is a WHITELIST, at word granularity: a word survives only by matching an
// allowed SHAPE, and anything unrecognised becomes "<x>". That is the opposite of
// listing the secrets we thought of -- the account name above is exactly the kind of
// thing a blacklist misses, because a username looks like any other short word. It
// is caught here not by knowing it, but because QUOTED TEXT IS NEVER ALLOWED: the
// shape 'label=<free text>' cannot survive whatever the free text happens to be.
//
// The classes that survive:
//   number      12  -3.5  0x1f4  512MB  16ms      (hex capped at 15 digits)
//   identifier  LAUNCHER  app_1966720  0.0.939.0  UnityPlayer.dll
//   punctuation ->  ::  ...                       (short runs, no letters)
//   pair        dt=16ms  app=0  err=0x80070002    (both halves must pass)
// Everything else -- quoted strings, emails, long base64/hex runs, URLs with a
// query, anything with an unexpected character -- becomes "<x>".
//
// Paths are reduced to their last component: "D:\games\app_70\hl2.exe" uploads as
// "<path>\hl2.exe". The file name is the diagnostic value; the directory above it is
// the user's machine.
//
// Defence in depth, on top of the whitelist, not instead of it:
//   - AddSensitive() takes strings the launcher knows are identifying (the picker
//     header, i.e. the signed-in account label) and removes them wherever they
//     appear, whatever shape they are in.
//   - A line mentioning a credential at all (cred/token/secret/password/bearer/jwt/
//     refresh) is dropped whole. No such line has ever helped diagnose a game.
//
// Scrub() is deliberately free of Windows types so the host test can link it and run
// it over a real pulled diag. Upload() is the only Win32 part.
#pragma once
// GH_CRASH_SCRUB_ONLY builds the scrub alone, with no Win32 and no loader headers,
// so toolchain/test_gh_crash.cpp can run it over a real pulled diag on the laptop.
// The plan's rule was "write the whitelist first and test it against a real diag
// before this ships", and a scrub that can only be exercised on a console is a
// scrub that does not get exercised.
#ifndef GH_CRASH_SCRUB_ONLY
#include "gh_site.h"
#include "gh_compat.h"
#endif
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace ghcrash {

// The site caps CrashReport.log at 256 KiB (MAX_LOG_BYTES in validation.ts) and
// rejects the request outright past that, so stay under it with room for the rest
// of the JSON envelope. The newest lines are the ones kept: the death is at the end.
inline constexpr size_t kMaxLogBytes = 192 * 1024;
inline constexpr size_t kMaxLineChars = 400;
// How many lines of a consecutive run of the same line we keep before collapsing
// the rest into one marker. Three is enough to see a trend in the numbers.
inline constexpr size_t kKeepRepeat = 3;

// ---- strings the launcher tells us are identifying ---------------------------------
inline std::mutex& SensMu(){ static std::mutex m; return m; }
inline std::vector<std::string>& Sens(){ static std::vector<std::string> v; return v; }

// The picker header (the signed-in account label) is the main one. Short or
// punctuation-only values are ignored: redacting a 2-character string would blank
// half the log and tells us nothing anyway.
inline void AddSensitive(const std::string& s)
{
    if (s.size() < 3) return;
    bool hasAlnum = false;
    for (unsigned char c : s) if (isalnum(c)) { hasAlnum = true; break; }
    if (!hasAlnum) return;
    std::lock_guard<std::mutex> lk(SensMu());
    for (const auto& e : Sens()) if (e == s) return;
    if (Sens().size() < 8) Sens().push_back(s);
}

inline void ClearSensitive(){ std::lock_guard<std::mutex> lk(SensMu()); Sens().clear(); }

// ---- the whitelist -----------------------------------------------------------------
namespace detail {

inline bool IsAlnum(char c){ return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool IsHexDigit(char c){ return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// 12  -3.5  0x1f4  512MB  16ms  1920x1080 is NOT a number (it is an identifier).
inline bool IsNumber(const std::string& w)
{
    if (w.empty() || w.size() > 24) return false;
    size_t i = 0;
    if (w[i] == '+' || w[i] == '-') ++i;
    if (i + 2 < w.size() && w[i] == '0' && (w[i + 1] == 'x' || w[i + 1] == 'X')) {
        i += 2;
        size_t h = 0;
        while (i < w.size() && IsHexDigit(w[i])) { ++i; ++h; }
        // A long hex run is a device id, a key id or a hash, never a quantity.
        if (h == 0 || h > 15) return false;
    } else {
        size_t d = 0, dots = 0;
        while (i < w.size() && ((w[i] >= '0' && w[i] <= '9') || w[i] == '.')) {
            if (w[i] == '.' && ++dots > 1) return false;
            if (w[i] != '.') ++d;
            ++i;
        }
        if (d == 0) return false;
        // A 13+ digit decimal is an identifier, not a quantity -- a SteamID64 is
        // exactly 17 digits, and the host test caught one sailing through as a
        // "number". Epoch seconds (10) and byte counts stay well under this.
        if (d >= 13) return false;
    }
    // An optional short unit: ms, MB, s, KB, fps.
    size_t u = 0;
    while (i < w.size() && IsAlnum(w[i])) { ++i; ++u; }
    return i == w.size() && u <= 4;
}

// LAUNCHER  app_1966720  0.0.939.0  UnityPlayer.dll  GH_ROLE_WARN
inline bool IsIdent(const std::string& w)
{
    if (w.empty() || w.size() > 48) return false;
    if (!(IsAlnum(w[0]) || w[0] == '_')) return false;
    // A bare hex string of 12+ digits is a key id, a device id or a hash. The general
    // 20-run limit below is far too loose for these: the host test leaked a 16-digit
    // repository key id. Checked separately because a word of real LETTERS that long
    // is ordinary ("GPUHEARTBEAT", "CRASHSCREEN") and must survive -- those contain
    // non-hex letters, so testing hex-ness is what separates the two.
    {
        size_t hex = 0;
        bool allHex = true;
        for (char c : w) {
            if (c == '_' || c == '-' || c == '.') continue;
            if (!IsHexDigit(c)) { allHex = false; break; }
            ++hex;
        }
        if (allHex && hex >= 12) return false;
    }
    size_t run = 0;
    for (char c : w) {
        if (IsAlnum(c)) {
            // A 20+ character unbroken alphanumeric run is a token, a hash or a
            // base64 blob. Real log words and dotted versions break well before it.
            if (++run >= 20) return false;
        } else if (c == '_' || c == '-' || c == '.') {
            run = 0;
        } else {
            return false;
        }
    }
    return true;
}

// -> :: ... ; short and letterless.
inline bool IsPunct(const std::string& w)
{
    if (w.empty() || w.size() > 8) return false;
    for (unsigned char c : w) {
        if (IsAlnum((char)c)) return false;
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// "D:\games\app_70\hl2.exe" -> "<path>\hl2.exe"; the directory is the user's machine,
// the file name is the diagnostic. A tail that is not itself a plain name is dropped.
inline std::string Path(const std::string& w)
{
    const size_t p = w.find_last_of("\\/");
    const std::string tail = (p == std::string::npos) ? std::string() : w.substr(p + 1);
    if (!tail.empty() && (IsIdent(tail) || IsNumber(tail))) return "<path>\\" + tail;
    return "<path>";
}

inline bool LooksLikePath(const std::string& w)
{
    if (w.find('\\') != std::string::npos) return true;
    // A single slash is usually a ratio or a fraction in our logs ("3/4 ok"), so a
    // path needs two separated, non-empty segments.
    const size_t a = w.find('/');
    if (a == std::string::npos) return false;
    const size_t b = w.find('/', a + 1);
    return b != std::string::npos || (a > 0 && a + 1 < w.size() && w.find(':') != std::string::npos);
}

inline std::string Core(const std::string& w);

// dt=16ms  app=0  err=0x80070002  label='...' -> label=<x>
inline std::string Pair(const std::string& w)
{
    const size_t e = w.find_first_of("=:");
    if (e == std::string::npos || e == 0 || e + 1 >= w.size()) return std::string();
    // "::" and "C:\..." are not pairs.
    if (w[e] == ':' && (w[e + 1] == ':' || w[e + 1] == '\\' || w[e + 1] == '/')) return std::string();
    const std::string k = w.substr(0, e), v = w.substr(e + 1);
    if (!IsIdent(k)) return std::string();
    return k + w[e] + Core(v);
}

inline std::string Core(const std::string& w)
{
    if (w.empty()) return std::string();
    // Quoted text is free text and never survives. This is what keeps
    // "label='<account name>'" from ever leaving the console.
    if (w.find('\'') != std::string::npos || w.find('"') != std::string::npos) return "<x>";
    if (LooksLikePath(w)) return Path(w);
    if (IsNumber(w) || IsIdent(w) || IsPunct(w)) return w;
    const std::string p = Pair(w);
    if (!p.empty()) return p;
    return "<x>";
}

// Leading/trailing brackets are kept so a line stays readable; the inside is scrubbed.
inline std::string Word(const std::string& w)
{
    size_t a = 0, b = w.size();
    std::string pre, post;
    while (a < b && pre.size() < 3 && (w[a] == '(' || w[a] == '[' || w[a] == '{' || w[a] == '<')) {
        if (w.compare(a, 6, "<path>") == 0) break;
        pre += w[a++];
    }
    // Our own placeholder from an earlier pass. Scrub must be idempotent -- the
    // browser preview shows a stored, already-scrubbed log and the upload scrubs
    // what it is handed again -- and stripping the '<' as a bracket here turned
    // "<path>\x.dll" into "<<path>\x.dll" on every pass.
    if (w.compare(a, 6, "<path>") == 0) {
        const std::string rest = w.substr(a + 6);
        if (rest.empty()) return pre + "<path>";
        if (rest[0] == '\\' || rest[0] == '/') return pre + Word("x" + rest);
        return pre + "<path>" + Word(rest);
    }
    auto endsWithPlaceholder = [&](size_t end) {
        for (const char* ph : { "<x>", "<path>", "<user>" }) {
            const size_t n = strlen(ph);
            if (end >= a + n && w.compare(end - n, n, ph) == 0) return true;
        }
        return false;
    };
    while (b > a && post.size() < 3 && !endsWithPlaceholder(b) &&
           (w[b - 1] == ')' || w[b - 1] == ']' || w[b - 1] == '}' || w[b - 1] == '>' ||
            w[b - 1] == ',' || w[b - 1] == ';' || w[b - 1] == '.' || w[b - 1] == '!' || w[b - 1] == '?'))
        post.insert(post.begin(), w[--b]);
    if (a >= b) return w.size() <= 8 ? w : "<x>";
    return pre + Core(w.substr(a, b - a)) + post;
}

// A line that mentions a credential at all goes entirely. The whitelist above would
// very likely redact the value anyway; this means we do not have to be right about
// that to be safe.
inline bool MentionsCredential(const std::string& lower)
{
    static const char* kWords[] = { "cred", "token", "secret", "password", "bearer", "jwt", "refresh" };
    for (const char* w : kWords) if (lower.find(w) != std::string::npos) return true;
    return false;
}

// The identity a run of repeated lines is collapsed on: the line with every number
// blanked, so two heartbeat samples that differ only in their counters share a key
// while two different file opens or two different fault codes do not. A run of hex
// digits only counts as a number when it follows `0x`, so words keep themselves
// (GPUHEARTBEAT does not become a row of blanks and collide with FRAME).
inline std::string RepeatKey(const std::string& s)
{
    std::string k;
    k.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const bool hexPrefix = (i >= 2 && s[i - 2] == '0' && (s[i - 1] == 'x' || s[i - 1] == 'X'));
        size_t j = i;
        if (hexPrefix) while (j < s.size() && isxdigit((unsigned char)s[j])) ++j;
        else           while (j < s.size() && isdigit((unsigned char)s[j]))  ++j;
        if (j > i) { k.push_back('#'); i = j; continue; }
        k.push_back(s[i]);
        ++i;
    }
    return k;
}

}  // namespace detail

// One line in, one scrubbed line out. Empty means "drop this line".
inline std::string ScrubLine(const std::string& in)
{
    std::string s;
    s.reserve(in.size());
    // Printable ASCII only: a diag carries stack dumps, UTF-8 game text and escapes.
    for (unsigned char c : in) {
        if (s.size() >= kMaxLineChars) break;
        if (c == '\t') s.push_back(' ');
        else if (c >= 0x20 && c <= 0x7E) s.push_back((char)c);
        else s.push_back('?');
    }
    // Identifying strings the launcher handed us, before anything else looks at the
    // line, so they cannot survive inside a shape that would otherwise be allowed.
    {
        std::lock_guard<std::mutex> lk(SensMu());
        for (const auto& sec : Sens()) {
            if (sec.empty()) continue;
            for (size_t p = s.find(sec); p != std::string::npos; p = s.find(sec, p + 6))
                s.replace(p, sec.size(), "<user>");
        }
    }
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return (char)tolower(c); });
    if (detail::MentionsCredential(lower)) return std::string();

    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') { out.push_back(' '); ++i; }
        const size_t b = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > b) out += detail::Word(s.substr(b, i - b));
    }
    return out;
}

// The lines to upload, newest-last, total capped. Lines are taken from the END:
// whatever killed the run is there, and the cap is what the site will accept.
inline std::string Scrub(const std::vector<std::string>& entries)
{
    // One Diag entry can be a whole stack dump, so split on newlines first.
    std::vector<std::string> lines;
    for (const auto& e : entries) {
        size_t a = 0;
        for (;;) {
            const size_t b = e.find('\n', a);
            std::string ln = e.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(std::move(ln));
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
    // Repeated lines are collapsed. Measured on two real 500 KB console diags: the
    // tail is almost entirely `MEMORY sample ...` and friends, so an uncollapsed
    // report spends its whole budget on the heartbeat and never reaches the death.
    // Keep the first kKeepRepeat of a run -- the numbers in them are the trend --
    // then one marker, so the budget buys older lines instead of more of the same.
    std::vector<std::string> keep;
    size_t total = 0;
    std::string runKey;
    size_t runSeen = 0, runOmitted = 0;
    auto pushLine = [&](std::string s) -> bool {
        if (total + s.size() + 1 > kMaxLogBytes) return false;
        total += s.size() + 1;
        keep.push_back(std::move(s));
        return true;
    };
    // Flushes the marker for a finished run. We walk backwards, so in the final
    // (re-reversed) output the marker lands just before the samples it stands for.
    auto flushRun = [&]() -> bool {
        if (!runOmitted) return true;
        const std::string m = "... <" + std::to_string(runOmitted) + " more like this>";
        runOmitted = 0;
        return pushLine(m);
    };
    for (size_t k = lines.size(); k-- > 0; ) {
        std::string s = ScrubLine(lines[k]);
        if (s.empty()) continue;
        const std::string key = detail::RepeatKey(s);
        if (key != runKey) { if (!flushRun()) break; runKey = key; runSeen = 0; }
        ++runSeen;
        if (!key.empty() && runSeen > kKeepRepeat) { ++runOmitted; continue; }
        if (!pushLine(std::move(s))) break;
    }
    flushRun();
    std::string out;
    out.reserve(total);
    for (size_t k = keep.size(); k-- > 0; ) { out += keep[k]; out.push_back('\n'); }
    return out;
}

#ifndef GH_CRASH_SCRUB_ONLY
// ---- the upload --------------------------------------------------------------------
// Result of one attempt, for the screen to show. `ok` means the site stored it.
struct Result {
    bool        ok = false;
    std::string why;        // short, already safe to draw
};

// The shared secret, if this build has one. It gates writes on the site
// (CRASH_INGEST_TOKEN); when the site has none set the endpoint is open and this
// being empty is fine. It is a WRITE-ONLY capability -- it cannot read a report
// back -- so it is the one secret that can live in a client at all.
#ifndef GH_CRASH_TOKEN
#define GH_CRASH_TOKEN ""
#endif

// §65. A game the user uploaded: the report says so, and carries the exe's hash,
// size and architecture -- so reports about the same file group together on the
// site without anyone having to send the file, or its folder's name.
struct Custom {
    bool        on = false;
    std::string exeSha256;          // 64 lowercase hex
    uint64_t    exeSize = 0;
    std::string arch;               // "x64" / "x86"
};

inline Result Upload(const std::string& titleName, const std::vector<std::string>& entries,
                     const std::string& loaderVersion, const Custom& custom = Custom())
{
    Result r;
    const std::string srv = ghsite::Server();
    if (srv.empty()) { r.why = "no site configured"; return r; }
    if (titleName.empty()) { r.why = "no title"; return r; }

    const std::string log = Scrub(entries);
    if (log.empty()) { r.why = "nothing to send"; return r; }

    const std::string body =
        std::string("{\"titleName\":\"") + ghrepo::JsonEsc(titleName) +
        "\",\"log\":\"" + ghrepo::JsonEsc(log) +
        "\",\"greenhouseVersion\":\"" + ghrepo::JsonEsc(loaderVersion) +
        "\",\"consoleModel\":\"" + ghcompat::ModelName(ghcompat::GetModel()) + "\"" +
        (custom.on ? ",\"origin\":\"custom\",\"exeSha256\":\"" + ghrepo::JsonEsc(custom.exeSha256) +
                     "\",\"exeSize\":" + std::to_string((unsigned long long)custom.exeSize) +
                     ",\"exeArch\":\"" + ghrepo::JsonEsc(custom.arch) + "\""
                   : std::string()) + "}";

    std::wstring hdr = L"Content-Type: application/json\r\n";
    const std::string tok = GH_CRASH_TOKEN;
    if (!tok.empty()) hdr += L"Authorization: Bearer " + ghrepo::Widen(tok) + L"\r\n";

    std::string resp;
    DWORD status = 0;
    std::string why;
    const bool sent = ghrepo::HttpRequest(L"POST", srv + "/api/crash-report", hdr, body,
                                          16 * 1024, &resp, &status, why);
    if (!sent) { r.why = why.empty() ? "could not reach the site" : why; return r; }
    if (status == 201) { r.ok = true; r.why = "sent"; return r; }
    if (status == 401) { r.why = "the site refused this build's token"; return r; }
    if (status == 429) { r.why = "too many reports just now -- try later"; return r; }
    if (status == 413) { r.why = "report too large"; return r; }
    r.why = "the site answered " + std::to_string((unsigned long)status);
    return r;
}

#endif  // GH_CRASH_SCRUB_ONLY

}  // namespace ghcrash
