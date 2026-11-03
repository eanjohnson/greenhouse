// gh_gdifont.h -- a guest's GDI text, served from DirectWrite. Included by
// guest_loader.cpp after the region beds (it uses GhGdiFn / GH_REAL_GDI / GhRgnDrop).
//
// The container's gdi32 has no font engine. CreateCompatibleDC(NULL), CreateFontA,
// AddFontResourceA and EnumFontFamiliesExA all answer "nothing" with GetLastError 0
// (the 0.0.797 probe). Source's vgui (vguimatsurface CWin32Font) rasterizes every
// glyph through exactly those calls -- CreateFontA, GetTextMetricsA,
// GetCharABCWidthsW, GetGlyphOutlineW(GGO_GRAY8_BITMAP) -- so Garry's Mod drew its
// whole menu with no text. A guest DC, font and DIB are therefore ours: fake handles
// in a tagged range (as the regions are), metrics and glyph bitmaps from DirectWrite,
// and AddFontResourceA's files in a DWrite custom collection. A face the console
// does not have (Tahoma, Verdana, Courier New...) maps to one it does; Source only
// asks EnumFontFamiliesEx whether a face exists, so every face "exists".
#pragma once
#include <dwrite_3.h>

struct GhGlyph { int a = 0, b = 0, c = 0, adv = 0; RECT box{}; std::vector<uint8_t> px; };   // px: box w*h, 0..255
struct GhGdiFont {
    LOGFONTW lf{};
    winrt::com_ptr<IDWriteFontFace> face;
    std::wstring family;
    float em = 16.f, upem = 2048.f;
    int asc = 0, desc = 0, lead = 0, extLead = 0, maxW = 0, aveW = 0;   // pixels
    bool aa = true;
    std::unordered_map<uint16_t, GhGlyph> glyphs;     // by glyph index
    std::vector<WCRANGE> ranges; uint32_t nCovered = 0; bool rangesDone = false;
};
enum class GhGdiKind : uint8_t { Dc, Font, Bmp };
struct GhGdiObj {
    GhGdiKind kind = GhGdiKind::Dc;
    std::shared_ptr<GhGdiFont> font;                         // Font
    HGDIOBJ selFont = nullptr, selBmp = nullptr;             // Dc
    int mapMode = MM_TEXT; UINT align = TA_TOP | TA_LEFT;    // Dc
    int cpX = 0, cpY = 0;                                    // Dc: MoveToEx's current position
    COLORREF textColor = 0, bkColor = 0xFFFFFF; int bkMode = OPAQUE;   // Dc
    void* bits = nullptr; BITMAPINFOHEADER bi{}; LONG stride = 0;   // Bmp
    void* view = nullptr;                                    // Bmp over a caller's section: the mapped view
};
static SRWLOCK g_gdiLock = SRWLOCK_INIT;
static std::unordered_map<HGDIOBJ, GhGdiObj>* g_gdiObjs = nullptr;
static uintptr_t g_gdiNext = 0x00006E6800000000ull;   // beside the regions' 0x6E67 range
// A 32-bit guest holds our handles as their low 32 bits (gh_x86_gdi.h), so those
// stay below 0x10000: real GDI handles carry their object type in bits 16-22 and
// are never that small. GhGdiNew wraps there and skips handles still live.
static constexpr uint32_t kGhGdiLowSpan = 0x10000;
// Where a DIB's bits come from. A 32-bit guest needs them below 4 GB, so its
// runtime sets this (gh_x86_gdi.h); the memory must be VirtualFree(MEM_RELEASE)-able.
static void* (*g_gdiBitsAlloc)(SIZE_T) = nullptr;

static IDWriteFactory2*                g_dw2 = nullptr;
static IDWriteFactory5*                g_dw5 = nullptr;        // custom fonts need 5 (in-memory loader)
static IDWriteFontCollection*          g_dwSys = nullptr;
static IDWriteFontCollection1*         g_dwCustom = nullptr;
static IDWriteFontSet*                 g_dwCustomSet = nullptr;
static IDWriteFontSetBuilder1*         g_dwBuilder = nullptr;
static IDWriteInMemoryFontFileLoader*  g_dwMemLoader = nullptr;

static std::string GhW2A(const std::wstring& w){
    std::string s; for (wchar_t c : w) s += (c >= 0x20 && c < 0x7F) ? (char)c : '?'; return s;
}
static std::wstring GhDwFamilyName(IDWriteFontFamily* fam){
    winrt::com_ptr<IDWriteLocalizedStrings> names; wchar_t buf[128] = {};
    if (fam && SUCCEEDED(fam->GetFamilyNames(names.put())) && names->GetCount())
        names->GetString(0, buf, 128);
    return buf;
}
// Under g_gdiLock (exclusive).
static bool GhDwInit(){
    static bool tried = false;
    if (tried) return g_dw2 != nullptr;
    tried = true;
    const auto mk = GhGdiFn<decltype(&DWriteCreateFactory)>(L"dwrite.dll", "DWriteCreateFactory");
    HRESULT hr = E_NOINTERFACE;
    if (mk){
        hr = mk(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(&g_dw5));
        if (SUCCEEDED(hr)) g_dw2 = g_dw5;
        else hr = mk(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(&g_dw2));
    }
    if (!g_dw2){ Diag("FONT dwrite: no factory hr=" + Hex((uint32_t)hr) + " -- guest text stays blank"); return false; }
    g_dw2->GetSystemFontCollection(&g_dwSys, FALSE);
    if (g_dw5 && SUCCEEDED(g_dw5->CreateInMemoryFontFileLoader(&g_dwMemLoader)) &&
        SUCCEEDED(g_dw5->RegisterFontFileLoader(g_dwMemLoader)))
        g_dw5->CreateFontSetBuilder(&g_dwBuilder);
    std::string fams; const UINT32 n = g_dwSys ? g_dwSys->GetFontFamilyCount() : 0;
    for (UINT32 i = 0; i < n && fams.size() < 900; ++i){
        winrt::com_ptr<IDWriteFontFamily> f;
        if (SUCCEEDED(g_dwSys->GetFontFamily(i, f.put()))) fams += (i ? ", " : "") + GhW2A(GhDwFamilyName(f.get()));
    }
    Diag("FONT dwrite: factory" + std::string(g_dw5 ? "5" : "2") + " custom=" + std::to_string(g_dwBuilder != nullptr) +
         " system families=" + std::to_string(n) + ": " + fams);
    return true;
}
static bool GhDwFind(IDWriteFontCollection* c, const wchar_t* name, winrt::com_ptr<IDWriteFontFamily>& out){
    UINT32 i = 0; BOOL ok = FALSE;
    return c && name && *name && SUCCEEDED(c->FindFamilyName(name, &i, &ok)) && ok &&
           SUCCEEDED(c->GetFontFamily(i, out.put()));
}
static bool GhDwIsMono(const LOGFONTW& lf){
    std::wstring n = lf.lfFaceName; for (auto& ch : n) ch = (wchar_t)towlower(ch);
    for (const wchar_t* k : { L"courier", L"console", L"mono", L"consolas", L"fixed", L"terminal" })
        if (n.find(k) != std::wstring::npos) return true;
    return (lf.lfPitchAndFamily & 3) == FIXED_PITCH || (lf.lfPitchAndFamily & 0xF0) == FF_MODERN;
}
// Under g_gdiLock. Returns null only when DirectWrite itself is missing.
static std::shared_ptr<GhGdiFont> GhDwMakeFont(const LOGFONTW& lf){
    if (!GhDwInit()) return nullptr;
    winrt::com_ptr<IDWriteFontFamily> fam; const char* src = "custom";
    if (!GhDwFind(g_dwCustom, lf.lfFaceName, fam)){ src = "system";
        if (!GhDwFind(g_dwSys, lf.lfFaceName, fam)){ src = "fallback";
            static const wchar_t* const kSans[] = { L"Segoe UI", L"Arial", L"Tahoma", L"Verdana", L"Selawik", nullptr };
            static const wchar_t* const kMono[] = { L"Consolas", L"Courier New", L"Lucida Console", L"Cascadia Mono", L"Segoe UI", nullptr };
            for (const wchar_t* const* p = GhDwIsMono(lf) ? kMono : kSans; *p && !fam; ++p) GhDwFind(g_dwSys, *p, fam);
            if (!fam && g_dwSys && g_dwSys->GetFontFamilyCount()) g_dwSys->GetFontFamily(0, fam.put());
        }
    }
    winrt::com_ptr<IDWriteFontFace> face;
    if (fam){
        winrt::com_ptr<IDWriteFont> font;
        const LONG wt = lf.lfWeight > 0 ? lf.lfWeight : FW_NORMAL;
        if (SUCCEEDED(fam->GetFirstMatchingFont((DWRITE_FONT_WEIGHT)(wt > 950 ? 950 : wt), DWRITE_FONT_STRETCH_NORMAL,
                                                lf.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, font.put())))
            font->CreateFontFace(face.put());
    }
    // A face only the custom set knows by its GDI name (name ID 1 differs from the
    // family DWrite groups it under).
    if (!face && g_dwCustomSet){
        const DWRITE_FONT_PROPERTY p{ DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, lf.lfFaceName, nullptr };
        winrt::com_ptr<IDWriteFontSet> hit; winrt::com_ptr<IDWriteFontFaceReference> ref; winrt::com_ptr<IDWriteFontFace3> f3;
        if (SUCCEEDED(g_dwCustomSet->GetMatchingFonts(&p, 1, hit.put())) && hit->GetFontCount() &&
            SUCCEEDED(hit->GetFontFaceReference(0, ref.put())) && SUCCEEDED(ref->CreateFontFace(f3.put()))){
            face = f3.as<IDWriteFontFace>(); src = "custom-w32"; fam = nullptr;
        }
    }
    if (!face){ Diag(std::string("FONT no face for '") + GhW2A(lf.lfFaceName) + "'"); return nullptr; }

    auto f = std::make_shared<GhGdiFont>();
    f->lf = lf; f->face = face; f->family = fam ? GhDwFamilyName(fam.get()) : std::wstring(lf.lfFaceName);
    f->aa = lf.lfQuality != NONANTIALIASED_QUALITY;
    DWRITE_FONT_METRICS fm{}; face->GetMetrics(&fm);
    f->upem = fm.designUnitsPerEm ? (float)fm.designUnitsPerEm : 2048.f;
    const float cell = (float)fm.ascent + (float)fm.descent;
    // GDI: a negative height is the em size, a positive one the cell (ascent + descent).
    f->em = lf.lfHeight < 0 ? (float)-lf.lfHeight : lf.lfHeight > 0 && cell > 0 ? lf.lfHeight * f->upem / cell : 16.f;
    DWRITE_FONT_METRICS gm{};
    if (FAILED(face->GetGdiCompatibleMetrics(f->em, 1.f, nullptr, &gm))) gm = fm;
    const float k = f->em / f->upem;
    f->asc  = (int)lroundf(gm.ascent * k);
    f->desc = (int)lroundf(gm.descent * k);
    if (lf.lfHeight > 0) f->desc = lf.lfHeight - f->asc;     // GDI hands back exactly the cell asked for
    f->lead = (std::max)(0, f->asc + f->desc - (int)lroundf(f->em));
    f->extLead = (int)lroundf(gm.lineGap * k);
    winrt::com_ptr<IDWriteFontFace1> f1 = face.try_as<IDWriteFontFace1>();
    if (f1){ DWRITE_FONT_METRICS1 m1{}; f1->GetMetrics(&m1); f->maxW = (int)ceilf((m1.glyphBoxRight - m1.glyphBoxLeft) * k); }
    if (f->maxW <= 0) f->maxW = (int)ceilf(f->em * 1.5f);
    UINT32 cpx = L'x'; UINT16 gx = 0; DWRITE_GLYPH_METRICS mx{};
    if (SUCCEEDED(face->GetGlyphIndices(&cpx, 1, &gx)) && SUCCEEDED(face->GetDesignGlyphMetrics(&gx, 1, &mx, FALSE)))
        f->aveW = (int)lroundf(mx.advanceWidth * k);
    if (f->aveW <= 0) f->aveW = (std::max)(1, (int)lroundf(f->em / 2));

    static std::set<std::wstring>* seen = new std::set<std::wstring>();
    if (seen->size() < 64 && seen->insert(std::wstring(lf.lfFaceName) + L"|" + std::to_wstring(lf.lfHeight)).second)
        Diag(std::string("FONT '") + GhW2A(lf.lfFaceName) + "' h=" + std::to_string(lf.lfHeight) + " wt=" + std::to_string(lf.lfWeight) +
             " q=" + std::to_string(lf.lfQuality) + " -> " + src + " '" + GhW2A(f->family) + "' em=" + std::to_string((int)lroundf(f->em)) +
             " asc=" + std::to_string(f->asc) + " desc=" + std::to_string(f->desc) + " maxW=" + std::to_string(f->maxW));
    return f;
}
// Under g_gdiLock. The black box comes from the same rasterization the bitmap does,
// so ABC widths and GetGlyphOutline's box can never disagree.
static const GhGlyph& GhDwGlyph(GhGdiFont& f, uint16_t gi){
    auto it = f.glyphs.find(gi);
    if (it != f.glyphs.end()) return it->second;
    GhGlyph g;
    DWRITE_GLYPH_METRICS m{};
    if (FAILED(f.face->GetGdiCompatibleGlyphMetrics(f.em, 1.f, nullptr, FALSE, &gi, 1, &m, FALSE)))
        f.face->GetDesignGlyphMetrics(&gi, 1, &m, FALSE);
    g.adv = (int)lroundf(m.advanceWidth * f.em / f.upem);
    const FLOAT advF = (FLOAT)g.adv; const DWRITE_GLYPH_OFFSET off{};
    DWRITE_GLYPH_RUN run{ f.face.get(), f.em, 1, &gi, &advF, &off, FALSE, 0 };
    winrt::com_ptr<IDWriteGlyphRunAnalysis> an;
    HRESULT hr = g_dw2->CreateGlyphRunAnalysis(&run, nullptr,
        f.aa ? DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC : DWRITE_RENDERING_MODE_ALIASED, DWRITE_MEASURING_MODE_NATURAL,
        DWRITE_GRID_FIT_MODE_ENABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.f, 0.f, an.put());
    RECT rc{};
    if (SUCCEEDED(hr)) hr = an->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &rc);
    if (SUCCEEDED(hr) && rc.right > rc.left && rc.bottom > rc.top){
        const UINT32 w = rc.right - rc.left, h = rc.bottom - rc.top;
        g.px.resize((size_t)w * h);
        if (FAILED(an->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &rc, g.px.data(), (UINT32)g.px.size()))) g.px.clear();
    }
    if (!g.px.empty()){
        g.box = rc; g.a = rc.left; g.b = rc.right - rc.left; g.c = g.adv - g.a - g.b;
    } else {
        g.box = RECT{}; g.a = 0; g.b = g.adv > 0 ? g.adv : 1; g.c = g.adv - g.b;   // a space: all advance, no ink
    }
    static volatile LONG bad = 0;
    if (FAILED(hr) && InterlockedIncrement(&bad) <= 4) Diag("FONT glyph " + std::to_string(gi) + " analysis hr=" + Hex((uint32_t)hr));
    return f.glyphs.emplace(gi, std::move(g)).first->second;
}
static uint16_t GhDwIndex(GhGdiFont& f, uint32_t cp){
    UINT16 gi = 0; f.face->GetGlyphIndices(&cp, 1, &gi); return gi;
}
static wchar_t GhAnsiChar(UINT ch){
    if (ch < 0x80) return (wchar_t)ch;
    char b[2] = { (char)(ch & 0xFF), 0 }; wchar_t w = 0;
    return MultiByteToWideChar(CP_ACP, 0, b, 1, &w, 1) ? w : (wchar_t)ch;
}

// --- the handle table ---------------------------------------------------------
static HGDIOBJ GhGdiNew(GhGdiObj&& o){
    AcquireSRWLockExclusive(&g_gdiLock);
    if (!g_gdiObjs) g_gdiObjs = new std::unordered_map<HGDIOBJ, GhGdiObj>();
    HGDIOBJ h = nullptr;
    do {
        g_gdiNext += 4;
        if ((uint32_t)g_gdiNext >= kGhGdiLowSpan) g_gdiNext = 0x00006E6800000004ull;
        h = (HGDIOBJ)g_gdiNext;
    } while (g_gdiObjs->count(h));
    (*g_gdiObjs)[h] = std::move(o);
    ReleaseSRWLockExclusive(&g_gdiLock);
    return h;
}
// Under g_gdiLock.
static GhGdiObj* GhGdiAt(HGDIOBJ h, GhGdiKind k){
    if (!g_gdiObjs || !h) return nullptr;
    auto it = g_gdiObjs->find(h);
    return it != g_gdiObjs->end() && it->second.kind == k ? &it->second : nullptr;
}
static bool GhGdiIsOurs(HGDIOBJ h){
    if ((uintptr_t)h >> 32 != 0x6E68) return false;
    AcquireSRWLockShared(&g_gdiLock);
    const bool ok = g_gdiObjs && g_gdiObjs->count(h);
    ReleaseSRWLockShared(&g_gdiLock);
    return ok;
}
static bool GhGdiDrop(HGDIOBJ h){
    if ((uintptr_t)h >> 32 != 0x6E68) return false;
    AcquireSRWLockExclusive(&g_gdiLock);
    bool ok = false;
    if (g_gdiObjs){
        auto it = g_gdiObjs->find(h);
        if (it != g_gdiObjs->end()){
            if (it->second.kind == GhGdiKind::Bmp && it->second.view) UnmapViewOfFile(it->second.view);
            else if (it->second.kind == GhGdiKind::Bmp && it->second.bits) VirtualFree(it->second.bits, 0, MEM_RELEASE);
            g_gdiObjs->erase(it); ok = true;
        }
    }
    ReleaseSRWLockExclusive(&g_gdiLock);
    return ok;
}
// Under g_gdiLock: the font selected into one of our DCs.
static GhGdiFont* GhDcFont(HDC dc){
    GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc);
    GhGdiObj* f = d ? GhGdiAt(d->selFont, GhGdiKind::Font) : nullptr;
    return f ? f->font.get() : nullptr;
}
struct GhGdiGuard { GhGdiGuard(){ AcquireSRWLockExclusive(&g_gdiLock); } ~GhGdiGuard(){ ReleaseSRWLockExclusive(&g_gdiLock); } };

// --- beds ---------------------------------------------------------------------
static HDC WINAPI Bed_CreateCompatibleDC(HDC){ return (HDC)GhGdiNew(GhGdiObj{ GhGdiKind::Dc }); }
static BOOL WINAPI Bed_DeleteDC(HDC dc){
    if (GhGdiDrop(dc)) return TRUE;
    GH_REAL_GDI(L"gdi32.dll", DeleteDC, FALSE); return r_DeleteDC(dc);
}
static HFONT GhCreateFontW(const LOGFONTW& lf){
    std::shared_ptr<GhGdiFont> f;
    { GhGdiGuard g; f = GhDwMakeFont(lf); }
    if (!f) return nullptr;
    GhGdiObj o; o.kind = GhGdiKind::Font; o.font = std::move(f);
    return (HFONT)GhGdiNew(std::move(o));
}
static LOGFONTW GhLogFontW(const LOGFONTA& a){
    LOGFONTW w{};
    memcpy(&w, &a, offsetof(LOGFONTA, lfFaceName));
    MultiByteToWideChar(CP_ACP, 0, a.lfFaceName, -1, w.lfFaceName, LF_FACESIZE);
    w.lfFaceName[LF_FACESIZE - 1] = 0;
    return w;
}
static HFONT WINAPI Bed_CreateFontA(int h, int w, int esc, int ori, int wt, DWORD it, DWORD ul, DWORD so, DWORD cs,
                                    DWORD op, DWORD cp, DWORD q, DWORD pf, LPCSTR face){
    LOGFONTA a{ h, w, esc, ori, wt, (BYTE)it, (BYTE)ul, (BYTE)so, (BYTE)cs, (BYTE)op, (BYTE)cp, (BYTE)q, (BYTE)pf, {} };
    if (face) strncpy_s(a.lfFaceName, face, _TRUNCATE);
    return GhCreateFontW(GhLogFontW(a));
}
static HFONT WINAPI Bed_CreateFontIndirectA(const LOGFONTA* a){ return a ? GhCreateFontW(GhLogFontW(*a)) : nullptr; }
static HFONT WINAPI Bed_CreateFontIndirectW(const LOGFONTW* w){ return w ? GhCreateFontW(*w) : nullptr; }
static HGDIOBJ WINAPI Bed_SelectObject(HDC dc, HGDIOBJ obj){
    if (!GhGdiIsOurs(dc)){
        if (GhGdiIsOurs(obj)) return nullptr;            // our font into a real DC: GDI could not use it either
        GH_REAL_GDI(L"gdi32.dll", SelectObject, nullptr); return r_SelectObject(dc, obj);
    }
    GhGdiGuard g;
    GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc);
    if (!d) return nullptr;
    HGDIOBJ prev = nullptr;
    if (GhGdiAt(obj, GhGdiKind::Font)){ prev = d->selFont; d->selFont = obj; }
    else if (GhGdiAt(obj, GhGdiKind::Bmp)){ prev = d->selBmp; d->selBmp = obj; }
    else return nullptr;
    return prev ? prev : obj;                            // no stock objects: hand back something non-null
}
static int WINAPI Bed_SetMapMode(HDC dc, int mode){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", SetMapMode, 0); return r_SetMapMode(dc, mode); }
    GhGdiGuard g; GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc); if (!d) return 0;
    const int prev = d->mapMode; d->mapMode = mode; return prev;
}
static UINT WINAPI Bed_SetTextAlign(HDC dc, UINT a){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", SetTextAlign, GDI_ERROR); return r_SetTextAlign(dc, a); }
    GhGdiGuard g; GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc); if (!d) return GDI_ERROR;
    const UINT prev = d->align; d->align = a; return prev;
}
template <class TM> static void GhFillTm(const GhGdiFont& f, TM* t){
    memset(t, 0, sizeof(*t));
    t->tmHeight = f.asc + f.desc; t->tmAscent = f.asc; t->tmDescent = f.desc;
    t->tmInternalLeading = f.lead; t->tmExternalLeading = f.extLead;
    t->tmAveCharWidth = f.aveW; t->tmMaxCharWidth = f.maxW;
    t->tmWeight = f.lf.lfWeight ? f.lf.lfWeight : FW_NORMAL; t->tmItalic = f.lf.lfItalic;
    t->tmUnderlined = f.lf.lfUnderline; t->tmStruckOut = f.lf.lfStrikeOut;
    t->tmDigitizedAspectX = t->tmDigitizedAspectY = 96;
    t->tmFirstChar = 0x20; t->tmLastChar = sizeof(t->tmLastChar) == 1 ? 0xFF : 0xFFFC;
    t->tmDefaultChar = sizeof(t->tmDefaultChar) == 1 ? 0x1F : 0x1F; t->tmBreakChar = 0x20;
    // TMPF_FIXED_PITCH SET means variable pitch (GDI's inverted bit).
    t->tmPitchAndFamily = TMPF_TRUETYPE | TMPF_VECTOR | (GhDwIsMono(f.lf) ? 0 : TMPF_FIXED_PITCH) |
                          (GhDwIsMono(f.lf) ? FF_MODERN : FF_SWISS);
    t->tmCharSet = f.lf.lfCharSet;
}
static BOOL WINAPI Bed_GetTextMetricsA(HDC dc, LPTEXTMETRICA t){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetTextMetricsA, FALSE); return r_GetTextMetricsA(dc, t); }
    GhGdiGuard g; GhGdiFont* f = GhDcFont(dc);
    if (!f || !t) return FALSE;
    GhFillTm(*f, t); return TRUE;
}
static BOOL WINAPI Bed_GetTextMetricsW(HDC dc, LPTEXTMETRICW t){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetTextMetricsW, FALSE); return r_GetTextMetricsW(dc, t); }
    GhGdiGuard g; GhGdiFont* f = GhDcFont(dc);
    if (!f || !t) return FALSE;
    GhFillTm(*f, t); return TRUE;
}
static BOOL GhAbc(HDC dc, UINT first, UINT last, LPABC out, bool ansi){
    GhGdiGuard g; GhGdiFont* f = GhDcFont(dc);
    if (!f || !out || last < first) return FALSE;
    for (UINT c = first; c <= last; ++c){
        const GhGlyph& gl = GhDwGlyph(*f, GhDwIndex(*f, ansi ? GhAnsiChar(c) : c));
        out[c - first] = ABC{ gl.a, (UINT)gl.b, gl.c };
    }
    return TRUE;
}
static BOOL WINAPI Bed_GetCharABCWidthsW(HDC dc, UINT first, UINT last, LPABC out){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetCharABCWidthsW, FALSE); return r_GetCharABCWidthsW(dc, first, last, out); }
    return GhAbc(dc, first, last, out, false);
}
static BOOL WINAPI Bed_GetCharABCWidthsA(HDC dc, UINT first, UINT last, LPABC out){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetCharABCWidthsA, FALSE); return r_GetCharABCWidthsA(dc, first, last, out); }
    return GhAbc(dc, first, last, out, true);
}
static DWORD GhGlyphOutline(HDC dc, UINT ch, UINT fmt, LPGLYPHMETRICS gm, DWORD cb, LPVOID buf, bool ansi){
    GhGdiGuard g; GhGdiFont* f = GhDcFont(dc);
    if (!f) return GDI_ERROR;
    const bool byIndex = (fmt & GGO_GLYPH_INDEX) != 0;
    fmt &= ~(GGO_GLYPH_INDEX | GGO_UNHINTED);
    const GhGlyph& gl = GhDwGlyph(*f, byIndex ? (uint16_t)ch : GhDwIndex(*f, ansi ? GhAnsiChar(ch) : ch));
    const UINT w = gl.px.empty() ? 0 : gl.box.right - gl.box.left, h = gl.px.empty() ? 0 : gl.box.bottom - gl.box.top;
    if (gm){
        gm->gmBlackBoxX = w ? w : 1; gm->gmBlackBoxY = h ? h : 1;
        gm->gmptGlyphOrigin.x = gl.px.empty() ? 0 : gl.box.left;
        gm->gmptGlyphOrigin.y = gl.px.empty() ? 0 : -gl.box.top;   // GDI's y points up from the baseline
        gm->gmCellIncX = (short)gl.adv; gm->gmCellIncY = 0;
    }
    if (fmt == GGO_METRICS) return 1;
    int levels = 0; DWORD stride = 0;
    switch (fmt){
        case GGO_BITMAP:      stride = ((w + 31) / 32) * 4; break;
        case GGO_GRAY2_BITMAP: levels = 4;  stride = (w + 3) & ~3u; break;
        case GGO_GRAY4_BITMAP: levels = 16; stride = (w + 3) & ~3u; break;
        case GGO_GRAY8_BITMAP: levels = 64; stride = (w + 3) & ~3u; break;
        default: {
            static volatile LONG n = 0;
            if (InterlockedIncrement(&n) <= 4) Diag("FONT GetGlyphOutline format " + std::to_string(fmt) + " not served");
            return GDI_ERROR;
        }
    }
    const DWORD need = stride * h;
    if (!need) return 0;                                 // no ink: a space
    if (!buf || !cb) return need;
    if (cb < need) return GDI_ERROR;
    uint8_t* o = (uint8_t*)buf; memset(o, 0, need);
    for (UINT y = 0; y < h; ++y){
        const uint8_t* s = &gl.px[(size_t)y * w];
        uint8_t* row = o + (size_t)y * stride;
        for (UINT x = 0; x < w; ++x){
            if (levels) row[x] = (uint8_t)((s[x] * levels + 127) / 255);
            else if (s[x] >= 128) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        }
    }
    return need;
}
static DWORD WINAPI Bed_GetGlyphOutlineW(HDC dc, UINT ch, UINT fmt, LPGLYPHMETRICS gm, DWORD cb, LPVOID buf, const MAT2* m){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetGlyphOutlineW, GDI_ERROR); return r_GetGlyphOutlineW(dc, ch, fmt, gm, cb, buf, m); }
    return GhGlyphOutline(dc, ch, fmt, gm, cb, buf, false);
}
static DWORD WINAPI Bed_GetGlyphOutlineA(HDC dc, UINT ch, UINT fmt, LPGLYPHMETRICS gm, DWORD cb, LPVOID buf, const MAT2* m){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetGlyphOutlineA, GDI_ERROR); return r_GetGlyphOutlineA(dc, ch, fmt, gm, cb, buf, m); }
    return GhGlyphOutline(dc, ch, fmt, gm, cb, buf, true);
}
static BOOL GhExtent(HDC dc, const wchar_t* s, int n, LPSIZE sz){
    GhGdiGuard g; GhGdiFont* f = GhDcFont(dc);
    if (!f || !sz || n < 0) return FALSE;
    int cx = 0;
    for (int i = 0; i < n; ++i) cx += GhDwGlyph(*f, GhDwIndex(*f, s[i])).adv;
    sz->cx = cx; sz->cy = f->asc + f->desc;
    return TRUE;
}
static BOOL WINAPI Bed_GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE sz){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetTextExtentPoint32A, FALSE); return r_GetTextExtentPoint32A(dc, s, n, sz); }
    std::wstring w;
    if (s && n > 0){ w.resize(n); w.resize(MultiByteToWideChar(CP_ACP, 0, s, n, &w[0], n)); }
    return GhExtent(dc, w.c_str(), (int)w.size(), sz);
}
static BOOL WINAPI Bed_GetTextExtentPoint32W(HDC dc, LPCWSTR s, int n, LPSIZE sz){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetTextExtentPoint32W, FALSE); return r_GetTextExtentPoint32W(dc, s, n, sz); }
    return GhExtent(dc, s ? s : L"", s ? n : 0, sz);
}
static DWORD WINAPI Bed_GetFontUnicodeRanges(HDC dc, LPGLYPHSET gs){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", GetFontUnicodeRanges, 0); return r_GetFontUnicodeRanges(dc, gs); }
    GhGdiGuard g; GhGdiFont* f = GhDcFont(dc);
    if (!f) return 0;
    if (!f->rangesDone){
        f->rangesDone = true;
        std::vector<UINT32> cps(4096); std::vector<UINT16> gis(4096);
        for (UINT32 base = 0; base < 0x10000; base += 4096){
            for (UINT32 i = 0; i < 4096; ++i) cps[i] = base + i;
            if (FAILED(f->face->GetGlyphIndices(cps.data(), 4096, gis.data()))) continue;
            for (UINT32 i = 0; i < 4096; ++i){
                if (!gis[i]) continue;
                const WCHAR c = (WCHAR)(base + i);
                ++f->nCovered;
                if (!f->ranges.empty() && f->ranges.back().wcLow + f->ranges.back().cGlyphs == c) ++f->ranges.back().cGlyphs;
                else f->ranges.push_back(WCRANGE{ c, 1 });
            }
        }
    }
    const DWORD size = (DWORD)(offsetof(GLYPHSET, ranges) + f->ranges.size() * sizeof(WCRANGE));
    if (!gs) return size;
    gs->cbThis = size; gs->flAccel = 0; gs->cGlyphsSupported = f->nCovered; gs->cRanges = (DWORD)f->ranges.size();
    if (!f->ranges.empty()) memcpy(gs->ranges, f->ranges.data(), f->ranges.size() * sizeof(WCRANGE));
    return size;
}
// Source only asks whether the face exists (its callback sets a flag); every face does.
// The one record every enumeration reports (the x86 bed fills guest copies with it).
static void GhFontEnumRecord(const LOGFONTA* lf, ENUMLOGFONTEXA& elf, NEWTEXTMETRICEXA& ntm){
    elf = ENUMLOGFONTEXA{}; ntm = NEWTEXTMETRICEXA{};
    if (lf) elf.elfLogFont = *lf;
    if (!elf.elfLogFont.lfFaceName[0]) strcpy_s(elf.elfLogFont.lfFaceName, "Segoe UI");
    memcpy(elf.elfFullName, elf.elfLogFont.lfFaceName, LF_FACESIZE);
    strcpy_s((char*)elf.elfStyle, sizeof(elf.elfStyle), "Regular");
    strcpy_s((char*)elf.elfScript, sizeof(elf.elfScript), "Western");
    ntm.ntmTm.tmHeight = 16; ntm.ntmTm.tmAscent = 13; ntm.ntmTm.tmDescent = 3; ntm.ntmTm.tmWeight = FW_NORMAL;
    ntm.ntmTm.tmCharSet = elf.elfLogFont.lfCharSet; ntm.ntmTm.tmPitchAndFamily = TMPF_TRUETYPE | TMPF_VECTOR | TMPF_FIXED_PITCH;
    ntm.ntmTm.ntmFlags = NTM_REGULAR | NTM_TT_OPENTYPE; ntm.ntmTm.ntmSizeEM = 2048;
}
static int WINAPI Bed_EnumFontFamiliesExA(HDC, LPLOGFONTA lf, FONTENUMPROCA cb, LPARAM lp, DWORD){
    if (!cb) return 0;
    ENUMLOGFONTEXA elf; NEWTEXTMETRICEXA ntm;
    GhFontEnumRecord(lf, elf, ntm);
    return cb((const LOGFONTA*)&elf, (const TEXTMETRICA*)&ntm, TRUETYPE_FONTTYPE, lp);
}
static int WINAPI Bed_AddFontResourceA(LPCSTR path){
    HANDLE h = path ? Bed_CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE){ Diag(std::string("FONT AddFontResourceA '") + (path ? path : "") + "' not found"); return 0; }
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    std::vector<uint8_t> data((size_t)(sz.QuadPart > 0 && sz.QuadPart < (64 << 20) ? sz.QuadPart : 0));
    DWORD got = 0; const BOOL rd = data.empty() ? FALSE : ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(h);
    GhGdiGuard g;
    if (!rd || got != data.size() || !GhDwInit() || !g_dwBuilder){
        Diag(std::string("FONT AddFontResourceA '") + path + "' read=" + std::to_string(rd) + " bytes=" + std::to_string(got) +
             " builder=" + std::to_string(g_dwBuilder != nullptr));
        return 0;
    }
    winrt::com_ptr<IDWriteFontFile> file; winrt::com_ptr<IDWriteFontSet> set; winrt::com_ptr<IDWriteFontCollection1> coll;
    HRESULT hr = g_dwMemLoader->CreateInMemoryFontFileReference(g_dw5, data.data(), (UINT32)data.size(), nullptr, file.put());   // copies
    if (SUCCEEDED(hr)) hr = g_dwBuilder->AddFontFile(file.get());
    if (SUCCEEDED(hr)) hr = g_dwBuilder->CreateFontSet(set.put());
    if (SUCCEEDED(hr)) hr = g_dw5->CreateFontCollectionFromFontSet(set.get(), coll.put());
    std::string fams;
    if (SUCCEEDED(hr)){
        if (g_dwCustom) g_dwCustom->Release();
        if (g_dwCustomSet) g_dwCustomSet->Release();
        g_dwCustom = coll.detach(); g_dwCustomSet = set.detach();
        for (UINT32 i = 0; i < g_dwCustom->GetFontFamilyCount(); ++i){
            winrt::com_ptr<IDWriteFontFamily> f;
            if (SUCCEEDED(g_dwCustom->GetFontFamily(i, f.put()))) fams += (i ? ", " : "") + GhW2A(GhDwFamilyName(f.get()));
        }
    }
    Diag(std::string("FONT AddFontResourceA '") + path + "' " + std::to_string(got) + " bytes hr=" + Hex((uint32_t)hr) + " custom families: " + fams);
    return SUCCEEDED(hr) ? 1 : 0;
}
static BOOL WINAPI Bed_RemoveFontResourceA(LPCSTR){ return TRUE; }   // the custom set lives as long as the process
static int WINAPI Bed_AddFontResourceExA(LPCSTR path, DWORD, PVOID){ return Bed_AddFontResourceA(path); }   // FR_PRIVATE is all we do

// The render-to-bitmap half of vgui's CWin32Font::GetCharRGBA. A font that is
// not antialiased (or a glyph GetGlyphOutline(GGO_GRAY8) answers 0 for) is drawn
// with ExtTextOutW into the DIB selected into the DC: OPAQUE background in the
// bk colour, the glyph in the text colour, at the current position (vgui sets
// TA_UPDATECP and MoveToEx(-A, 0) first). Pixels are 32-bit BGRX.
static BOOL WINAPI Bed_MoveToEx(HDC dc, int x, int y, LPPOINT old){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", MoveToEx, FALSE); return r_MoveToEx(dc, x, y, old); }
    GhGdiGuard g; GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc); if (!d) return FALSE;
    if (old){ old->x = d->cpX; old->y = d->cpY; }
    d->cpX = x; d->cpY = y; return TRUE;
}
static COLORREF WINAPI Bed_SetTextColor(HDC dc, COLORREF c){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", SetTextColor, CLR_INVALID); return r_SetTextColor(dc, c); }
    GhGdiGuard g; GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc); if (!d) return CLR_INVALID;
    const COLORREF prev = d->textColor; d->textColor = c; return prev;
}
static COLORREF WINAPI Bed_SetBkColor(HDC dc, COLORREF c){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", SetBkColor, CLR_INVALID); return r_SetBkColor(dc, c); }
    GhGdiGuard g; GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc); if (!d) return CLR_INVALID;
    const COLORREF prev = d->bkColor; d->bkColor = c; return prev;
}
static int WINAPI Bed_SetBkMode(HDC dc, int mode){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", SetBkMode, 0); return r_SetBkMode(dc, mode); }
    GhGdiGuard g; GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc); if (!d) return 0;
    const int prev = d->bkMode; d->bkMode = mode; return prev;
}
static BOOL GhTextOut(HDC dc, int x, int y, UINT opts, const RECT* clip, const wchar_t* s, UINT n, const INT* dx){
    GhGdiGuard g;
    GhGdiObj* d = GhGdiAt(dc, GhGdiKind::Dc);
    GhGdiObj* b = d ? GhGdiAt(d->selBmp, GhGdiKind::Bmp) : nullptr;
    GhGdiFont* f = GhDcFont(dc);
    if (!d || !f) return FALSE;
    const bool upd = (d->align & TA_UPDATECP) != 0;
    int px = upd ? d->cpX : x, py = upd ? d->cpY : y;
    if ((d->align & (TA_BASELINE | TA_BOTTOM)) == TA_BASELINE) py -= f->asc;
    else if ((d->align & (TA_BASELINE | TA_BOTTOM)) == TA_BOTTOM) py -= f->asc + f->desc;
    const bool canDraw = b && b->bits && b->bi.biBitCount == 32;
    static volatile LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 4)
        Diag("FONT ExtTextOut n=" + std::to_string(n) + " at " + std::to_string(px) + "," + std::to_string(py) +
             " bmp=" + (b ? std::to_string(b->bi.biWidth) + "x" + std::to_string(b->bi.biHeight) + "x" + std::to_string(b->bi.biBitCount) : std::string("none")) +
             " bk=" + std::to_string(d->bkMode) + (canDraw ? "" : " NOT DRAWN"));
    const LONG W = canDraw ? b->bi.biWidth : 0, H = canDraw ? (b->bi.biHeight < 0 ? -b->bi.biHeight : b->bi.biHeight) : 0;
    auto at = [&](LONG xx, LONG yy) -> uint32_t* {
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) return nullptr;
        if (clip && (opts & ETO_CLIPPED) && (xx < clip->left || xx >= clip->right || yy < clip->top || yy >= clip->bottom)) return nullptr;
        const LONG row = b->bi.biHeight < 0 ? yy : H - 1 - yy;
        return (uint32_t*)((uint8_t*)b->bits + (size_t)row * b->stride) + xx;
    };
    auto bgrx = [](COLORREF c) -> uint32_t { return (uint32_t)GetBValue(c) | ((uint32_t)GetGValue(c) << 8) | ((uint32_t)GetRValue(c) << 16); };
    const uint32_t fg = bgrx(d->textColor), bk = bgrx(d->bkColor);
    if (canDraw && (opts & ETO_OPAQUE) && clip)
        for (LONG yy = clip->top; yy < clip->bottom; ++yy) for (LONG xx = clip->left; xx < clip->right; ++xx) if (uint32_t* p = at(xx, yy)) *p = bk;
    for (UINT i = 0; i < n; ++i){
        const GhGlyph& gl = GhDwGlyph(*f, GhDwIndex(*f, s[i]));
        if (canDraw){
            if (d->bkMode == OPAQUE)
                for (int yy = 0; yy < f->asc + f->desc; ++yy) for (int xx = 0; xx < gl.adv; ++xx) if (uint32_t* p = at(px + xx, py + yy)) *p = bk;
            const LONG gw = gl.box.right - gl.box.left, gh = gl.box.bottom - gl.box.top;
            for (LONG yy = 0; !gl.px.empty() && yy < gh; ++yy)
                for (LONG xx = 0; xx < gw; ++xx){
                    const uint8_t cov = gl.px[(size_t)yy * gw + xx];
                    if (!cov) continue;
                    uint32_t* p = at(px + gl.box.left + xx, py + f->asc + gl.box.top + yy);
                    if (!p) continue;
                    if (f->aa){
                        auto mix = [&](int sh){ const int a = (fg >> sh) & 0xFF, z = (*p >> sh) & 0xFF; return (uint32_t)((z + (a - z) * cov / 255) & 0xFF) << sh; };
                        *p = mix(0) | mix(8) | mix(16);
                    } else if (cov >= 128) *p = fg;
                }
        }
        px += dx ? dx[i] : gl.adv;
    }
    if (upd) d->cpX = px;
    return TRUE;
}
static BOOL WINAPI Bed_ExtTextOutW(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCWSTR s, UINT n, const INT* dx){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", ExtTextOutW, FALSE); return r_ExtTextOutW(dc, x, y, opts, rc, s, n, dx); }
    return GhTextOut(dc, x, y, opts, rc, s ? s : L"", s ? n : 0, dx);
}
static BOOL WINAPI Bed_ExtTextOutA(HDC dc, int x, int y, UINT opts, const RECT* rc, LPCSTR s, UINT n, const INT* dx){
    if (!GhGdiIsOurs(dc)){ GH_REAL_GDI(L"gdi32.dll", ExtTextOutA, FALSE); return r_ExtTextOutA(dc, x, y, opts, rc, s, n, dx); }
    std::wstring w;
    if (s && n){ w.resize(n); w.resize(MultiByteToWideChar(CP_ACP, 0, s, (int)n, &w[0], (int)n)); }
    return GhTextOut(dc, x, y, opts, rc, w.c_str(), (UINT)w.size(), w.size() == n ? dx : nullptr);
}
// A real DC goes to the real gdi32 first. When that fails too -- the container's
// CreateDIBSection(NULL, ..) does, and Chromium's paint canvas (skia
// CreateHBitmap over a shared section, libcef 86 +0x2704425) dies on it -- the
// bitmap is ours: the caller only wants the bits, mapped from its section when
// it passes one.
static HBITMAP WINAPI Bed_CreateDIBSection(HDC dc, const BITMAPINFO* bi, UINT use, void** bits, HANDLE sec, DWORD off){
    if (!GhGdiIsOurs(dc)){
        static const auto real = GhGdiFn<decltype(&CreateDIBSection)>(L"gdi32.dll", "CreateDIBSection");
        if (HBITMAP r = real ? real(dc, bi, use, bits, sec, off) : nullptr) return r;
    }
    if (!bi) return nullptr;
    const BITMAPINFOHEADER& h = bi->bmiHeader;
    const LONG w = h.biWidth, rows = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    if (w <= 0 || rows <= 0 || !h.biBitCount) return nullptr;
    const LONG stride = ((w * h.biBitCount + 31) / 32) * 4;
    const SIZE_T size = (SIZE_T)stride * rows;
    void* p = nullptr; void* view = nullptr;
    if (sec){
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        const DWORD base = off - off % si.dwAllocationGranularity;   // a view starts on the granularity
        view = MapViewOfFile(sec, FILE_MAP_READ | FILE_MAP_WRITE, 0, base, size + (off - base));
        if (view) p = (uint8_t*)view + (off - base);
    } else {
        p = g_gdiBitsAlloc ? g_gdiBitsAlloc(size) : VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    static volatile LONG logged = 0;
    if (!GhGdiIsOurs(dc) && InterlockedIncrement(&logged) <= 4)
        Diag("GDI CreateDIBSection dc=" + Hex((uintptr_t)dc) + " " + std::to_string(w) + "x" + std::to_string(h.biHeight) +
             "x" + std::to_string(h.biBitCount) + " sec=" + Hex((uintptr_t)sec) + " off=" + std::to_string(off) +
             " -> ours " + (p ? "ok" : "FAILED gle=" + std::to_string(GetLastError())));
    if (!p) return nullptr;
    GhGdiObj o; o.kind = GhGdiKind::Bmp; o.bits = p; o.view = view; o.bi = h; o.stride = stride;
    if (bits) *bits = p;
    return (HBITMAP)GhGdiNew(std::move(o));
}
static int WINAPI Bed_GetObjectA(HANDLE h, int cb, LPVOID out){
    if (!GhGdiIsOurs(h)){ GH_REAL_GDI(L"gdi32.dll", GetObjectA, 0); return r_GetObjectA(h, cb, out); }
    GhGdiGuard g;
    if (GhGdiObj* b = GhGdiAt(h, GhGdiKind::Bmp)){
        if (!out) return sizeof(BITMAP);
        if (cb < (int)sizeof(BITMAP)) return 0;
        BITMAP bm{ 0, b->bi.biWidth, b->bi.biHeight < 0 ? -b->bi.biHeight : b->bi.biHeight, b->stride, 1, b->bi.biBitCount, b->bits };
        memcpy(out, &bm, sizeof(bm)); return sizeof(BITMAP);
    }
    if (GhGdiObj* f = GhGdiAt(h, GhGdiKind::Font)){
        if (!out) return sizeof(LOGFONTA);
        if (cb < (int)sizeof(LOGFONTA)) return 0;
        LOGFONTA a{}; memcpy(&a, &f->font->lf, offsetof(LOGFONTA, lfFaceName));
        WideCharToMultiByte(CP_ACP, 0, f->font->lf.lfFaceName, -1, a.lfFaceName, LF_FACESIZE, nullptr, nullptr);
        memcpy(out, &a, sizeof(a)); return sizeof(LOGFONTA);
    }
    return 0;
}
static int WINAPI Bed_GetObjectW(HANDLE h, int cb, LPVOID out){
    if (!GhGdiIsOurs(h)){ GH_REAL_GDI(L"gdi32.dll", GetObjectW, 0); return r_GetObjectW(h, cb, out); }
    {
        GhGdiGuard g;
        if (GhGdiObj* f = GhGdiAt(h, GhGdiKind::Font)){
            if (!out) return sizeof(LOGFONTW);
            if (cb < (int)sizeof(LOGFONTW)) return 0;
            memcpy(out, &f->font->lf, sizeof(LOGFONTW)); return sizeof(LOGFONTW);
        }
    }
    return Bed_GetObjectA(h, cb, out);   // a BITMAP has no A/W difference
}
