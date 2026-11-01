// Host test for src/loader/gh_dsound.h (run by test_gh_dsound.sh under wine).
// Drives the emulation through the SDK's own IDirectSound8 / IDirectSoundBuffer8
// declarations, so a method out of vtable order fails here, and plays the part
// of Source's snd_dma: a looping secondary buffer written ahead of the cursor.
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

static void Diag(const std::string& s){ printf("  diag: %s\n", s.c_str()); }
static std::string Hex(uintptr_t v){ char b[32]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); return b; }
#include "../src/loader/gh_dsound.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main()
{
    IDirectSound8* ds = nullptr;
    CHECK(SUCCEEDED(ghds::Create(nullptr, (IUnknown**)&ds, nullptr)) && ds);
    IUnknown* agg = nullptr;
    CHECK(ghds::Create(nullptr, &agg, (IUnknown*)ds) == CLASS_E_NOAGGREGATION && !agg);
    IDirectSound* ds1 = nullptr;
    CHECK(SUCCEEDED(ds->QueryInterface(IID_IDirectSound, (void**)&ds1)) && ds1 == (IDirectSound*)ds);
    if (ds1) ds1->Release();
    CHECK(SUCCEEDED(ds->SetCooperativeLevel(nullptr, DSSCL_PRIORITY)));
    DSCAPS caps{}; caps.dwSize = sizeof caps;
    CHECK(SUCCEEDED(ds->GetCaps(&caps)) && caps.dwSize == sizeof caps && (caps.dwFlags & DSCAPS_PRIMARYSTEREO));
    DWORD spk = 0;
    CHECK(SUCCEEDED(ds->GetSpeakerConfig(&spk)) && spk == DSSPEAKER_STEREO);

    // Primary: format only.
    DSBUFFERDESC pd{}; pd.dwSize = sizeof pd; pd.dwFlags = DSBCAPS_PRIMARYBUFFER;
    IDirectSoundBuffer* prim = nullptr;
    CHECK(SUCCEEDED(ds->CreateSoundBuffer(&pd, &prim, nullptr)) && prim);
    WAVEFORMATEX f{ WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    CHECK(SUCCEEDED(prim->SetFormat(&f)));
    WAVEFORMATEX g{}; DWORD got = 0;
    CHECK(SUCCEEDED(prim->GetFormat(&g, sizeof g, &got)) && g.nSamplesPerSec == 44100 && got == sizeof g);
    CHECK(SUCCEEDED(prim->Play(0, 0, DSBPLAY_LOOPING)));

    // Secondary, Source's shape: 16-bit stereo 44.1k, 0x10000 bytes, GETCURRENTPOSITION2.
    DSBUFFERDESC sd{}; sd.dwSize = sizeof sd;
    sd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME;
    sd.dwBufferBytes = 0x10000; sd.lpwfxFormat = &f;
    IDirectSoundBuffer* sb = nullptr;
    CHECK(SUCCEEDED(ds->CreateSoundBuffer(&sd, &sb, nullptr)) && sb);
    IDirectSoundBuffer8* sb8 = nullptr;
    CHECK(SUCCEEDED(sb->QueryInterface(IID_IDirectSoundBuffer8, (void**)&sb8)) && sb8);
    if (sb8) sb8->Release();
    DSBCAPS bc{}; bc.dwSize = sizeof bc;
    CHECK(SUCCEEDED(sb->GetCaps(&bc)) && bc.dwBufferBytes == 0x10000);
    CHECK(prim->SetFormat(&f) == S_OK && sb->SetFormat(&f) != S_OK);   // secondaries have fixed formats

    // Lock that wraps: 0x200 bytes from 0xFF00 -> 0x100 + 0x100.
    void *p1, *p2; DWORD b1, b2;
    CHECK(SUCCEEDED(sb->Lock(0xFF00, 0x200, &p1, &b1, &p2, &b2, 0)) && b1 == 0x100 && b2 == 0x100 && p2);
    sb->Unlock(p1, b1, p2, b2);
    CHECK(SUCCEEDED(sb->Lock(0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER)) && b1 == 0x10000 && b2 == 0 && !p2);
    for (DWORD i = 0; i < b1 / 4; ++i){   // 440 Hz tone, quiet
        short v = (short)(3000 * sin(i * 2 * 3.14159265 * 440 / 44100));
        ((short*)p1)[2 * i] = v; ((short*)p1)[2 * i + 1] = v;
    }
    sb->Unlock(p1, b1, p2, b2);
    CHECK(SUCCEEDED(sb->SetVolume(-600)));
    LONG vol = 0; CHECK(SUCCEEDED(sb->GetVolume(&vol)) && vol == -600);
    CHECK(sb->SetVolume(100) == E_INVALIDARG);

    DWORD st = 99;
    CHECK(SUCCEEDED(sb->GetStatus(&st)) && st == 0);
    CHECK(SUCCEEDED(sb->Play(0, 0, DSBPLAY_LOOPING)));
    CHECK(SUCCEEDED(sb->GetStatus(&st)) && st == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING));

    // The mixer loop: the play cursor must advance, wrap, and stay behind the write cursor.
    DWORD last = 0, travelled = 0, wraps = 0;
    for (int i = 0; i < 120; ++i){   // ~1.2 s, the buffer is ~0.37 s
        Sleep(10);
        DWORD play = 0, write = 0;
        CHECK(SUCCEEDED(sb->GetCurrentPosition(&play, &write)));
        CHECK(play < 0x10000 && write < 0x10000 && play % 4 == 0);
        const DWORD ahead = (write + 0x10000 - play) % 0x10000;
        CHECK(ahead <= 4 * (44100 * 4 / 100) + 4);   // at most the queued chunks
        travelled += (play + 0x10000 - last) % 0x10000;
        if (play < last) ++wraps;
        last = play;
        // Write ahead from the write cursor, as Source does.
        if (SUCCEEDED(sb->Lock(0, 1764, &p1, &b1, &p2, &b2, DSBLOCK_FROMWRITECURSOR))) sb->Unlock(p1, b1, p2, b2);
    }
    printf("  looping: travelled %lu bytes (%.2f s), %lu wraps\n", travelled, travelled / 176400.0, wraps);
    CHECK(travelled > 176400 / 2 && wraps >= 2);

    CHECK(SUCCEEDED(sb->Stop()));
    DWORD play1 = 0, play2 = 0;
    sb->GetCurrentPosition(&play1, nullptr); Sleep(50); sb->GetCurrentPosition(&play2, nullptr);
    CHECK(play1 == play2);
    CHECK(SUCCEEDED(sb->GetStatus(&st)) && st == 0);
    CHECK(SUCCEEDED(sb->SetCurrentPosition(0x1003)));
    sb->GetCurrentPosition(&play1, nullptr);
    CHECK(play1 == 0x1000);

    // One-shot: a short buffer played once ends on its own.
    sd.dwFlags = DSBCAPS_GETCURRENTPOSITION2; sd.dwBufferBytes = 44100 * 4 / 10;   // 0.1 s
    IDirectSoundBuffer* one = nullptr;
    CHECK(SUCCEEDED(ds->CreateSoundBuffer(&sd, &one, nullptr)) && one);
    CHECK(SUCCEEDED(one->Play(0, 0, 0)));
    int ms = 0;
    for (; ms < 2000; ms += 10){ one->GetStatus(&st); if (!(st & DSBSTATUS_PLAYING)) break; Sleep(10); }
    printf("  one-shot ended after %d ms\n", ms);
    CHECK(ms >= 50 && ms < 1000);
    CHECK(SUCCEEDED(one->Play(0, 0, 0)));   // replays after it ended
    one->GetStatus(&st); CHECK(st & DSBSTATUS_PLAYING);

    CHECK(one->Release() == 0);
    CHECK(sb->Release() == 0);
    CHECK(prim->Release() == 0);
    CHECK(ds->Release() == 0);
    printf(g_fail ? "%d FAILED\n" : "gh_dsound: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
