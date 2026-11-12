#!/usr/bin/env python3
"""PE import-table auditor.

Scans a Win32 PE's import (and delay-import) tables and classifies every
imported symbol against the Conservatory's three-tier resolution model:

  tier 1 "bed"    - intercepted by a specific Greenhouse Bed (may still call
                    through to the real export after path/data translation,
                    or fully virtualize it -- see docs/architecture/api-shims.md)
  tier 2 "runtime" - forwarded to the real on-console export via runtime
                    LoadLibraryEx/GetProcAddress resolution (the uwp2win32-
                    style technique, §4.2)
  tier 3 "stub"   - unsupported surface (GUI/shell/legacy multimedia/etc);
                    logged and stubbed, promoted to a real Bed only if it
                    shows up often enough to be worth building (§7.3)

This is a planning/triage tool: it tells you what Beds a given binary will
actually exercise *before* you try to run it, per Appendix D's corpus
methodology. It does not execute the target.
"""
import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

import pefile

# ---------------------------------------------------------------------------
# Classification rules
# ---------------------------------------------------------------------------
# Every export lands in exactly one Bed. Rules are checked in order:
#   1. exact (dll, function) override
#   2. per-DLL default
#   3. fallback: unknown DLL -> tier "stub", bed "unknown"
#
# Bed names line up with the loader's beds (docs/architecture/api-shims.md).

# (tier, bed) per DLL, used unless a function-specific override applies below.
DLL_DEFAULTS = {
    # Core NT/kernel surface: broad; most of it is runtime-resolvable.
    "kernel32.dll":  ("runtime", "memory_or_threading"),
    "kernelbase.dll": ("runtime", "memory_or_threading"),
    "ntdll.dll":      ("runtime", "memory_or_threading"),
    "api-ms-win-core-processthreads-l1-1-0.dll": ("runtime", "threading"),
    "api-ms-win-core-synch-l1-1-0.dll":          ("runtime", "threading"),
    "api-ms-win-core-memory-l1-1-0.dll":         ("runtime", "memory"),
    "api-ms-win-core-file-l1-1-0.dll":           ("bed", "vfs"),
    "api-ms-win-core-heap-l1-1-0.dll":           ("runtime", "memory"),

    # Registry -> always virtualized.
    "advapi32.dll": ("bed", "registry"),

    # Winsock -> forwarded (client sockets are usable per §3.8).
    "ws2_32.dll":   ("runtime", "winsock"),
    "wship6.dll":   ("runtime", "winsock"),
    "mswsock.dll":  ("runtime", "winsock"),

    # TLS / crypto: generally reachable at runtime.
    "secur32.dll":  ("runtime", "winsock_tls"),
    "crypt32.dll":  ("runtime", "crt"),
    "bcrypt.dll":   ("runtime", "crt"),
    "ncrypt.dll":   ("runtime", "crt"),

    # CRT / UCRT: ships as an OS component, should resolve cleanly.
    "msvcrt.dll":    ("runtime", "crt"),
    "ucrtbase.dll":  ("runtime", "crt"),
    "vcruntime140.dll": ("runtime", "crt"),
    "vcruntime140_1.dll": ("runtime", "crt"),

    # COM: partial support, best-effort runtime resolution.
    "ole32.dll":    ("runtime", "com"),
    "oleaut32.dll": ("runtime", "com"),
    "combase.dll":  ("runtime", "com"),

    # GUI / windowing / legacy multimedia -> Appendix A territory, T4+.
    "user32.dll":    ("stub", "gui_user32"),
    "gdi32.dll":     ("stub", "gui_gdi"),
    "gdi32full.dll": ("stub", "gui_gdi"),
    "gdiplus.dll":   ("stub", "gui_gdiplus"),
    "comctl32.dll":  ("stub", "gui_comctl32"),
    "comdlg32.dll":  ("stub", "gui_comdlg32"),
    "winmm.dll":     ("stub", "legacy_multimedia"),
    "shell32.dll":   ("stub", "shell"),
    "shlwapi.dll":   ("stub", "shell"),
    "uxtheme.dll":   ("stub", "gui_theme"),
    "dwmapi.dll":    ("stub", "gui_compositor"),

    # DirectX -> native on Xbox, always passthrough (not a Bed at all).
    "d3d11.dll":   ("native", "-"),
    "d3d12.dll":   ("native", "-"),
    "dxgi.dll":    ("native", "-"),
    "d2d1.dll":    ("native", "-"),
    "dwrite.dll":  ("native", "-"),
    "xaudio2_9.dll": ("native", "-"),
}

# Exact (dll, function) overrides -- for symbols whose DLL default is wrong
# for that specific call (e.g. kernel32's default is "runtime", but its
# file-I/O and process-creation exports are VFS/Process Bed territory).
FUNCTION_OVERRIDES = {
    # --- VFS Bed: file path virtualization (§3.3) ---
    ("kernel32.dll", "CreateFileW"): ("bed", "vfs"),
    ("kernel32.dll", "CreateFileA"): ("bed", "vfs"),
    ("kernel32.dll", "CreateFile2"): ("bed", "vfs"),
    ("kernel32.dll", "ReadFile"): ("bed", "vfs"),
    ("kernel32.dll", "WriteFile"): ("bed", "vfs"),
    ("kernel32.dll", "DeleteFileW"): ("bed", "vfs"),
    ("kernel32.dll", "MoveFileW"): ("bed", "vfs"),
    ("kernel32.dll", "MoveFileExW"): ("bed", "vfs"),
    ("kernel32.dll", "CopyFileW"): ("bed", "vfs"),
    ("kernel32.dll", "FindFirstFileW"): ("bed", "vfs"),
    ("kernel32.dll", "FindFirstFileExW"): ("bed", "vfs"),
    ("kernel32.dll", "FindNextFileW"): ("bed", "vfs"),
    ("kernel32.dll", "FindClose"): ("bed", "vfs"),
    ("kernel32.dll", "GetFileAttributesW"): ("bed", "vfs"),
    ("kernel32.dll", "GetFileAttributesExW"): ("bed", "vfs"),
    ("kernel32.dll", "SetFileAttributesW"): ("bed", "vfs"),
    ("kernel32.dll", "GetFileSizeEx"): ("bed", "vfs"),
    ("kernel32.dll", "SetFilePointer"): ("bed", "vfs"),
    ("kernel32.dll", "SetFilePointerEx"): ("bed", "vfs"),
    ("kernel32.dll", "FlushFileBuffers"): ("bed", "vfs"),
    ("kernel32.dll", "CreateDirectoryW"): ("bed", "vfs"),
    ("kernel32.dll", "RemoveDirectoryW"): ("bed", "vfs"),
    ("kernel32.dll", "GetFullPathNameW"): ("bed", "vfs"),
    ("kernel32.dll", "GetTempPathW"): ("bed", "vfs"),
    ("kernel32.dll", "GetCurrentDirectoryW"): ("bed", "vfs"),
    ("kernel32.dll", "SetCurrentDirectoryW"): ("bed", "vfs"),
    ("kernel32.dll", "LockFile"): ("bed", "vfs"),
    ("kernel32.dll", "LockFileEx"): ("bed", "vfs"),
    ("kernel32.dll", "UnlockFile"): ("bed", "vfs"),
    ("kernel32.dll", "UnlockFileEx"): ("bed", "vfs"),
    ("kernel32.dll", "GetDriveTypeW"): ("bed", "vfs"),
    ("kernel32.dll", "GetLogicalDrives"): ("bed", "vfs"),
    ("kernel32.dll", "CreateFileMappingW"): ("bed", "vfs"),
    ("kernel32.dll", "MapViewOfFile"): ("bed", "vfs"),

    # --- Process Bed: single-process surrogate (§3.5) ---
    ("kernel32.dll", "CreateProcessW"): ("bed", "process"),
    ("kernel32.dll", "CreateProcessA"): ("bed", "process"),
    ("kernel32.dll", "CreateProcessAsUserW"): ("bed", "process"),
    ("kernel32.dll", "TerminateProcess"): ("bed", "process"),
    ("kernel32.dll", "OpenProcess"): ("bed", "process"),
    ("kernel32.dll", "GetExitCodeProcess"): ("bed", "process"),
    ("shell32.dll", "ShellExecuteW"): ("bed", "process"),
    ("shell32.dll", "ShellExecuteExW"): ("bed", "process"),

    # --- Memory Bed: this is exactly the P0-spike surface (§3.2) ---
    ("kernel32.dll", "VirtualAlloc"): ("runtime", "memory"),
    ("kernel32.dll", "VirtualAllocEx"): ("runtime", "memory"),
    ("kernel32.dll", "VirtualProtect"): ("runtime", "memory"),
    ("kernel32.dll", "VirtualFree"): ("runtime", "memory"),
    ("kernel32.dll", "HeapAlloc"): ("runtime", "memory"),
    ("kernel32.dll", "HeapFree"): ("runtime", "memory"),
    ("kernel32.dll", "HeapCreate"): ("runtime", "memory"),
    ("kernel32.dll", "GetProcessHeap"): ("runtime", "memory"),

    # --- Threading Bed ---
    ("kernel32.dll", "CreateThread"): ("runtime", "threading"),
    ("kernel32.dll", "ExitThread"): ("runtime", "threading"),
    ("kernel32.dll", "WaitForSingleObject"): ("runtime", "threading"),
    ("kernel32.dll", "WaitForMultipleObjects"): ("runtime", "threading"),
    ("kernel32.dll", "CreateEventW"): ("runtime", "threading"),
    ("kernel32.dll", "SetEvent"): ("runtime", "threading"),
    ("kernel32.dll", "ResetEvent"): ("runtime", "threading"),
    ("kernel32.dll", "CreateMutexW"): ("runtime", "threading"),
    ("kernel32.dll", "ReleaseMutex"): ("runtime", "threading"),
    ("kernel32.dll", "InitializeCriticalSection"): ("runtime", "threading"),
    ("kernel32.dll", "InitializeCriticalSectionAndSpinCount"): ("runtime", "threading"),
    ("kernel32.dll", "EnterCriticalSection"): ("runtime", "threading"),
    ("kernel32.dll", "LeaveCriticalSection"): ("runtime", "threading"),
    ("kernel32.dll", "DeleteCriticalSection"): ("runtime", "threading"),
    ("kernel32.dll", "TlsAlloc"): ("runtime", "threading"),
    ("kernel32.dll", "TlsGetValue"): ("runtime", "threading"),
    ("kernel32.dll", "TlsSetValue"): ("runtime", "threading"),
    ("kernel32.dll", "InterlockedIncrement"): ("runtime", "threading"),
    ("kernel32.dll", "InterlockedDecrement"): ("runtime", "threading"),

    # --- Registry (advapi32 also carries crypto/service/token APIs) ---
    ("advapi32.dll", "RegOpenKeyExW"): ("bed", "registry"),
    ("advapi32.dll", "RegQueryValueExW"): ("bed", "registry"),
    ("advapi32.dll", "RegSetValueExW"): ("bed", "registry"),
    ("advapi32.dll", "RegCreateKeyExW"): ("bed", "registry"),
    ("advapi32.dll", "RegCloseKey"): ("bed", "registry"),
    ("advapi32.dll", "RegDeleteKeyW"): ("bed", "registry"),
    ("advapi32.dll", "RegEnumKeyExW"): ("bed", "registry"),
    ("advapi32.dll", "RegEnumValueW"): ("bed", "registry"),
    # advapi32 crypto/service-control calls are not registry -- best-effort runtime.
    ("advapi32.dll", "CryptAcquireContextW"): ("runtime", "crt"),
    ("advapi32.dll", "CryptGenRandom"): ("runtime", "crt"),

    # user32.dll carries a handful of legacy locale/string helpers for
    # compatibility reasons -- these are not windowing calls and should not
    # be lumped in with the GUI stub bucket (Appendix A).
    ("user32.dll", "CharUpperW"): ("runtime", "crt"),
    ("user32.dll", "CharUpperA"): ("runtime", "crt"),
    ("user32.dll", "CharLowerW"): ("runtime", "crt"),
    ("user32.dll", "CharLowerA"): ("runtime", "crt"),
    ("user32.dll", "CharNextW"): ("runtime", "crt"),
    ("user32.dll", "CharPrevW"): ("runtime", "crt"),
    ("user32.dll", "CharPrevExA"): ("runtime", "crt"),
    ("user32.dll", "CharNextExA"): ("runtime", "crt"),
    ("user32.dll", "IsCharAlphaW"): ("runtime", "crt"),
    ("user32.dll", "IsCharAlphaNumericW"): ("runtime", "crt"),
    ("user32.dll", "wsprintfW"): ("runtime", "crt"),
    ("user32.dll", "wsprintfA"): ("runtime", "crt"),
}

# WinRT/unsupported-feature-area stubs (§3.9) -- names that show up in some
# desktop-adjacent apps but are explicitly not-supported-on-Xbox per the
# official docs. Kept separate so the report can call out "these are known
# dead ends," not just "unclassified."
KNOWN_UNSUPPORTED_SUBSTRINGS = (
    "Bluetooth", "Nfc", "PrintDlg", "PrintDocument", "Ocr", "DragDrop",
    "SecondaryTile", "ToastNotification", "CachedFileUpdater",
)


def classify(dll: str, func: str):
    dll_l = dll.lower()
    key = (dll_l, func)
    if key in FUNCTION_OVERRIDES:
        tier, bed = FUNCTION_OVERRIDES[key]
        return tier, bed, "explicit override"
    if any(s in func for s in KNOWN_UNSUPPORTED_SUBSTRINGS):
        return "stub", "unsupported_feature_area", "matches known-unsupported name"
    if dll_l in DLL_DEFAULTS:
        tier, bed = DLL_DEFAULTS[dll_l]
        return tier, bed, "dll default"
    if re.match(r"^api-ms-win-crt-", dll_l):
        return "runtime", "crt", "ucrt forwarder dll"
    if re.match(r"^api-ms-win-", dll_l):
        return "runtime", "memory_or_threading", "unrecognized api-ms-win-* forwarder"
    return "stub", "unknown", "unrecognized DLL"


def audit_pe(path: Path):
    pe = pefile.PE(str(path), fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
    ])

    rows = []
    entries = list(getattr(pe, "DIRECTORY_ENTRY_IMPORT", []))
    entries += list(getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []))

    for entry in entries:
        dll = entry.dll.decode("ascii", "replace")
        for imp in entry.imports:
            if imp.name:
                func = imp.name.decode("ascii", "replace")
            else:
                func = f"Ordinal#{imp.ordinal}"
            tier, bed, reason = classify(dll, func)
            rows.append({
                "dll": dll, "function": func,
                "tier": tier, "bed": bed, "reason": reason,
            })

    machine = hex(pe.FILE_HEADER.Machine)
    is_64 = pe.OPTIONAL_HEADER.Magic == 0x20B
    pe.close()

    return {
        "file": str(path),
        "machine": machine,
        "arch": "x64" if is_64 else "x86",
        "import_count": len(rows),
        "imports": rows,
    }


def summarize(report):
    tier_counts = Counter(r["tier"] for r in report["imports"])
    bed_counts = Counter((r["tier"], r["bed"]) for r in report["imports"])
    dll_counts = Counter(r["dll"] for r in report["imports"])
    return tier_counts, bed_counts, dll_counts


def print_human(report):
    tier_counts, bed_counts, dll_counts = summarize(report)
    print(f"\n=== {report['file']} ({report['arch']}, {report['import_count']} imports) ===")
    print(f"  x64 native (no Bed needed): {'yes' if report['arch'] == 'x64' else 'NO -- 32-bit track, §3.7'}")
    print("\n  By tier:")
    for tier in ("bed", "runtime", "native", "stub"):
        n = tier_counts.get(tier, 0)
        if n:
            print(f"    {tier:8s} {n:4d}")
    print("\n  By Bed (tier, bed -> count):")
    for (tier, bed), n in sorted(bed_counts.items(), key=lambda kv: -kv[1]):
        print(f"    {tier:8s} {bed:24s} {n:4d}")
    print("\n  Top source DLLs:")
    for dll, n in dll_counts.most_common(8):
        print(f"    {dll:28s} {n:4d}")
    stub_rows = [r for r in report["imports"] if r["tier"] == "stub"]
    if stub_rows:
        print(f"\n  Stubbed symbols ({len(stub_rows)}) -- candidates for a future Bed if this recurs across the corpus:")
        for r in stub_rows[:20]:
            print(f"    {r['dll']:20s} {r['function']}")
        if len(stub_rows) > 20:
            print(f"    ... and {len(stub_rows) - 20} more")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("targets", nargs="+", help="PE file(s) to audit")
    ap.add_argument("--json-out", help="write combined JSON report to this path")
    args = ap.parse_args()

    reports = []
    for t in args.targets:
        p = Path(t)
        if not p.exists():
            print(f"skip (not found): {p}", file=sys.stderr)
            continue
        report = audit_pe(p)
        reports.append(report)
        print_human(report)

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(reports, indent=2))
        print(f"\nJSON report written to {args.json_out}")


if __name__ == "__main__":
    main()
