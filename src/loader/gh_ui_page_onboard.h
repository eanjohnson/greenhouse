// gh_ui_page_onboard.h -- the first-run sequence, as content plus a marker.
//
// Like gh_ui_page_themes.h (§63d), this page's CONTENT needs no launcher state:
// it is four paragraphs and a step counter. So it lives here, the preview
// harness renders the REAL page rather than a fixture of it, and launcher_ui.h
// owns nothing but the input loop and the marker write.
//
// What onboarding does NOT do is drive other pages. It would be easy to make
// step 3 open the sign-in flow and step 4 open Themes, and it would mean the
// first thing a new console did was move the user somewhere they did not ask to
// go, with our own QR already on screen. It tells them where both live instead,
// and the picker is one button away the whole time.
#pragma once

#include <string>

#include "gh_ui_vm.h"

// ---------------------------------------------------------------------------
// The marker
// ---------------------------------------------------------------------------
// One file in LocalState. Its presence is the whole state: there is no version
// and no per-step resume, because a sequence this short is either worth reading
// or worth skipping, and a half-finished one is neither.
//
// It is deliberately NOT a knob the loader reads at startup (loader-knob-reads
// are logged and swept); it is user state, like active.txt, and it survives an
// app update the same way the themes and input profiles beside it do.
inline const wchar_t* kGhOnbMarker = L"gh_onboarded.txt";

// ---------------------------------------------------------------------------
// The steps
// ---------------------------------------------------------------------------
struct GhOnbStep {
    const wchar_t* heading;
    const wchar_t* body;
};

// Four, in the order someone meets them: what this is, where the games come
// from, how to sign in, how to change the look. Each body is one short
// paragraph -- a first-run screen nobody reads is worse than no first-run
// screen, and the fourth step is the one that earns the other three.
inline const GhOnbStep kGhOnbSteps[] = {
    { L"Welcome to Greenhouse",
      L"Greenhouse runs PC games on this console.\n\n"
      L"Games appear in one library, you start them with A, and the Menu and "
      L"View buttons together bring up an overlay while you play." },

    { L"Your games stay yours",
      L"Greenhouse does not sell or supply games. It installs the ones you "
      L"already own, from your own Steam account, onto this console.\n\n"
      L"Nothing you do not own shows up in the library." },

    { L"Signing in to Steam",
      L"Open Packages from the picker with the Menu button and install Steam. "
      L"The next time Greenhouse starts it shows a code: scan it with the Steam "
      L"app on your phone.\n\n"
      L"You can skip this and come back to it later. Games already copied onto "
      L"the console play without signing in." },

    { L"Make it yours",
      L"Press LB on the picker for Themes. A theme changes the colours, the "
      L"type and the way each screen is laid out, and it applies the moment you "
      L"pick it.\n\n"
      L"Text size and the screen edges live on the same page, for a TV that "
      L"crops the picture." },
};

inline int GhOnbCount() { return (int)(sizeof(kGhOnbSteps) / sizeof(kGhOnbSteps[0])); }

// ---------------------------------------------------------------------------
// The view-model
// ---------------------------------------------------------------------------
// A MessagePage, not centred: these are paragraphs under a header, and the step
// counter the layouts already draw (GhUiMessageVM::stepIndex/stepCount, which
// all three have rendered since U0) is what makes a sequence feel finite.
inline GhUiMessageVM GhOnbPageVM(int step)
{
    if (step < 0) step = 0;
    if (step >= GhOnbCount()) step = GhOnbCount() - 1;
    const GhOnbStep& s = kGhOnbSteps[step];
    const bool last = (step == GhOnbCount() - 1);

    GhUiMessageVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    vm.heading.set(s.heading);
    vm.body.set(s.body);
    vm.centered  = false;
    vm.stepIndex = step;
    vm.stepCount = GhOnbCount();

    // A is always forward, so the sequence can be read by pressing one button
    // four times. B is back on every step but the first, where it is the way
    // out -- and the way out is offered from the start, not hidden on step 4.
    vm.chrome.addHint(GH_GLYPH_A, last ? L"Start using Greenhouse" : L"Next");
    vm.chrome.addHint(GH_GLYPH_B, step == 0 ? L"Skip" : L"Back");
    return vm;
}
