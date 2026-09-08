// Greenhouse #164: a stand-in for tbs.dll (TPM Base Services).
//
// The Xbox One's system image does not ship tbs.dll -- LoadLibraryW("tbs.dll")
// returns 0x7E from every search path we can reach. Sony's PsPcSdk.dll imports
// four TBS entry points STATICALLY, so its own load fails before a single line
// of its code runs, and Ghost of Tsushima treats that as fatal.
//
// The honest answer to "is there a TPM here?" is no, so every entry point
// returns TBS_E_TPM_NOT_FOUND. That is a value the SDK's own error path is
// written for. What we must NOT do is what MakeStub would have done -- return
// 0, which is TBS_SUCCESS, leaving the caller's out-parameters untouched and
// its uninitialised stack buffers treated as real data.
//
// Every out-parameter is written before returning, for the same reason.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>


typedef UINT32 TBS_RESULT;
typedef PVOID  TBS_HCONTEXT;

__declspec(dllexport) TBS_RESULT WINAPI
Tbsi_Context_Create(PVOID pContextParams, TBS_HCONTEXT* phContext)
{
    (void)pContextParams;
    if (phContext) *phContext = 0;
    return TBS_E_TPM_NOT_FOUND;
}

__declspec(dllexport) TBS_RESULT WINAPI
Tbsip_Context_Close(TBS_HCONTEXT hContext)
{
    (void)hContext;
    return TBS_E_TPM_NOT_FOUND;
}

__declspec(dllexport) TBS_RESULT WINAPI
Tbsi_GetDeviceInfo(UINT32 Size, PVOID Info)
{
    // A caller that ignores the return code must still not read uninitialised
    // bytes back out of its own stack.
    if (Info && Size) SecureZeroMemory(Info, Size);
    return TBS_E_TPM_NOT_FOUND;
}

__declspec(dllexport) TBS_RESULT WINAPI
Tbsi_Get_TCG_Log(TBS_HCONTEXT hContext, PBYTE pOutputBuf, PUINT32 pOutputBufLen)
{
    (void)hContext; (void)pOutputBuf;
    // The length matters most: PsPcSdk sizes a stack buffer from it, and a
    // garbage length is a stack buffer overrun (which is exactly the
    // __GSHandlerCheck recursion this DLL exists to stop).
    if (pOutputBufLen) *pOutputBufLen = 0;
    return TBS_E_TPM_NOT_FOUND;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
