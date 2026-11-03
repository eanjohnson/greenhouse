// gh_x86_gdi.h -- GDI text for a 32-bit guest, served by gh_gdifont.h.
//
// The container's gdi32 has no font engine (gh_gdifont.h). The x64 path fixed
// that for Garry's Mod; the x86 forward table still sent every gdi32 call to the
// real DLL, so HL2's vgui (vguimatsurface CWin32Font, vgui2) got CreateFontA = 0
// and drew its whole main menu with no text over the rendered background map
// (s852). These beds put the same DirectWrite-backed DCs, fonts and DIBs behind
// the 32-bit imports. Three things differ from the x64 table:
//   * handles: ours are 0x6E68'0000'xxxx with xxxx < 0x10000 (kGhGdiLowSpan), so
//     the guest holds the low 32 bits and gdi_h64 puts the tag back; a real GDI
//     handle is a sign-extended 32-bit value with its type in bits 16-22;
//   * a DIB's bits must be a guest pointer, so g_gdiBitsAlloc reserves below 4 GB;
//   * BITMAP has a pointer in it (24 bytes here, 32 on x64), and
//     EnumFontFamiliesExA's callback is guest code (call_guest32).
// Every other struct these calls take (LOGFONTA, TEXTMETRIC, ABC, GLYPHMETRICS,
// SIZE, POINT, RECT, GLYPHSET) is pointer free and is used in place.
//
// The Launcher build only: gh_gdifont.h lives in guest_loader.cpp.

#pragma once

namespace ghx86 {

#ifdef GH_LAUNCHER
#ifndef GH_X86_POP
#define GH_X86_POP(F) thunk_bytes_of((decltype(&::F))nullptr)   // as gh_x86_beds.h defines it, further down
#endif
inline HGDIOBJ gdi_h64(uint32_t g)
{
    if (g && g < kGhGdiLowSpan) return (HGDIOBJ)(0x00006E6800000000ull | g);
    return (HGDIOBJ)(intptr_t)(int32_t)g;
}
inline uint32_t gdi_h32(HGDIOBJ h) { return (uint32_t)(uintptr_t)h; }
#define GH_GDI_HDC(v) ((HDC)gdi_h64(v))

inline void RegisterGdiBeds(ThunkRegistry& R)
{
    g_gdiBitsAlloc = [](SIZE_T n) -> void* {
        void* p = low_reserve(n);
        return p ? VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) : nullptr;
    };

    static_assert(GH_X86_POP(CreateCompatibleDC) == 4, "CreateCompatibleDC");
    GH_X86_BED(R, "gdi32.dll", "CreateCompatibleDC", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return gdi_h32(Bed_CreateCompatibleDC(GH_GDI_HDC(a[0]))); });
    static_assert(GH_X86_POP(DeleteDC) == 4, "DeleteDC");
    GH_X86_BED(R, "gdi32.dll", "DeleteDC", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_DeleteDC(GH_GDI_HDC(a[0])); });
    static_assert(GH_X86_POP(DeleteObject) == 4, "DeleteObject");
    GH_X86_BED(R, "gdi32.dll", "DeleteObject", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_DeleteObject(gdi_h64(a[0])); });
    static_assert(GH_X86_POP(SelectObject) == 8, "SelectObject");
    GH_X86_BED(R, "gdi32.dll", "SelectObject", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return gdi_h32(Bed_SelectObject(GH_GDI_HDC(a[0]), gdi_h64(a[1]))); });

    static_assert(GH_X86_POP(CreateFontA) == 56, "CreateFontA");
    GH_X86_BED(R, "gdi32.dll", "CreateFontA", 56,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HFONT f = Bed_CreateFontA((int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], a[5], a[6], a[7],
                                      a[8], a[9], a[10], a[11], a[12], (LPCSTR)(uintptr_t)a[13]);
            static volatile LONG n = 0;
            if (InterlockedIncrement(&n) <= 6)
                log(std::string("X86 GDI CreateFontA '") + (a[13] ? (const char*)(uintptr_t)a[13] : "") + "' h=" +
                    std::to_string((int)a[0]) + " w=" + std::to_string((int)a[4]) + " q=" + std::to_string(a[11]) +
                    " -> " + hex(gdi_h32(f)));
            return gdi_h32(f);
        });
    static_assert(GH_X86_POP(CreateFontIndirectA) == 4, "CreateFontIndirectA");
    GH_X86_BED(R, "gdi32.dll", "CreateFontIndirectA", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return gdi_h32(Bed_CreateFontIndirectA((const LOGFONTA*)(uintptr_t)a[0])); });
    static_assert(GH_X86_POP(CreateFontIndirectW) == 4, "CreateFontIndirectW");
    GH_X86_BED(R, "gdi32.dll", "CreateFontIndirectW", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return gdi_h32(Bed_CreateFontIndirectW((const LOGFONTW*)(uintptr_t)a[0])); });

    static_assert(GH_X86_POP(SetMapMode) == 8, "SetMapMode");
    GH_X86_BED(R, "gdi32.dll", "SetMapMode", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_SetMapMode(GH_GDI_HDC(a[0]), (int)a[1]); });
    static_assert(GH_X86_POP(SetTextAlign) == 8, "SetTextAlign");
    GH_X86_BED(R, "gdi32.dll", "SetTextAlign", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_SetTextAlign(GH_GDI_HDC(a[0]), a[1]); });
    static_assert(GH_X86_POP(SetTextColor) == 8, "SetTextColor");
    GH_X86_BED(R, "gdi32.dll", "SetTextColor", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_SetTextColor(GH_GDI_HDC(a[0]), a[1]); });
    static_assert(GH_X86_POP(SetBkColor) == 8, "SetBkColor");
    GH_X86_BED(R, "gdi32.dll", "SetBkColor", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_SetBkColor(GH_GDI_HDC(a[0]), a[1]); });
    static_assert(GH_X86_POP(SetBkMode) == 8, "SetBkMode");
    GH_X86_BED(R, "gdi32.dll", "SetBkMode", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_SetBkMode(GH_GDI_HDC(a[0]), (int)a[1]); });
    static_assert(GH_X86_POP(MoveToEx) == 16, "MoveToEx");
    GH_X86_BED(R, "gdi32.dll", "MoveToEx", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)Bed_MoveToEx(GH_GDI_HDC(a[0]), (int)a[1], (int)a[2], (LPPOINT)(uintptr_t)a[3]);
        });

    static_assert(GH_X86_POP(GetTextMetricsA) == 8, "GetTextMetricsA");
    GH_X86_BED(R, "gdi32.dll", "GetTextMetricsA", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_GetTextMetricsA(GH_GDI_HDC(a[0]), (LPTEXTMETRICA)(uintptr_t)a[1]); });
    static_assert(GH_X86_POP(GetTextMetricsW) == 8, "GetTextMetricsW");
    GH_X86_BED(R, "gdi32.dll", "GetTextMetricsW", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_GetTextMetricsW(GH_GDI_HDC(a[0]), (LPTEXTMETRICW)(uintptr_t)a[1]); });
    static_assert(GH_X86_POP(GetCharABCWidthsA) == 16, "GetCharABCWidthsA");
    GH_X86_BED(R, "gdi32.dll", "GetCharABCWidthsA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_GetCharABCWidthsA(GH_GDI_HDC(a[0]), a[1], a[2], (LPABC)(uintptr_t)a[3]); });
    static_assert(GH_X86_POP(GetCharABCWidthsW) == 16, "GetCharABCWidthsW");
    GH_X86_BED(R, "gdi32.dll", "GetCharABCWidthsW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_GetCharABCWidthsW(GH_GDI_HDC(a[0]), a[1], a[2], (LPABC)(uintptr_t)a[3]); });
    static_assert(GH_X86_POP(GetGlyphOutlineA) == 28, "GetGlyphOutlineA");
    GH_X86_BED(R, "gdi32.dll", "GetGlyphOutlineA", 28,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return Bed_GetGlyphOutlineA(GH_GDI_HDC(a[0]), a[1], a[2], (LPGLYPHMETRICS)(uintptr_t)a[3], a[4],
                                        (LPVOID)(uintptr_t)a[5], (const MAT2*)(uintptr_t)a[6]);
        });
    static_assert(GH_X86_POP(GetGlyphOutlineW) == 28, "GetGlyphOutlineW");
    GH_X86_BED(R, "gdi32.dll", "GetGlyphOutlineW", 28,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return Bed_GetGlyphOutlineW(GH_GDI_HDC(a[0]), a[1], a[2], (LPGLYPHMETRICS)(uintptr_t)a[3], a[4],
                                        (LPVOID)(uintptr_t)a[5], (const MAT2*)(uintptr_t)a[6]);
        });
    static_assert(GH_X86_POP(GetTextExtentPoint32A) == 16, "GetTextExtentPoint32A");
    GH_X86_BED(R, "gdi32.dll", "GetTextExtentPoint32A", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)Bed_GetTextExtentPoint32A(GH_GDI_HDC(a[0]), (LPCSTR)(uintptr_t)a[1], (int)a[2], (LPSIZE)(uintptr_t)a[3]);
        });
    static_assert(GH_X86_POP(GetTextExtentPoint32W) == 16, "GetTextExtentPoint32W");
    GH_X86_BED(R, "gdi32.dll", "GetTextExtentPoint32W", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)Bed_GetTextExtentPoint32W(GH_GDI_HDC(a[0]), (LPCWSTR)(uintptr_t)a[1], (int)a[2], (LPSIZE)(uintptr_t)a[3]);
        });
    static_assert(GH_X86_POP(GetFontUnicodeRanges) == 8, "GetFontUnicodeRanges");
    GH_X86_BED(R, "gdi32.dll", "GetFontUnicodeRanges", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return Bed_GetFontUnicodeRanges(GH_GDI_HDC(a[0]), (LPGLYPHSET)(uintptr_t)a[1]); });
    static_assert(GH_X86_POP(ExtTextOutA) == 32, "ExtTextOutA");
    GH_X86_BED(R, "gdi32.dll", "ExtTextOutA", 32,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)Bed_ExtTextOutA(GH_GDI_HDC(a[0]), (int)a[1], (int)a[2], a[3], (const RECT*)(uintptr_t)a[4],
                                             (LPCSTR)(uintptr_t)a[5], a[6], (const INT*)(uintptr_t)a[7]);
        });
    static_assert(GH_X86_POP(ExtTextOutW) == 32, "ExtTextOutW");
    GH_X86_BED(R, "gdi32.dll", "ExtTextOutW", 32,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)Bed_ExtTextOutW(GH_GDI_HDC(a[0]), (int)a[1], (int)a[2], a[3], (const RECT*)(uintptr_t)a[4],
                                             (LPCWSTR)(uintptr_t)a[5], a[6], (const INT*)(uintptr_t)a[7]);
        });

    static_assert(GH_X86_POP(AddFontResourceA) == 4, "AddFontResourceA");
    GH_X86_BED(R, "gdi32.dll", "AddFontResourceA", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_AddFontResourceA((LPCSTR)(uintptr_t)a[0]); });
    static_assert(GH_X86_POP(AddFontResourceExA) == 12, "AddFontResourceExA");
    GH_X86_BED(R, "gdi32.dll", "AddFontResourceExA", 12,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)Bed_AddFontResourceA((LPCSTR)(uintptr_t)a[0]); });
    static_assert(GH_X86_POP(RemoveFontResourceA) == 4, "RemoveFontResourceA");
    GH_X86_BED(R, "gdi32.dll", "RemoveFontResourceA", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });
    static_assert(GH_X86_POP(RemoveFontResourceExA) == 12, "RemoveFontResourceExA");
    GH_X86_BED(R, "gdi32.dll", "RemoveFontResourceExA", 12,
        [](Machine&, const uint32_t*) -> uint64_t { return 1; });

    // The callback is guest code; the two records go in guest memory.
    static_assert(GH_X86_POP(EnumFontFamiliesExA) == 20, "EnumFontFamiliesExA");
    GH_X86_BED(R, "gdi32.dll", "EnumFontFamiliesExA", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[2]) return 0;
            const uint32_t elf = m.heap_alloc(sizeof(ENUMLOGFONTEXA) + sizeof(NEWTEXTMETRICEXA), true);
            if (!elf) return 0;
            const uint32_t ntm = elf + (uint32_t)sizeof(ENUMLOGFONTEXA);
            GhFontEnumRecord((const LOGFONTA*)(uintptr_t)a[1], *(ENUMLOGFONTEXA*)(uintptr_t)elf, *(NEWTEXTMETRICEXA*)(uintptr_t)ntm);
            const uint32_t args[4] = { elf, ntm, TRUETYPE_FONTTYPE, a[3] };
            const uint32_t r = m.call_guest32(a[2], args, 4);
            m.heap_free(elf);
            return r;
        });

    static_assert(GH_X86_POP(CreateDIBSection) == 24, "CreateDIBSection");
    GH_X86_BED(R, "gdi32.dll", "CreateDIBSection", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            void* bits = nullptr;
            HBITMAP b = Bed_CreateDIBSection(GH_GDI_HDC(a[0]), (const BITMAPINFO*)(uintptr_t)a[1], a[2], &bits,
                                             (HANDLE)(intptr_t)(int32_t)a[4], a[5]);
            if (b && (uintptr_t)bits >> 32) {
                log("X86 GDI CreateDIBSection bits " + hex((uint64_t)(uintptr_t)bits) + " above 4 GB -- refused");
                Bed_DeleteObject(b);
                b = nullptr; bits = nullptr;
            }
            if (a[3]) { const uint32_t v = (uint32_t)(uintptr_t)bits; std::memcpy((void*)(uintptr_t)a[3], &v, 4); }
            return gdi_h32(b);
        });
    // BITMAP is the one struct here with a pointer in it.
    static_assert(GH_X86_POP(GetObjectA) == 12, "GetObjectA");
    GH_X86_BED(R, "gdi32.dll", "GetObjectA", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            struct Bitmap32 { LONG type, w, h, widthBytes; WORD planes, bpp; uint32_t bits; };
            static_assert(sizeof(Bitmap32) == 24, "x86 BITMAP");
            alignas(8) uint8_t tmp[sizeof(LOGFONTW) + sizeof(DIBSECTION)] = {};
            const int r = Bed_GetObjectA(gdi_h64(a[0]), (int)sizeof(tmp), tmp);
            if (r <= 0) return 0;
            const bool bmp = r == (int)sizeof(BITMAP) || r == (int)sizeof(DIBSECTION);
            const int need = bmp ? (int)sizeof(Bitmap32) : r;
            if (!a[2]) return (uint32_t)need;
            if ((int)a[1] < need && bmp) return 0;
            if (bmp) {
                const BITMAP& s = *(const BITMAP*)tmp;
                const Bitmap32 o{ s.bmType, s.bmWidth, s.bmHeight, s.bmWidthBytes, s.bmPlanes, s.bmBitsPixel,
                                  (uint32_t)(uintptr_t)s.bmBits };
                std::memcpy((void*)(uintptr_t)a[2], &o, sizeof(o));
                return sizeof(o);
            }
            const int n = (int)a[1] < r ? (int)a[1] : r;
            std::memcpy((void*)(uintptr_t)a[2], tmp, (size_t)n);
            return (uint32_t)n;
        });
}
#undef GH_GDI_HDC
#else
inline void RegisterGdiBeds(ThunkRegistry&) {}
#endif

}   // namespace ghx86
