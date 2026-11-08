// One api-set forwarder DLL. Every export is a forwarder written in the .def,
// so there is nothing to compile but the anchor: a forwarder resolves only
// once ucrtbase.dll is mapped, and a DLL with no imports would never cause it
// to be. Importing (and calling) the anchor is what maps it.
__declspec(dllimport) int _gh_ucrt_anchor(void);

int __stdcall DllMainCRTStartup(void* inst, unsigned reason, void* reserved)
{
    (void)inst; (void)reserved;
    if (reason == 1) return _gh_ucrt_anchor();
    return 1;
}
