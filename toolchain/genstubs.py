#!/usr/bin/env python3
"""Emit src/loader/x86/gh_x86_stubs.h from the classification below.

Run from anywhere:  python3 toolchain/genstubs.py
"""
import os

# Each entry is (name, return-expression). The expression is what the stub
# answers; it is NOT a forward. A dynamic forward that fails to resolve on the
# console registers nothing at all, which puts the name straight back to being
# a fatal unimplemented import -- so anything that needs real behaviour is a
# hand-written bed in gh_x86_beds.h, which registers first and wins.
S_FALSE   = '0'
S_TRUE    = '1'
E_NOTIMPL = '0x80004001u'          # E_NOTIMPL
SEC_UNSUP = '0x80090302u'          # SEC_E_UNSUPPORTED_FUNCTION
SOCK_ERR  = '(uint32_t)-1'         # SOCKET_ERROR
NOT_SUP   = '50'                   # ERROR_NOT_SUPPORTED

T = [
 # ---- advapi32 ---------------------------------------------------------
 ('advapi32.dll', [
   ('ConvertSidToStringSidW', S_FALSE),
   ('CreateProcessAsUserW',   S_FALSE),
   ('CryptContextAddRef',     S_FALSE),
   ('CryptExportKey',         S_FALSE),
   ('CryptGenKey',            S_FALSE),
   ('CryptGetKeyParam',       S_FALSE),
   ('CryptGetUserKey',        S_FALSE),
   ('CryptSetHashParam',      S_FALSE),
   ('CryptSignHashW',         S_FALSE),
   ('GetTraceEnableFlags',    S_FALSE),
   ('GetTraceEnableLevel',    S_FALSE),
   ('GetTraceLoggerHandle',   S_FALSE),
   ('InitializeSecurityDescriptor', S_TRUE),
   ('RegEnumKeyExW',          '259'),     # ERROR_NO_MORE_ITEMS
   ('RegQueryInfoKeyW',       '120'),     # ERROR_CALL_NOT_IMPLEMENTED
   ('RegisterTraceGuidsW',    NOT_SUP),
   ('SetSecurityDescriptorDacl', S_TRUE),
   ('TraceEvent',             NOT_SUP),
   ('UnregisterTraceGuids',   '0'),
 ]),
 # ---- comctl32 / comdlg32 ---------------------------------------------
 ('comctl32.dll', [ ('InitCommonControlsEx', S_TRUE) ]),
 ('comdlg32.dll', [ ('GetSaveFileNameW', S_FALSE), ('PrintDlgExW', '0x80004005u') ]),
 # ---- crypt32: every one fails, which is the honest answer -------------
 ('crypt32.dll', [(n, S_FALSE) for n in [
   'CertAddCertificateContextToStore','CertAddEncodedCertificateToStore',
   'CertAddSerializedElementToStore','CertCloseStore','CertCreateCertificateChainEngine',
   'CertDuplicateCertificateContext','CertFindChainInStore','CertFindExtension',
   'CertFreeCertificateChain','CertFreeCertificateChainEngine','CertFreeCertificateContext',
   'CertGetCertificateChain','CertGetCertificateContextProperty','CertGetIntendedKeyUsage',
   'CertGetPublicKeyLength','CertOpenStore','CertOpenSystemStoreW','CertRDNValueToStrA',
   'CertSerializeCertificateStoreElement','CertSetCertificateContextProperty',
   'CertVerifyCertificateChainPolicy','CertVerifyTimeValidity',
   'CryptAcquireCertificatePrivateKey','CryptDecodeObjectEx','CryptFindOIDInfo',
   'CryptHashCertificate']]),
 # ---- gdi32: our HDC/HGDIOBJ are synthetic, so nothing here may forward -
 ('gdi32.dll', [
   ('AbortPath', S_TRUE), ('BeginPath', S_TRUE), ('EndPath', S_TRUE),
   ('StrokePath', S_TRUE), ('StrokeAndFillPath', S_TRUE), ('PathToRegion', S_FALSE),
   ('AddFontMemResourceEx', S_FALSE), ('RemoveFontMemResourceEx', S_TRUE),
   ('CreateRectRgnIndirect', S_FALSE), ('ExtCreatePen', S_FALSE),
   ('EndDoc', S_FALSE), ('EndPage', S_FALSE), ('StartDocW', S_FALSE),
   ('StartPage', S_FALSE), ('SetAbortProc', S_FALSE),
   ('EnumFontFamiliesExW', S_FALSE), ('EnumFontFamiliesW', S_FALSE),
   ('GdiAlphaBlend', S_FALSE), ('GdiFlush', S_TRUE),
   ('GetCharWidthI', S_FALSE), ('GetClipRgn', S_FALSE), ('GetCurrentObject', S_FALSE),
   ('GetFontData', '(uint32_t)-1'),     # GDI_ERROR
   ('GetGlyphIndicesW', '(uint32_t)-1'),
   ('GetObjectType', S_FALSE), ('GetObjectW', S_FALSE),
   ('GetOutlineTextMetricsW', S_FALSE), ('GetStretchBltMode', S_FALSE),
   ('GetTextFaceW', S_FALSE),
   ('IntersectClipRect', '3'),          # SIMPLEREGION: the clip is "everything"
   ('SelectClipRgn', '3'), ('OffsetRgn', '3'),
   ('PolyBezier', S_FALSE),
   ('RestoreDC', S_FALSE), ('SaveDC', S_FALSE),
   ('SetArcDirection', S_FALSE), ('SetBrushOrgEx', S_FALSE),
   ('SetDCBrushColor', '(uint32_t)-1'), ('SetDCPenColor', '(uint32_t)-1'),
   ('SetDIBits', S_FALSE), ('SetDIBitsToDevice', S_FALSE), ('StretchDIBits', S_FALSE),
   ('SetGraphicsMode', S_FALSE), ('SetMiterLimit', S_FALSE),
   ('SetPolyFillMode', S_FALSE), ('SetROP2', S_FALSE), ('SetWorldTransform', S_FALSE),
 ]),
 ('imm32.dll',    [ ('ImmGetIMEFileNameA', S_FALSE), ('ImmIsIME', S_FALSE) ]),
 ('iphlpapi.dll', [ ('CancelIPChangeNotify', S_FALSE), ('NotifyAddrChange', NOT_SUP) ]),
 # ---- kernel32 ---------------------------------------------------------
 ('kernel32.dll', [
   ('AssignProcessToJobObject', S_FALSE),
   ('CreateProcessW',    S_FALSE),
   ('FatalAppExitA',     '0'),
   ('GetCurrencyFormatW', S_FALSE), ('GetNumberFormatW', S_FALSE),
   ('GetDiskFreeSpaceExW', S_FALSE), ('GetDiskFreeSpaceW', S_FALSE),
   # A console the guest never had: reporting no buffer makes every caller
   # fall back to its own default width, and colouring it is a no-op.
   ('GetConsoleScreenBufferInfo', S_FALSE), ('SetConsoleTextAttribute', S_TRUE),
   ('GetLogicalDrives',  '0xCu'),       # C: and D:, which is what the VFS shows
   ('GetLogicalDriveStringsW', S_FALSE),
   ('GetLongPathNameW',  S_FALSE),
   ('GetPrivateProfileStringW', S_FALSE),
   ('GetTempFileNameA',  S_FALSE),
   ('GetThreadLocale',   '0x409u'), ('GetUserDefaultLangID', '0x409u'),
   ('GetVolumeInformationA', S_FALSE),
   ('GlobalAddAtomW',    '0xC100u'), ('GlobalDeleteAtom', S_FALSE),
   ('LockFile',   S_TRUE), ('LockFileEx',   S_TRUE),
   ('UnlockFile', S_TRUE), ('UnlockFileEx', S_TRUE),
   ('Module32First', S_FALSE),
   ('MoveFileA', S_FALSE),
   ('PostQueuedCompletionStatus', S_FALSE),
   ('QueueUserWorkItem', S_FALSE),
   ('RegisterWaitForSingleObject', S_FALSE),
   ('RtlCaptureStackBackTrace', S_FALSE),
   ('SetCurrentDirectoryW', S_TRUE),    # the cwd must stay the game dir
   ('TzSpecificLocalTimeToSystemTime', S_FALSE),
 ]),
 ('msimg32.dll', [ ('AlphaBlend', S_FALSE) ]),
 ('ole32.dll',   [ ('DoDragDrop', E_NOTIMPL), ('OleDuplicateData', S_FALSE),
                   ('OleUninitialize', '0') ]),
 ('psapi.dll',   [ ('GetProcessMemoryInfo', S_FALSE) ]),
 ('secur32.dll', [
   ('AcquireCredentialsHandleW', SEC_UNSUP), ('DecryptMessage', SEC_UNSUP),
   ('EncryptMessage', SEC_UNSUP), ('InitializeSecurityContextW', SEC_UNSUP),
   ('QueryContextAttributesW', SEC_UNSUP), ('QuerySecurityPackageInfoW', SEC_UNSUP),
   ('DeleteSecurityContext', '0'), ('FreeContextBuffer', '0'),
   ('FreeCredentialsHandle', '0'),
 ]),
 ('setupapi.dll', [(n, S_FALSE) for n in [
   'SetupDiBuildDriverInfoList','SetupDiDestroyDriverInfoList','SetupDiEnumDriverInfoW',
   'SetupDiGetDeviceInstallParamsW','SetupDiSetDeviceInstallParamsW']]),
 ('shell32.dll',  [ ('SHGetSpecialFolderPathW', S_FALSE) ]),
 ('shlwapi.dll',  [
   ('PathFileExistsA', S_FALSE),        # must go through the VFS, not the host FS
   ('PathGetCharTypeW', S_FALSE), ('PathIsUNCA', S_FALSE), ('PathIsUNCW', S_FALSE),
   ('UrlCreateFromPathA', '0x80004005u'), ('UrlCreateFromPathW', '0x80004005u'),
 ]),
 ('urlmon.dll',   [ ('CoInternetCreateSecurityManager', E_NOTIMPL) ]),
 # ---- user32: no entry that takes an HWND may forward ------------------
 ('user32.dll', [
   ('AttachThreadInput', S_TRUE),
   ('CallMsgFilterW', S_FALSE),
   ('CreateCaret', S_TRUE), ('DestroyCaret', S_TRUE), ('SetCaretPos', S_TRUE),
   ('CreatePopupMenu', S_FALSE), ('DestroyMenu', S_TRUE),
   ('InsertMenuItemW', S_TRUE), ('TrackPopupMenu', S_FALSE),
   ('DrawEdge', S_FALSE), ('DrawFocusRect', S_FALSE), ('DrawFrameControl', S_FALSE),
   ('FrameRect', S_FALSE), ('InvertRect', S_FALSE), ('ScrollDC', S_FALSE),
   ('EnumThreadWindows', S_FALSE),
   ('FindWindowExA', S_FALSE),
   ('GetCapture', S_FALSE), ('GetCursor', S_FALSE),
   ('GetClassNameW', S_FALSE), ('RealGetWindowClassW', S_FALSE),
   ('GetGuiResources', S_FALSE), ('GetQueueStatus', S_FALSE),
   ('GetSysColorBrush', S_FALSE),
   ('MessageBeep', S_TRUE),
   ('RedrawWindow', S_TRUE),
   ('RegisterClipboardFormatW', '0xC200u'), ('RegisterWindowMessageW', '0xC001u'),
   ('SendInput', S_FALSE),
   ('SetClassLongW', S_FALSE), ('SetParent', S_FALSE),
 ]),
 ('userenv.dll',  [ ('CreateEnvironmentBlock', S_FALSE), ('DestroyEnvironmentBlock', S_FALSE) ]),
 ('usp10.dll',    [ ('ScriptFreeCache', '0') ] +
                  [(n, E_NOTIMPL) for n in ['ScriptCPtoX','ScriptGetFontProperties',
                   'ScriptItemize','ScriptJustify','ScriptPlace','ScriptShape','ScriptXtoCP']]),
 ('version.dll',  [ ('VerQueryValueW', S_FALSE) ]),
 ('winhttp.dll',  [ ('WinHttpOpen', S_FALSE), ('WinHttpCloseHandle', S_TRUE),
                    ('WinHttpGetProxyForUrl', S_FALSE), ('WinHttpSetTimeouts', S_TRUE) ]),
 ('winmm.dll',    [ ('PlaySoundA', S_FALSE), ('waveInMessage', '8') ]),  # MMSYSERR_NOTSUPPORTED
 ('winspool.drv', [ ('ClosePrinter', S_TRUE), ('GetPrinterW', S_FALSE),
                    ('OpenPrinterW', S_FALSE) ]),
 ('ws2_32.dll',   [ ('gethostbyaddr', S_FALSE), ('getservbyname', S_FALSE),
                    ('getservbyport', S_FALSE), ('WSAGetOverlappedResult', S_FALSE),
                    ('WSALookupServiceBeginW', SOCK_ERR), ('WSALookupServiceEnd', SOCK_ERR),
                    ('WSALookupServiceNextW', SOCK_ERR),
                    ('WSARecv', SOCK_ERR), ('WSASend', SOCK_ERR), ('WSASendTo', SOCK_ERR) ]),
 # ---- the Adobe AIR runtime (Super Fancy Pants Adventure) ----------------
 # What Adobe AIR.dll binds and a game never needs on a console: menus, DDE,
 # the clipboard, printing, certificate stores, colour management, named
 # pipes, tray icons. Each is the failure the real API gives when the thing is
 # unavailable. The resource API and the string helpers it also binds are real
 # beds (RegisterAirGapBeds in gh_x86_beds.h), as is GetVolumeInformationW.
 ('kernel32.dll', [
   ('QueueUserAPC', S_FALSE),
   ('SetNamedPipeHandleState', S_FALSE), ('ConnectNamedPipe', S_FALSE),
   ('CreateNamedPipeA', '(uint32_t)-1'), ('CreateNamedPipeW', '(uint32_t)-1'),   # INVALID_HANDLE_VALUE
   ('BeginUpdateResourceW', S_FALSE), ('UpdateResourceW', S_FALSE), ('EndUpdateResourceW', S_FALSE),
   ('GetComputerNameExW', S_FALSE), ('GenerateConsoleCtrlEvent', S_FALSE),
 ]),
 ('user32.dll', [
   ('UpdateLayeredWindow', S_FALSE), ('CreateMenu', S_FALSE), ('SetMenu', S_FALSE),
   ('SetMenuItemInfoW', S_FALSE), ('GetMenuItemInfoW', S_FALSE), ('RemoveMenu', S_FALSE),
   ('DeleteMenu', S_FALSE), ('SetMenuInfo', S_FALSE), ('GetMenuInfo', S_FALSE),
   ('GetMenuBarInfo', S_FALSE), ('GetSystemMenu', S_FALSE), ('DrawMenuBar', S_TRUE),
   ('DdeInitializeW', '0x400F'),                                  # DMLERR_SYS_ERROR
   ('DdeUninitialize', S_TRUE), ('DdeConnect', S_FALSE), ('DdeDisconnect', S_TRUE),
   ('DdeClientTransaction', S_FALSE), ('DdeCreateStringHandleA', S_FALSE), ('DdeFreeStringHandle', S_TRUE),
   ('ToAscii', S_FALSE), ('ShowCaret', S_TRUE), ('IsZoomed', S_FALSE), ('SetWindowPlacement', S_TRUE),
   ('FindWindowExW', S_FALSE), ('RegisterClipboardFormatA', S_FALSE), ('GetClipboardFormatNameA', S_FALSE),
   ('WaitForInputIdle', '(uint32_t)-1'),                          # WAIT_FAILED: no such process
   ('CloseWindow', S_TRUE), ('SendMessageTimeoutW', S_FALSE),
   ('GetDlgItemTextW', S_FALSE), ('GetDlgItemTextA', S_FALSE),
 ]),
 ('ole32.dll', [
   ('CreateBindCtx', E_NOTIMPL), ('MkParseDisplayName', E_NOTIMPL),
   ('OleFlushClipboard', '0x800401D0u'), ('OleGetClipboard', '0x800401D0u'),   # CLIPBRD_E_CANT_OPEN
   ('OleSetClipboard', '0x800401D0u'), ('OleIsCurrentClipboard', '1'),          # S_FALSE
   ('CoFreeUnusedLibraries', '0'), ('CreateStreamOnHGlobal', E_NOTIMPL),
   ('CoMarshalInterThreadInterfaceInStream', E_NOTIMPL), ('CoGetInterfaceAndReleaseStream', E_NOTIMPL),
 ]),
 ('crypt32.dll', [(n, S_FALSE) for n in [
   'CertGetEnhancedKeyUsage','CryptFindLocalizedName','CryptImportPublicKeyInfo',
   'CryptProtectData','CryptUnprotectData','CertFindCertificateInStore',
   'CertVerifySubjectCertificateContext','CryptVerifyMessageSignature',
   'CryptGetMessageCertificates','CertEnumCertificatesInStore','CertCreateCertificateContext']]),
 ('winspool.drv', [ ('EnumPrintersW', S_FALSE) ]),
 ('winmm.dll', [
   ('timeSetEvent', S_FALSE), ('timeKillEvent', '11'),           # MMSYSERR_INVALPARAM
   ('waveInGetPosition', '5'), ('waveInStop', '5'),               # MMSYSERR_INVALHANDLE
   ('mixerGetID', '6'), ('waveOutMessage', '6'),                  # MMSYSERR_NODRIVER
 ]),
 ('oleacc.dll', [ ('AccessibleObjectFromWindow', '0x80004005u') ]),
 ('gdi32.dll', [
   ('GetTextCharacterExtra', '0x80000000u'), ('SetTextCharacterExtra', '0x80000000u'),
   ('GetBkMode', S_FALSE), ('GetTextAlign', S_FALSE), ('EnumFontFamiliesA', S_TRUE),
   ('LineTo', S_TRUE), ('PolyBezierTo', S_TRUE), ('FillPath', S_TRUE), ('SelectClipPath', S_FALSE),
   ('GetClipBox', S_FALSE), ('SelectPalette', S_FALSE), ('RealizePalette', '(uint32_t)-1'),
   ('GetSystemPaletteEntries', S_FALSE), ('GetICMProfileA', S_FALSE),
   ('CreateDCA', S_FALSE), ('CreateICW', S_FALSE), ('ResetDCW', S_FALSE),
   ('GetWorldTransform', S_FALSE), ('DPtoLP', S_TRUE), ('LPtoDP', S_TRUE),
   ('AbortDoc', '(uint32_t)-1'),                                  # SP_ERROR
 ]),
 ('advapi32.dll', [
   ('SetEntriesInAclW', '120'), ('SetSecurityInfo', '120'),       # ERROR_CALL_NOT_IMPLEMENTED
   ('CryptEncrypt', S_FALSE), ('GetSidSubAuthorityCount', S_FALSE), ('IsValidSid', S_FALSE),
   ('SetSecurityDescriptorControl', S_FALSE), ('MakeSelfRelativeSD', S_FALSE), ('InitializeAcl', S_FALSE),
 ]),
 ('comdlg32.dll', [ ('PrintDlgW', S_FALSE), ('PageSetupDlgW', S_FALSE),
                    ('GetOpenFileNameW', S_FALSE), ('CommDlgExtendedError', '0') ]),
 ('shell32.dll', [
   ('ShellExecuteExW', S_FALSE), ('SHChangeNotify', '0'), ('Shell_NotifyIconW', S_FALSE),
   ('SHGetSpecialFolderLocation', '0x80004005u'), ('SHGetFolderLocation', '0x80004005u'),
   ('SHGetDiskFreeSpaceExW', S_FALSE), ('SHAppBarMessage', S_FALSE), ('SHGetSettings', '0'),
   ('SHBrowseForFolderW', S_FALSE),
 ]),
 ('shlwapi.dll', [ ('AssocQueryStringW', '0x80070483u') ]),     # HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION)
 ('urlmon.dll', [ ('CopyStgMedium', E_NOTIMPL), ('CoInternetCompareUrl', '1'),   # S_FALSE: not equal
                  ('CoInternetParseUrl', '0x800C0011u') ]),      # INET_E_DEFAULT_ACTION
 ('comctl32.dll', [ ('ImageList_GetIconSize', S_FALSE), ('ImageList_Draw', S_FALSE) ]),
 ('mscms.dll', [(n, S_FALSE) for n in [
   'OpenColorProfileW','CloseColorProfile','CreateColorTransformW','DeleteColorTransform',
   'TranslateBitmapBits']]),
 ('iphlpapi.dll', [ ('GetAdaptersAddresses', '232') ]),          # ERROR_NO_DATA
]

HEAD = '''// gh_x86_stubs.h -- the imports a 32-bit guest binds but that nothing here
// implements, answered instead of being fatal.
//
// GENERATED by toolchain/genstubs.py from the list inside it. Regenerate; do
// not edit the table by hand.
//
// WHY THIS EXISTS. An import with no host function behind it is fatal the
// moment the guest calls it (gh_x86_win.h: "unimplemented import ..."), and the
// guest dies with 0xC0000139. That rule is right while a surface is being
// discovered -- it names what is missing instead of jumping to zero -- but it
// turns discovery into one deploy per name. Half-Life 1's gameui.dll statically
// imports chromehtml.dll, which imports libcef.dll: Chromium, which alone binds
// 633 imports, 225 of them names no console has. At ~25 minutes a deploy, that
// is weeks of serial bisection to learn which of the 225 are ever CALLED.
//
// So each of them gets an answer here, and the log says which ones the guest
// actually reached. The answers are chosen to be the honest failure, not a
// convenient success: a certificate store that cannot be opened, a printer that
// cannot be opened, an SSPI package that is not supported. Where failing would
// send a caller round a retry loop -- a file lock nothing else can contend for,
// a caret, a menu teardown -- the answer is success.
//
// THE STACK IS THE WHOLE REASON THIS IS GENERATED. A 32-bit stdcall callee pops
// its own arguments, so a stub with a guessed argument count silently corrupts
// the caller's stack: the crash lands somewhere else entirely, minutes later.
// Every entry below takes its pop size from GH_X86_POP(name) -- decltype of the
// REAL declaration in the Windows SDK headers -- so the size cannot be wrong,
// and a name the headers do not declare simply fails to compile here.
//
// Registered LAST, after the beds and the generated forward table, because
// ThunkRegistry::add is first-registration-wins: anything genuinely implemented
// keeps its implementation and the stub is dropped.

#pragma once

#include "gh_x86_beds.h"

// Headers for the declarations the bed table did not already need. They exist
// only so GH_X86_POP can read a real prototype.
#define SECURITY_WIN32
#include <aclapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ddeml.h>
#include <evntrace.h>
#include <icm.h>
#include <iphlpapi.h>
#include <ole2.h>
#include <oleacc.h>
#include <psapi.h>
#include <sddl.h>
#include <security.h>
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>
#include <userenv.h>
#include <usp10.h>
#include <winhttp.h>
#include <winspool.h>

namespace ghx86 {

// One line the first time a stub is actually reached, and a count at teardown.
// This is the point of the file: the log stops being a list of what a module
// BINDS and becomes a list of what it CALLS.
inline void stub_called(const char* who)
{
    static std::set<std::string> told;
    static int n = 0;
    if (told.insert(who).second)
        log(std::string("X86 STUB CALLED: ") + who +
            "  <-- answered, not implemented (" + std::to_string(++n) + " so far)");
}

#define GH_X86_STUB(REG, DLL, FUNC, RET)                                        \\
    GH_X86_BED(REG, DLL, #FUNC, GH_X86_POP(FUNC),                               \\
        [](Machine&, const uint32_t*) -> uint64_t {                             \\
            stub_called(DLL "!" #FUNC);                                         \\
            return (uint64_t)(uint32_t)(RET);                                   \\
        })

inline void RegisterStubBeds(Machine& m)
{
    ThunkRegistry& R = m.registry;
    (void)R;
'''

TAIL = '''}

#undef GH_X86_STUB

}   // namespace ghx86
'''

out = [HEAD]
nf = ns = 0
for dll, names in T:
    out.append('\n    // ---- %s ----\n' % dll)
    for name, ret in names:
        assert ret != 'F', name
        out.append('    GH_X86_STUB(R, "%s", %s, %s);\n' % (dll, name, ret)); ns += 1
out.append(TAIL)
open(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src/loader/x86/gh_x86_stubs.h'), 'w').write(''.join(out))
print('stubs', ns)
