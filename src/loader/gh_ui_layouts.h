// gh_ui_layouts.h -- every structural layout this build has, and the registry.
//
// Include THIS, not the individual layout headers: the launcher and the preview
// harness must agree on the set, and a build that saw only two of the three
// would quietly fall back to `list` for every theme that asks for the third --
// a theme looking wrong is much harder to notice than a link error.
//
// ORDER MATTERS. GhUiLayoutActive() falls back to g_ghUiLayouts[0] when a theme
// names a layout this build does not have, which is the guard that stops a
// repository theme from blanking the screen. `list` is registered first because
// it is the layout that cannot be wrong: it implements every archetype, it is
// what the pages were written against, and it is the one we have run on a TV.
#pragma once

#include "gh_ui_layout.h"
#include "gh_ui_layout_list.h"
#include "gh_ui_layout_blades.h"
#include "gh_ui_layout_grid.h"
#include "gh_ui_layout_cabinet.h"

// Explicit, and called from LuiInit and from the harness -- deliberately not
// static initialisers in each header. Registration order would then depend on
// translation-unit order, and the fallback above depends on it.
inline void GhUiLayoutsInit()
{
    GhUiLayoutAdd(&kGhUiLayoutList);     // first: the fallback
    GhUiLayoutAdd(&kGhUiLayoutBlades);
    GhUiLayoutAdd(&kGhUiLayoutGrid);
    GhUiLayoutAdd(&kGhUiLayoutCabinet);
}
