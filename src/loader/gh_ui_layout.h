// gh_ui_layout.h -- the layout interface, the registry, and the page entry points.
//
// THE DECISION THIS HEADER ENCODES
// --------------------------------
// Layouts are code; themes are data. A LAYOUT is a compiled renderer with a
// stable id -- "list", "blades", "grid" -- that ships inside the binary. A THEME
// is data that names a layout and supplies tokens. That is what lets a theme be
// downloaded from the package repository while containing nothing executable,
// and it is still enough for two themes to look nothing like each other,
// because the layout one of them selects is nothing like the other's.
//
// The price, paid knowingly: a genuinely NEW structure needs an app update. It
// costs the seven functions below, which is the reason the page archetypes were
// collapsed to seven in the first place.
#pragma once

#include "gh_ui.h"
#include "gh_ui_vm.h"

// ---------------------------------------------------------------------------
// The interface a layout implements
// ---------------------------------------------------------------------------
// `chrome` draws the header, the footer and anything else that frames a page,
// and RETURNS the rectangle left over for the body. Every other entry point is
// handed that rectangle, so a layout that wants a sidebar, a blade strip or a
// title bar of a different height says so once rather than in seven places.
//
// `grid` may be null: a layout without a tile view renders GH_PAGE_GRID as a
// list, which is why GhUiListVM serves both.
struct GhUiLayout {
    const char* id;
    const char* name;

    D2D1_RECT_F (*chrome)  (const GhUiChrome& c);
    void        (*list)    (const GhUiListVM& vm,     const D2D1_RECT_F& body);
    void        (*grid)    (const GhUiListVM& vm,     const D2D1_RECT_F& body);
    void        (*modal)   (const GhUiModalVM& vm);
    void        (*qr)      (const GhUiQrVM& vm,       const D2D1_RECT_F& body);
    void        (*progress)(const GhUiProgressVM& vm, const D2D1_RECT_F& body);
    void        (*message) (const GhUiMessageVM& vm,  const D2D1_RECT_F& body);
};

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------
// Registration is explicit (GhUiLayoutsInit, defined by the header that pulls
// every layout in) rather than by static initialisers in each layout header.
// Static-init order across headers is exactly the kind of thing that works on
// the desktop harness and fails on the console, and there are three layouts to
// register, not thirty.
inline const GhUiLayout* g_ghUiLayouts[8] = {};
inline int               g_ghUiLayoutCount = 0;

inline void GhUiLayoutAdd(const GhUiLayout* l)
{
    if (!l || !l->id || g_ghUiLayoutCount >= 8) return;
    for (int i = 0; i < g_ghUiLayoutCount; ++i)
        if (std::strcmp(g_ghUiLayouts[i]->id, l->id) == 0) return;   // idempotent
    g_ghUiLayouts[g_ghUiLayoutCount++] = l;
}

inline const GhUiLayout* GhUiLayoutFind(const char* id)
{
    if (!id || !*id) return nullptr;
    for (int i = 0; i < g_ghUiLayoutCount; ++i)
        if (std::strcmp(g_ghUiLayouts[i]->id, id) == 0) return g_ghUiLayouts[i];
    return nullptr;
}

// The layout the active theme asks for. A theme naming a layout this build does
// not have still renders -- in the first registered layout, which is `list` --
// because a theme that arrived from the repository must never be able to leave
// the console with a blank screen.
inline const GhUiLayout* GhUiLayoutActive()
{
    if (g_ghUiLayoutCount <= 0) return nullptr;
    if (const GhUiLayout* l = GhUiLayoutFind(GhThemeCurrent().layout)) return l;
    return g_ghUiLayouts[0];
}

// ---------------------------------------------------------------------------
// Page entry points -- what a page's controller actually calls
// ---------------------------------------------------------------------------
// Each one paints a whole frame: background, chrome, body. The caller owns
// BeginDraw/EndDraw, because the launcher draws a page and then sometimes a
// modal over it inside one frame.
inline void GhUiPageList(const GhUiListVM& vm)
{
    const GhUiLayout* l = GhUiLayoutActive();
    if (!l || !l->list) return;
    GhUiBackground();
    l->list(vm, l->chrome ? l->chrome(vm.chrome) : GhUiFrame());
}

inline void GhUiPageGrid(const GhUiListVM& vm)
{
    const GhUiLayout* l = GhUiLayoutActive();
    if (!l) return;
    GhUiBackground();
    const D2D1_RECT_F body = l->chrome ? l->chrome(vm.chrome) : GhUiFrame();
    if (l->grid) l->grid(vm, body);
    else if (l->list) l->list(vm, body);
}

inline void GhUiPageModal(const GhUiModalVM& vm)
{
    const GhUiLayout* l = GhUiLayoutActive();
    if (!l || !l->modal) return;
    // A modal drawn over a page scrims it; one that owns the screen paints the
    // background itself. The launcher's uninstall prompt is the second kind.
    if (vm.overPage) GhUiScrim();
    else             GhUiBackground();
    l->modal(vm);
}

inline void GhUiPageQr(const GhUiQrVM& vm)
{
    const GhUiLayout* l = GhUiLayoutActive();
    if (!l || !l->qr) return;
    GhUiBackground();
    l->qr(vm, l->chrome ? l->chrome(vm.chrome) : GhUiFrame());
}

inline void GhUiPageProgress(const GhUiProgressVM& vm)
{
    const GhUiLayout* l = GhUiLayoutActive();
    if (!l || !l->progress) return;
    GhUiBackground();
    l->progress(vm, l->chrome ? l->chrome(vm.chrome) : GhUiFrame());
}

inline void GhUiPageMessage(const GhUiMessageVM& vm)
{
    const GhUiLayout* l = GhUiLayoutActive();
    if (!l || !l->message) return;
    GhUiBackground();
    l->message(vm, l->chrome ? l->chrome(vm.chrome) : GhUiFrame());
}
