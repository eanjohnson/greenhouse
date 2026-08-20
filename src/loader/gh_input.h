// gh_input.h -- pad → keyboard/mouse profiles (Steam Input-style, overlay-owned).
//
// Header-only, included once from guest_loader.cpp (GH_LAUNCHER) after the
// input beds exist (g_padState, ApplyMouseSample, KM_Push, g_vkDown) and after
// ReadWholeFile / EnsureRealDir. Overlay UI in launcher_ui.h calls the GhInput*
// API; the UI thread pump calls GhInputApply() in place of OverlayPadToMouse.
#pragma once

enum {
    GH_SRC_A = 0, GH_SRC_B, GH_SRC_X, GH_SRC_Y,
    GH_SRC_LB, GH_SRC_RB,
    GH_SRC_L3, GH_SRC_R3,
    GH_SRC_DU, GH_SRC_DD, GH_SRC_DL, GH_SRC_DR,
    GH_SRC_START, GH_SRC_BACK,
    GH_SRC_LT, GH_SRC_RT,
    GH_SRC_LS, GH_SRC_RS,
    GH_SRC_COUNT
};

enum {
    GH_TGT_NONE = 0,
    GH_TGT_MOUSE_LEFT,
    GH_TGT_MOUSE_RIGHT,
    GH_TGT_MOUSE_MIDDLE,
    GH_TGT_WHEEL_UP,
    GH_TGT_WHEEL_DOWN,
    GH_TGT_MOUSE,     // analog stick → cursor
    GH_TGT_WASD,
    GH_TGT_ARROWS,
    GH_TGT_KEY        // extra = virtual-key
};

struct GhBind {
    int kind = GH_TGT_NONE;
    int vk   = 0;
};

struct GhInputProfile {
    char     id[32]   = {};
    wchar_t  name[32] = {};
    int      sens     = 100;   // 25–200, 100 = original OverlayPadToMouse scale
    int      invertY  = 0;
    int      dead     = 8000;
    GhBind   bind[GH_SRC_COUNT]{};
};

struct GhProfileInfo {
    char     id[32]   = {};
    wchar_t  name[32] = {};
    bool     builtin  = false;
};

struct GhPadSwallow {
    WORD buttons = 0;
    bool ls = false, rs = false, lt = false, rt = false;
};

static GhInputProfile g_ghProf{};
static uint32_t       g_ghGameKey = 0;
static bool           g_ghProfOn  = false;  // applied this session (mirrors g_kmRemapOn)
static bool           g_synVk[256] = {};
static unsigned       g_synMouse   = 0;

static const wchar_t* GhSrcLabel(int src)
{
    switch (src) {
        case GH_SRC_A:     return L"A button";
        case GH_SRC_B:     return L"B button";
        case GH_SRC_X:     return L"X button";
        case GH_SRC_Y:     return L"Y button";
        case GH_SRC_LB:    return L"Left bumper";
        case GH_SRC_RB:    return L"Right bumper";
        case GH_SRC_L3:    return L"LS click";
        case GH_SRC_R3:    return L"RS click";
        case GH_SRC_DU:    return L"D-pad up";
        case GH_SRC_DD:    return L"D-pad down";
        case GH_SRC_DL:    return L"D-pad left";
        case GH_SRC_DR:    return L"D-pad right";
        case GH_SRC_START: return L"Start";
        case GH_SRC_BACK:  return L"Back";
        case GH_SRC_LT:    return L"Left trigger";
        case GH_SRC_RT:    return L"Right trigger";
        case GH_SRC_LS:    return L"Left stick";
        case GH_SRC_RS:    return L"Right stick";
        default:           return L"?";
    }
}

static const char* GhSrcTok(int src)
{
    static const char* t[GH_SRC_COUNT] = {
        "A","B","X","Y","LB","RB","L3","R3",
        "DU","DD","DL","DR","START","BACK","LT","RT","LS","RS"
    };
    return (src >= 0 && src < GH_SRC_COUNT) ? t[src] : "?";
}

static int GhSrcFromTok(const char* s)
{
    if (!s || !s[0]) return -1;
    for (int i = 0; i < GH_SRC_COUNT; ++i)
        if (_stricmp(s, GhSrcTok(i)) == 0) return i;
    return -1;
}

static bool GhSrcIsAnalog(int src)
{
    return src == GH_SRC_LS || src == GH_SRC_RS;
}

static void GhTgtLabel(const GhBind& b, wchar_t* out, int cap)
{
    if (!out || cap < 2) return;
    out[0] = 0;
    switch (b.kind) {
        case GH_TGT_NONE:         wcsncpy(out, L"—", cap - 1); break;
        case GH_TGT_MOUSE_LEFT:   wcsncpy(out, L"Left mouse", cap - 1); break;
        case GH_TGT_MOUSE_RIGHT:  wcsncpy(out, L"Right mouse", cap - 1); break;
        case GH_TGT_MOUSE_MIDDLE: wcsncpy(out, L"Middle mouse", cap - 1); break;
        case GH_TGT_WHEEL_UP:     wcsncpy(out, L"Wheel up", cap - 1); break;
        case GH_TGT_WHEEL_DOWN:   wcsncpy(out, L"Wheel down", cap - 1); break;
        case GH_TGT_MOUSE:        wcsncpy(out, L"Mouse", cap - 1); break;
        case GH_TGT_WASD:         wcsncpy(out, L"WASD", cap - 1); break;
        case GH_TGT_ARROWS:       wcsncpy(out, L"Arrows", cap - 1); break;
        case GH_TGT_KEY: {
            int vk = b.vk;
            if (vk >= 'A' && vk <= 'Z') { out[0] = (wchar_t)vk; out[1] = 0; }
            else if (vk >= '0' && vk <= '9') { out[0] = (wchar_t)vk; out[1] = 0; }
            else {
                const wchar_t* n = nullptr;
                switch (vk) {
                    case VK_SPACE: n = L"Space"; break;
                    case VK_RETURN: n = L"Enter"; break;
                    case VK_TAB: n = L"Tab"; break;
                    case VK_ESCAPE: n = L"Esc"; break;
                    case VK_BACK: n = L"Backspace"; break;
                    case VK_SHIFT: n = L"Shift"; break;
                    case VK_CONTROL: n = L"Ctrl"; break;
                    case VK_MENU: n = L"Alt"; break;
                    case VK_CAPITAL: n = L"Caps"; break;
                    case VK_LEFT: n = L"Left"; break;
                    case VK_RIGHT: n = L"Right"; break;
                    case VK_UP: n = L"Up"; break;
                    case VK_DOWN: n = L"Down"; break;
                    case VK_OEM_3: n = L"`"; break;
                    case VK_OEM_MINUS: n = L"-"; break;
                    case VK_OEM_PLUS: n = L"="; break;
                    case VK_OEM_4: n = L"["; break;
                    case VK_OEM_6: n = L"]"; break;
                    case VK_OEM_1: n = L";"; break;
                    case VK_OEM_7: n = L"'"; break;
                    case VK_OEM_COMMA: n = L","; break;
                    case VK_OEM_PERIOD: n = L"."; break;
                    case VK_OEM_2: n = L"/"; break;
                    default:
                        if (vk >= VK_F1 && vk <= VK_F12) {
                            swprintf(out, cap, L"F%d", vk - VK_F1 + 1);
                            return;
                        }
                        swprintf(out, cap, L"VK %d", vk);
                        return;
                }
                wcsncpy(out, n, cap - 1);
            }
            break;
        }
        default: wcsncpy(out, L"?", cap - 1); break;
    }
    out[cap - 1] = 0;
}

static std::wstring GhInputDir()
{
    return std::wstring(kLocalState) + L"gh_input\\";
}
static std::wstring GhInputProfDir()
{
    return GhInputDir() + L"profiles\\";
}
static std::wstring GhInputActivePath()
{
    return GhInputDir() + L"active.txt";
}
static std::wstring GhInputProfPath(const char* id)
{
    std::wstring w = GhInputProfDir();
    for (const char* p = id; p && *p; ++p) w.push_back((wchar_t)(unsigned char)*p);
    w += L".txt";
    return w;
}

static bool GhInputIsBuiltinId(const char* id)
{
    return id && (strcmp(id, "builtin-off") == 0 ||
                  strcmp(id, "builtin-mouse") == 0 ||
                  strcmp(id, "builtin-wasd") == 0);
}

static void GhInputClearBinds(GhInputProfile& p)
{
    for (int i = 0; i < GH_SRC_COUNT; ++i) { p.bind[i].kind = GH_TGT_NONE; p.bind[i].vk = 0; }
}

static void GhInputMakeOff(GhInputProfile& p)
{
    memset(&p, 0, sizeof(p));
    strncpy(p.id, "builtin-off", 31);
    wcsncpy(p.name, L"Off", 31);
    p.sens = 100; p.dead = 8000;
}

static void GhInputMakeMouse(GhInputProfile& p)
{
    GhInputMakeOff(p);
    strncpy(p.id, "builtin-mouse", 31);
    wcsncpy(p.name, L"Mouse", 31);
    p.bind[GH_SRC_A].kind  = GH_TGT_MOUSE_LEFT;
    p.bind[GH_SRC_B].kind  = GH_TGT_MOUSE_RIGHT;
    p.bind[GH_SRC_RT].kind = GH_TGT_WHEEL_UP;
    p.bind[GH_SRC_LT].kind = GH_TGT_WHEEL_DOWN;
    p.bind[GH_SRC_RS].kind = GH_TGT_MOUSE;
}

static void GhInputMakeWasd(GhInputProfile& p)
{
    GhInputMakeOff(p);
    strncpy(p.id, "builtin-wasd", 31);
    wcsncpy(p.name, L"WASD + Mouse", 31);
    p.bind[GH_SRC_A].kind  = GH_TGT_MOUSE_LEFT;
    p.bind[GH_SRC_B].kind  = GH_TGT_MOUSE_RIGHT;
    p.bind[GH_SRC_X].kind  = GH_TGT_KEY; p.bind[GH_SRC_X].vk = 'R';
    p.bind[GH_SRC_Y].kind  = GH_TGT_KEY; p.bind[GH_SRC_Y].vk = 'E';
    p.bind[GH_SRC_LB].kind = GH_TGT_KEY; p.bind[GH_SRC_LB].vk = 'Q';
    p.bind[GH_SRC_RB].kind = GH_TGT_KEY; p.bind[GH_SRC_RB].vk = 'F';
    p.bind[GH_SRC_LT].kind = GH_TGT_KEY; p.bind[GH_SRC_LT].vk = 'C';
    p.bind[GH_SRC_RT].kind = GH_TGT_KEY; p.bind[GH_SRC_RT].vk = VK_SPACE;
    p.bind[GH_SRC_LS].kind = GH_TGT_WASD;
    p.bind[GH_SRC_RS].kind = GH_TGT_MOUSE;
}

static bool GhInputBuiltinById(const char* id, GhInputProfile& p)
{
    if (!id) return false;
    if (strcmp(id, "builtin-off") == 0)   { GhInputMakeOff(p);   return true; }
    if (strcmp(id, "builtin-mouse") == 0) { GhInputMakeMouse(p); return true; }
    if (strcmp(id, "builtin-wasd") == 0)  { GhInputMakeWasd(p);  return true; }
    return false;
}

static bool GhInputWriteBytes(const std::wstring& path, const std::string& body)
{
    EnsureParentOf(path);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
    CloseHandle(h);
    return ok != 0;
}

static int GhTgtFromTok(const char* t, int* vkOut)
{
    if (!t) return GH_TGT_NONE;
    if (_stricmp(t, "none") == 0)        return GH_TGT_NONE;
    if (_stricmp(t, "mouse_left") == 0)  return GH_TGT_MOUSE_LEFT;
    if (_stricmp(t, "mouse_right") == 0) return GH_TGT_MOUSE_RIGHT;
    if (_stricmp(t, "mouse_middle") == 0)return GH_TGT_MOUSE_MIDDLE;
    if (_stricmp(t, "wheel_up") == 0)    return GH_TGT_WHEEL_UP;
    if (_stricmp(t, "wheel_down") == 0)  return GH_TGT_WHEEL_DOWN;
    if (_stricmp(t, "mouse") == 0)       return GH_TGT_MOUSE;
    if (_stricmp(t, "wasd") == 0)        return GH_TGT_WASD;
    if (_stricmp(t, "arrows") == 0)      return GH_TGT_ARROWS;
    if (_strnicmp(t, "key", 3) == 0) {
        const char* p = t + 3;
        while (*p == ' ' || *p == '=' || *p == '_') ++p;
        int vk = 0;
        if (p[0] && !p[1] && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')))
            vk = (int)(unsigned char)toupper((unsigned char)p[0]);
        else if (p[0] && !p[1] && p[0] >= '0' && p[0] <= '9')
            vk = (int)p[0];
        else
            vk = (int)strtol(p, nullptr, 0);
        if (vkOut) *vkOut = vk;
        return GH_TGT_KEY;
    }
    return GH_TGT_NONE;
}

static const char* GhTgtTok(const GhBind& b)
{
    switch (b.kind) {
        case GH_TGT_MOUSE_LEFT:   return "mouse_left";
        case GH_TGT_MOUSE_RIGHT:  return "mouse_right";
        case GH_TGT_MOUSE_MIDDLE: return "mouse_middle";
        case GH_TGT_WHEEL_UP:     return "wheel_up";
        case GH_TGT_WHEEL_DOWN:   return "wheel_down";
        case GH_TGT_MOUSE:        return "mouse";
        case GH_TGT_WASD:         return "wasd";
        case GH_TGT_ARROWS:       return "arrows";
        default:                  return "none";
    }
}

static bool GhInputParse(const std::string& text, GhInputProfile& p)
{
    GhInputMakeOff(p);
    p.id[0] = 0; p.name[0] = 0;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find(' ');
        std::string k = (sp == std::string::npos) ? line : line.substr(0, sp);
        std::string v = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
        if (k == "id") {
            strncpy(p.id, v.c_str(), 31);
        } else if (k == "name") {
            MultiByteToWideChar(CP_UTF8, 0, v.c_str(), -1, p.name, 32);
            p.name[31] = 0;
        } else if (k == "sens") {
            p.sens = atoi(v.c_str());
            if (p.sens < 25) p.sens = 25;
            if (p.sens > 200) p.sens = 200;
        } else if (k == "invert") {
            p.invertY = (atoi(v.c_str()) != 0);
        } else if (k == "dead") {
            p.dead = atoi(v.c_str());
            if (p.dead < 0) p.dead = 0;
            if (p.dead > 30000) p.dead = 30000;
        } else if (k == "bind") {
            size_t sp2 = v.find(' ');
            std::string src = (sp2 == std::string::npos) ? v : v.substr(0, sp2);
            std::string rest = (sp2 == std::string::npos) ? std::string() : v.substr(sp2 + 1);
            int si = GhSrcFromTok(src.c_str());
            if (si < 0) continue;
            int vk = 0;
            int kind = GhTgtFromTok(rest.c_str(), &vk);
            p.bind[si].kind = kind;
            p.bind[si].vk = (kind == GH_TGT_KEY) ? vk : 0;
        }
    }
    return p.id[0] != 0;
}

static std::string GhInputSerialize(const GhInputProfile& p)
{
    std::string s;
    char buf[128];
    snprintf(buf, sizeof(buf), "id %s\n", p.id);
    s += buf;
    char utf[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, p.name, -1, utf, 128, nullptr, nullptr);
    s += "name "; s += utf; s += "\n";
    snprintf(buf, sizeof(buf), "sens %d\ninvert %d\ndead %d\n", p.sens, p.invertY ? 1 : 0, p.dead);
    s += buf;
    for (int i = 0; i < GH_SRC_COUNT; ++i) {
        if (p.bind[i].kind == GH_TGT_NONE) continue;
        if (p.bind[i].kind == GH_TGT_KEY) {
            snprintf(buf, sizeof(buf), "bind %s key %d\n", GhSrcTok(i), p.bind[i].vk);
        } else {
            snprintf(buf, sizeof(buf), "bind %s %s\n", GhSrcTok(i), GhTgtTok(p.bind[i]));
        }
        s += buf;
    }
    return s;
}

static bool GhInputLoadFile(const char* id, GhInputProfile& p)
{
    if (GhInputBuiltinById(id, p)) return true;
    std::vector<uint8_t> b;
    if (!ReadWholeFile(GhInputProfPath(id), b) || b.empty()) return false;
    std::string t((char*)b.data(), b.size());
    if (!GhInputParse(t, p)) return false;
    strncpy(p.id, id, 31);
    return true;
}

static bool GhInputSaveProfile(const GhInputProfile& p)
{
    if (GhInputIsBuiltinId(p.id)) return true;
    return GhInputWriteBytes(GhInputProfPath(p.id), GhInputSerialize(p));
}

static uint32_t GhInputHashDir(const wchar_t* dir)
{
    if (!dir || !dir[0]) return 1;
    const wchar_t* last = dir;
    for (const wchar_t* p = dir; *p; ++p)
        if (*p == L'\\' || *p == L'/') last = p + 1;
    uint32_t h = 2166136261u;
    for (const wchar_t* p = last; *p; ++p) {
        wchar_t c = *p;
        if (c == L'\\' || c == L'/') break;
        h ^= (uint32_t)towlower(c);
        h *= 16777619u;
    }
    if (h == 0) h = 1;
    return h;
}

static void GhInputNewId(char* out, int cap)
{
    static volatile long seq = 0;
    unsigned n = (unsigned)InterlockedIncrement(&seq);
    snprintf(out, cap, "%08x%08x", (unsigned)GetTickCount(), n);
}

static bool GhInputReadActive(uint32_t key, char* idOut, int idCap, int* onOut)
{
    if (idOut && idCap) idOut[0] = 0;
    if (onOut) *onOut = 0;
    std::vector<uint8_t> b;
    if (!ReadWholeFile(GhInputActivePath(), b) || b.empty()) return false;
    std::string t((char*)b.data(), b.size());
    size_t i = 0;
    char found[32] = {};
    int on = 0;
    bool hit = false;
    while (i < t.size()) {
        size_t e = t.find('\n', i);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(i, e - i);
        i = e + 1;
        unsigned long k = 0; char id[32] = {}; int en = 1;
        if (sscanf(line.c_str(), "%lu %31s %d", &k, id, &en) < 2) continue;
        if ((uint32_t)k != key) continue;
        strncpy(found, id, 31);
        on = en;
        hit = true;
    }
    if (!hit) return false;
    if (idOut) strncpy(idOut, found, idCap - 1);
    if (onOut) *onOut = on;
    return true;
}

static void GhInputWriteActive(uint32_t key, const char* id, int on)
{
    std::vector<uint8_t> b;
    std::string t;
    if (ReadWholeFile(GhInputActivePath(), b) && !b.empty())
        t.assign((char*)b.data(), b.size());
    std::string out;
    size_t i = 0;
    bool replaced = false;
    while (i < t.size()) {
        size_t e = t.find('\n', i);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(i, e - i);
        i = e + 1;
        unsigned long k = 0; char dummy[32] = {};
        if (sscanf(line.c_str(), "%lu %31s", &k, dummy) >= 2 && (uint32_t)k == key) {
            char buf[80];
            snprintf(buf, sizeof(buf), "%u %s %d\n", key, id, on);
            out += buf;
            replaced = true;
        } else if (!line.empty()) {
            out += line;
            if (out.empty() || out.back() != '\n') out += '\n';
        }
    }
    if (!replaced) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%u %s %d\n", key, id, on);
        out += buf;
    }
    GhInputWriteBytes(GhInputActivePath(), out);
}

static void GhInputApplyEnabled(bool on)
{
    g_ghProfOn = on;
    g_kmRemapOn = on;
}

static void GhInputSyncCursor(bool on)
{
    std::lock_guard<std::mutex> lk(g_kmMutex);
    if (on) {
        g_kmCurX = (LONG)g_winW / 2;
        g_kmCurY = (LONG)g_winH / 2;
        g_kmActive = true;
    } else {
        g_kmActive = false;
    }
}

static void GhInputReleaseAll()
{
    if (g_synMouse) {
        ApplyMouseSample(0, 0, 0, 0);
        g_synMouse = 0;
    }
    for (int vk = 1; vk < 256; ++vk) {
        if (!g_synVk[vk]) continue;
        g_synVk[vk] = false;
        {
            std::lock_guard<std::mutex> lk(g_kmMutex);
            g_vkDown[vk] = false;
        }
        LPARAM lp = 1 | ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
        KM_Push(WM_KEYUP, (WPARAM)vk, lp);
    }
}

static void GhInputSetSynKey(int vk, bool down)
{
    if (vk <= 0 || vk > 255) return;
    if (down) {
        if (g_synVk[vk]) return;
        bool was = false;
        {
            std::lock_guard<std::mutex> lk(g_kmMutex);
            was = g_vkDown[vk];
            g_vkDown[vk] = true;
            g_kmActive = true;
        }
        g_synVk[vk] = true;
        if (!was) KM_Push(WM_KEYDOWN, (WPARAM)vk, 1);
    } else {
        if (!g_synVk[vk]) return;
        g_synVk[vk] = false;
        {
            std::lock_guard<std::mutex> lk(g_kmMutex);
            g_vkDown[vk] = false;
        }
        LPARAM lp = 1 | ((LPARAM)1 << 30) | ((LPARAM)1 << 31);
        KM_Push(WM_KEYUP, (WPARAM)vk, lp);
    }
}

static short GhInputClampS(int v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return (short)-32768;
    return (short)v;
}

static void GhInputApply()
{
    if (!g_guestRunning || !g_kmRemapOn || g_overlayOpen) {
        GhInputReleaseAll();
        return;
    }
    XINPUT_GAMEPAD g{};
    { std::lock_guard<std::mutex> lk(g_padMutex); g = g_padState; }

    const bool chord = (g.wButtons & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) ==
                       (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK);

    unsigned mouse = 0;
    short dx = 0, dy = 0, wheel = 0;
    bool wantMouse = false;
    bool wantKey[256] = {};

    auto down = [&](int src) -> bool {
        if (chord && (src == GH_SRC_START || src == GH_SRC_BACK)) return false;
        switch (src) {
            case GH_SRC_A:     return (g.wButtons & XINPUT_GAMEPAD_A) != 0;
            case GH_SRC_B:     return (g.wButtons & XINPUT_GAMEPAD_B) != 0;
            case GH_SRC_X:     return (g.wButtons & XINPUT_GAMEPAD_X) != 0;
            case GH_SRC_Y:     return (g.wButtons & XINPUT_GAMEPAD_Y) != 0;
            case GH_SRC_LB:    return (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            case GH_SRC_RB:    return (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
            case GH_SRC_L3:    return (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            case GH_SRC_R3:    return (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
            case GH_SRC_DU:    return (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            case GH_SRC_DD:    return (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            case GH_SRC_DL:    return (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            case GH_SRC_DR:    return (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            case GH_SRC_START: return (g.wButtons & XINPUT_GAMEPAD_START) != 0;
            case GH_SRC_BACK:  return (g.wButtons & XINPUT_GAMEPAD_BACK) != 0;
            case GH_SRC_LT:    return g.bLeftTrigger > 40;
            case GH_SRC_RT:    return g.bRightTrigger > 40;
            default:           return false;
        }
    };

    auto applyTgt = [&](const GhBind& b, bool held) {
        if (!held) return;
        switch (b.kind) {
            case GH_TGT_MOUSE_LEFT:   mouse |= 1; wantMouse = true; break;
            case GH_TGT_MOUSE_RIGHT:  mouse |= 2; wantMouse = true; break;
            case GH_TGT_MOUSE_MIDDLE: mouse |= 4; wantMouse = true; break;
            case GH_TGT_WHEEL_UP:     wheel = (short)(wheel + 1); wantMouse = true; break;
            case GH_TGT_WHEEL_DOWN:   wheel = (short)(wheel - 1); wantMouse = true; break;
            case GH_TGT_KEY:
                if (b.vk > 0 && b.vk < 256) wantKey[b.vk] = true;
                break;
            default: break;
        }
    };

    for (int s = 0; s < GH_SRC_LS; ++s)
        applyTgt(g_ghProf.bind[s], down(s));

    auto applyStick = [&](int src, int sx, int sy) {
        const GhBind& b = g_ghProf.bind[src];
        int dead = g_ghProf.dead;
        int ax = sx, ay = sy;
        if (ax > -dead && ax < dead) ax = 0;
        if (ay > -dead && ay < dead) ay = 0;
        if (b.kind == GH_TGT_MOUSE) {
            int sens = g_ghProf.sens;
            if (sens < 25) sens = 100;
            int mdx = ax * 28 * sens / 100 / 32767;
            int mdy = -ay * 28 * sens / 100 / 32767;
            if (g_ghProf.invertY) mdy = -mdy;
            dx = GhInputClampS((int)dx + mdx);
            dy = GhInputClampS((int)dy + mdy);
            wantMouse = true;
        } else if (b.kind == GH_TGT_WASD || b.kind == GH_TGT_ARROWS) {
            int up, dn, lf, rt;
            if (b.kind == GH_TGT_WASD) { up = 'W'; dn = 'S'; lf = 'A'; rt = 'D'; }
            else { up = VK_UP; dn = VK_DOWN; lf = VK_LEFT; rt = VK_RIGHT; }
            if (ay >  dead) wantKey[up] = true;
            if (ay < -dead) wantKey[dn] = true;
            if (ax < -dead) wantKey[lf] = true;
            if (ax >  dead) wantKey[rt] = true;
        } else {
            applyTgt(b, ax || ay); // digital-on-deflect, unused in templates
        }
    };
    applyStick(GH_SRC_LS, g.sThumbLX, g.sThumbLY);
    applyStick(GH_SRC_RS, g.sThumbRX, g.sThumbRY);

    for (int vk = 1; vk < 256; ++vk) {
        if (wantKey[vk]) GhInputSetSynKey(vk, true);
        else if (g_synVk[vk]) GhInputSetSynKey(vk, false);
    }

    if (wantMouse || g_synMouse || mouse || dx || dy || wheel)
        ApplyMouseSample(mouse, dx, dy, wheel);
    g_synMouse = mouse;
}

static GhPadSwallow GhInputSwallow()
{
    GhPadSwallow s{};
    if (!g_kmRemapOn) return s;
    auto bound = [](int src) { return g_ghProf.bind[src].kind != GH_TGT_NONE; };
    if (bound(GH_SRC_A))     s.buttons |= XINPUT_GAMEPAD_A;
    if (bound(GH_SRC_B))     s.buttons |= XINPUT_GAMEPAD_B;
    if (bound(GH_SRC_X))     s.buttons |= XINPUT_GAMEPAD_X;
    if (bound(GH_SRC_Y))     s.buttons |= XINPUT_GAMEPAD_Y;
    if (bound(GH_SRC_LB))    s.buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (bound(GH_SRC_RB))    s.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (bound(GH_SRC_L3))    s.buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (bound(GH_SRC_R3))    s.buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (bound(GH_SRC_DU))    s.buttons |= XINPUT_GAMEPAD_DPAD_UP;
    if (bound(GH_SRC_DD))    s.buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (bound(GH_SRC_DL))    s.buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (bound(GH_SRC_DR))    s.buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (bound(GH_SRC_START)) s.buttons |= XINPUT_GAMEPAD_START;
    if (bound(GH_SRC_BACK))  s.buttons |= XINPUT_GAMEPAD_BACK;
    s.lt = bound(GH_SRC_LT);
    s.rt = bound(GH_SRC_RT);
    s.ls = bound(GH_SRC_LS);
    s.rs = bound(GH_SRC_RS);
    return s;
}

static void GhInputMaskXInput(XINPUT_GAMEPAD* g)
{
    if (!g || !g_kmRemapOn) return;
    GhPadSwallow s = GhInputSwallow();
    g->wButtons = (WORD)(g->wButtons & ~s.buttons);
    if (s.ls) { g->sThumbLX = 0; g->sThumbLY = 0; }
    if (s.rs) { g->sThumbRX = 0; g->sThumbRY = 0; }
    if (s.lt) g->bLeftTrigger = 0;
    if (s.rt) g->bRightTrigger = 0;
}

static void GhInputMaskWgi(GhWgiPadReading* out)
{
    if (!out || !g_kmRemapOn) return;
    GhPadSwallow s = GhInputSwallow();
    if (s.buttons & XINPUT_GAMEPAD_START)           out->Buttons &= ~0x1u;
    if (s.buttons & XINPUT_GAMEPAD_BACK)            out->Buttons &= ~0x2u;
    if (s.buttons & XINPUT_GAMEPAD_A)               out->Buttons &= ~0x4u;
    if (s.buttons & XINPUT_GAMEPAD_B)               out->Buttons &= ~0x8u;
    if (s.buttons & XINPUT_GAMEPAD_X)               out->Buttons &= ~0x10u;
    if (s.buttons & XINPUT_GAMEPAD_Y)               out->Buttons &= ~0x20u;
    if (s.buttons & XINPUT_GAMEPAD_DPAD_UP)         out->Buttons &= ~0x40u;
    if (s.buttons & XINPUT_GAMEPAD_DPAD_DOWN)       out->Buttons &= ~0x80u;
    if (s.buttons & XINPUT_GAMEPAD_DPAD_LEFT)       out->Buttons &= ~0x100u;
    if (s.buttons & XINPUT_GAMEPAD_DPAD_RIGHT)      out->Buttons &= ~0x200u;
    if (s.buttons & XINPUT_GAMEPAD_LEFT_SHOULDER)   out->Buttons &= ~0x400u;
    if (s.buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER)  out->Buttons &= ~0x800u;
    if (s.buttons & XINPUT_GAMEPAD_LEFT_THUMB)      out->Buttons &= ~0x1000u;
    if (s.buttons & XINPUT_GAMEPAD_RIGHT_THUMB)     out->Buttons &= ~0x2000u;
    if (s.lt) out->LeftTrigger = 0;
    if (s.rt) out->RightTrigger = 0;
    if (s.ls) { out->LeftThumbstickX = 0; out->LeftThumbstickY = 0; }
    if (s.rs) { out->RightThumbstickX = 0; out->RightThumbstickY = 0; }
}

static void GhInputBoot()
{
    EnsureRealDir(GhInputProfDir());
    if (g_ghProf.id[0] == 0) GhInputMakeOff(g_ghProf);
}

static void GhInputRemember()
{
    if (!g_ghGameKey) return;
    GhInputWriteActive(g_ghGameKey, g_ghProf.id, g_kmRemapOn ? 1 : 0);
}

static bool GhInputSelectId(const char* id, bool persist)
{
    GhInputProfile p{};
    if (!GhInputLoadFile(id, p)) return false;
    g_ghProf = p;
    bool on = (strcmp(p.id, "builtin-off") != 0);
    bool was = (bool)g_kmRemapOn;
    GhInputApplyEnabled(on);
    if (on && !was) {
        GhInputSyncCursor(true);
        LONG x, y;
        { std::lock_guard<std::mutex> lk(g_kmMutex); x = g_kmCurX; y = g_kmCurY; }
        KM_Push(WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    } else if (!on && was) {
        GhInputReleaseAll();
        GhInputSyncCursor(false);
    }
    if (persist) GhInputRemember();
    Diag(std::string("INPUT profile ") + p.id + (on ? " on" : " off"));
    return true;
}

static void GhInputOnGuestStart(uint32_t app, const wchar_t* guestDir)
{
    GhInputBoot();
    g_ghGameKey = app ? app : GhInputHashDir(guestDir);
    char id[32] = {};
    int on = 0;
    if (GhInputReadActive(g_ghGameKey, id, 32, &on) && id[0]) {
        if (!GhInputSelectId(id, false)) GhInputSelectId("builtin-mouse", false);
        GhInputApplyEnabled(on != 0);
        if (!on) { GhInputReleaseAll(); GhInputSyncCursor(false); }
        Diag(std::string("INPUT load game=") + std::to_string(g_ghGameKey) +
             " id=" + g_ghProf.id + (on ? " on" : " off"));
        return;
    }
    std::vector<uint8_t> b;
    bool legacyOn = false;
    if (ReadWholeFile(std::wstring(kLocalState) + L"gh_remap.txt", b) && !b.empty())
        legacyOn = (b[0] == '1' || b[0] == 't' || b[0] == 'T');
    GhInputSelectId(legacyOn ? "builtin-mouse" : "builtin-off", true);
}

static void GhInputSetEnabled(bool on)
{
    if ((bool)g_kmRemapOn == on) return;
    if (on) {
        if (strcmp(g_ghProf.id, "builtin-off") == 0)
            GhInputSelectId("builtin-mouse", true);
        else {
            GhInputApplyEnabled(true);
            GhInputSyncCursor(true);
            LONG x, y;
            { std::lock_guard<std::mutex> lk(g_kmMutex); x = g_kmCurX; y = g_kmCurY; }
            KM_Push(WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            GhInputRemember();
        }
    } else {
        GhInputApplyEnabled(false);
        GhInputReleaseAll();
        GhInputSyncCursor(false);
        GhInputRemember();
    }
    Diag(on ? "REMAP on" : "REMAP off");
}

static bool GhInputForkIfBuiltin()
{
    if (!GhInputIsBuiltinId(g_ghProf.id)) return false;
    if (strcmp(g_ghProf.id, "builtin-off") == 0) return false;
    char nid[32];
    GhInputNewId(nid, 32);
    strncpy(g_ghProf.id, nid, 31);
    wchar_t nm[32];
    wcsncpy(nm, g_ghProf.name, 31); nm[31] = 0;
    // keep the template name; user can rename
    (void)nm;
    GhInputSaveProfile(g_ghProf);
    GhInputRemember();
    Diag(std::string("INPUT forked ") + g_ghProf.id);
    return true;
}

static bool GhInputDuplicate()
{
    GhInputProfile p = g_ghProf;
    GhInputNewId(p.id, 32);
    wchar_t nm[32];
    swprintf(nm, 32, L"%.20s (copy)", g_ghProf.name[0] ? g_ghProf.name : L"Profile");
    wcsncpy(p.name, nm, 31); p.name[31] = 0;
    if (!GhInputSaveProfile(p)) return false;
    g_ghProf = p;
    GhInputApplyEnabled(true);
    GhInputRemember();
    Diag(std::string("INPUT dup ") + p.id);
    return true;
}

static bool GhInputNewFrom(const char* builtinId)
{
    GhInputProfile p{};
    if (!GhInputBuiltinById(builtinId, p)) GhInputMakeMouse(p);
    GhInputNewId(p.id, 32);
    if (wcscmp(p.name, L"Off") == 0) wcsncpy(p.name, L"Profile", 31);
    if (!GhInputSaveProfile(p)) return false;
    g_ghProf = p;
    GhInputApplyEnabled(true);
    GhInputRemember();
    return true;
}

static bool GhInputDeleteCurrent()
{
    if (GhInputIsBuiltinId(g_ghProf.id)) return false;
    std::wstring path = GhInputProfPath(g_ghProf.id);
    DeleteFileW(path.c_str());
    GhInputSelectId("builtin-mouse", true);
    return true;
}

static void GhInputRename(const wchar_t* name)
{
    if (!name || !name[0]) return;
    GhInputForkIfBuiltin();
    wcsncpy(g_ghProf.name, name, 31);
    g_ghProf.name[31] = 0;
    GhInputSaveProfile(g_ghProf);
}

static void GhInputSetBind(int src, int kind, int vk)
{
    if (src < 0 || src >= GH_SRC_COUNT) return;
    GhInputForkIfBuiltin();
    g_ghProf.bind[src].kind = kind;
    g_ghProf.bind[src].vk = (kind == GH_TGT_KEY) ? vk : 0;
    GhInputSaveProfile(g_ghProf);
    GhInputRemember();
}

static void GhInputSetSens(int v)
{
    if (v < 25) v = 25;
    if (v > 200) v = 200;
    if (g_ghProf.sens == v) return;
    GhInputForkIfBuiltin();
    g_ghProf.sens = v;
    GhInputSaveProfile(g_ghProf);
}

static void GhInputSetInvert(bool on)
{
    int v = on ? 1 : 0;
    if (g_ghProf.invertY == v) return;
    GhInputForkIfBuiltin();
    g_ghProf.invertY = v;
    GhInputSaveProfile(g_ghProf);
}

static void GhInputSetDead(int v)
{
    if (v < 1000) v = 1000;
    if (v > 20000) v = 20000;
    if (g_ghProf.dead == v) return;
    GhInputForkIfBuiltin();
    g_ghProf.dead = v;
    GhInputSaveProfile(g_ghProf);
}

static int GhInputList(GhProfileInfo* out, int cap)
{
    if (!out || cap <= 0) return 0;
    int n = 0;
    auto add = [&](const char* id, const wchar_t* name, bool builtin) {
        if (n >= cap) return;
        strncpy(out[n].id, id, 31);
        wcsncpy(out[n].name, name, 31);
        out[n].name[31] = 0;
        out[n].builtin = builtin;
        ++n;
    };
    add("builtin-off",   L"Off",           true);
    add("builtin-mouse", L"Mouse",         true);
    add("builtin-wasd",  L"WASD + Mouse",  true);

    WIN32_FIND_DATAW fd{};
    std::wstring glob = GhInputProfDir() + L"*.txt";
    HANDLE h = FindFirstFileW(glob.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char id[32] = {};
            const wchar_t* w = fd.cFileName;
            int i = 0;
            for (; w[i] && w[i] != L'.' && i < 31; ++i) id[i] = (char)w[i];
            id[i] = 0;
            if (!id[0] || GhInputIsBuiltinId(id)) continue;
            GhInputProfile p{};
            if (!GhInputLoadFile(id, p)) continue;
            add(p.id, p.name[0] ? p.name : L"Profile", false);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}
