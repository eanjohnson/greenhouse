// Stub opengl32.dll for Greenhouse guests that IMPORT opengl32 for capability
// probing but don't actually need working OpenGL.
//
// UnityPlayer.dll on Windows always imports six wgl functions at link time --
// wglCreateContext etc. -- and uses them to probe whether an OpenGL context is
// available. If it can't create a GL context it falls back to D3D11, which is
// exactly what we want (Unity 2022 uses D3D11 by default anyway; we route it
// through the D3D11On12 bridge).
//
// The Xbox UWP app container does not ship opengl32.dll at all, so
// UnityPlayer.dll fails to load with ERROR_MOD_NOT_FOUND before its DllMain
// even runs. This stub satisfies the loader's dependency check; every entry
// point returns NULL/FALSE so any probe treats GL as unavailable.
#include <windows.h>

extern "C" {

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

// The six functions UnityPlayer.dll imports.
__declspec(dllexport) PROC   WINAPI wglGetProcAddress(LPCSTR)      { return nullptr; }
__declspec(dllexport) BOOL   WINAPI wglDeleteContext(HGLRC)        { return FALSE;   }
__declspec(dllexport) BOOL   WINAPI wglMakeCurrent(HDC, HGLRC)     { return FALSE;   }
__declspec(dllexport) HDC    WINAPI wglGetCurrentDC(void)          { return nullptr; }
__declspec(dllexport) HGLRC  WINAPI wglCreateContext(HDC)          { return nullptr; }
__declspec(dllexport) HGLRC  WINAPI wglGetCurrentContext(void)     { return nullptr; }

}
