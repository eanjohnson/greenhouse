#!/usr/bin/env python3
r"""
qrtool_lethalco.py — grab the freshest Steam login QR from the LethalCo app on the Xbox and render it.

Sibling of qrtool.py (which targets CheeseMP). Same protocol, same shim contract:
the Greenhouse loader writes the current Steam QR-login challenge URL to
  <app>\LocalState\gh_qr_url.txt
and rotates it every ~12-18s. This tool pulls that URL over the Windows Device Portal (WDP),
auto-detecting whichever LethalCo package version is installed, and renders a scannable QR — as a
PNG and/or directly in the terminal. Because the challenge rotates, the default mode WAITS for the
next rotation so you scan a QR at the very start of its window (maximum time to scan).

Requirements: `qrencode` on PATH. No other deps (pure stdlib + WDP).

Usage:
  qrtool_lethalco.py                 # wait for a fresh rotation, render once (terminal + PNG), print URL
  qrtool_lethalco.py --now           # don't wait; render whatever URL is current right now
  qrtool_lethalco.py --watch         # live mode: keep re-rendering each time it rotates (Ctrl-C to stop)
  qrtool_lethalco.py --png out.png    # also write the PNG here (default: ./steam_qr.png)
  qrtool_lethalco.py --no-term        # skip the terminal render (PNG only)
  qrtool_lethalco.py --host <console-ip>:11443
Env overrides: GH_XBOX=<host[:port]>, GH_LETHALCO_FAMILY=<package family name>.
Exit codes: 0 ok, 2 app not in QR stage / no URL yet, 3 device unreachable, 4 qrencode missing.
"""
import sys, os, ssl, json, http.client, time, argparse, subprocess, shutil
from urllib.parse import quote

DEFAULT_HOST = os.environ.get("GH_XBOX", "")   # host:port; required (env or --host)
FAMILY = os.environ.get("GH_LETHALCO_FAMILY", "Greenhouse.LethalCo_mgw46vk3ppz9w")
PKG_SUBSTR = "Greenhouse.LethalCo"
CTX = ssl._create_unverified_context()


def _split_host(h):
    if ":" in h:
        host, port = h.rsplit(":", 1); return host, int(port)
    return h, 11443


class WDP:
    def __init__(self, host):
        self.host, self.port = _split_host(host)
        self.csrf = None

    def _conn(self):
        return http.client.HTTPSConnection(self.host, self.port, context=CTX, timeout=30)

    def _get_csrf(self):
        c = self._conn(); c.request("GET", "/api/os/machinename"); r = c.getresponse(); r.read()
        tok = None
        for k, v in r.getheaders():
            if k.lower() == "set-cookie" and "CSRF-Token=" in v:
                tok = v.split("CSRF-Token=")[1].split(";")[0]
        c.close(); return tok

    def req(self, method, path, expect_json=True):
        if self.csrf is None:
            self.csrf = self._get_csrf()
        hdr = {"X-CSRF-Token": self.csrf or "", "Cookie": f"CSRF-Token={self.csrf or ''}"}
        c = self._conn(); c.request(method, path, headers=hdr); r = c.getresponse()
        data = r.read(); c.close()
        if expect_json:
            try: return r.status, (json.loads(data) if data else None)
            except Exception: return r.status, data.decode("utf-8", "replace")
        return r.status, data

    def find_pfn(self):
        """Return the installed LethalCo package full name, or None."""
        st, d = self.req("GET", "/api/app/packagemanager/packages")
        if st != 200 or not isinstance(d, dict):
            return None
        pkgs = d.get("InstalledPackages", [])
        # Prefer exact family match; fall back to name substring.
        for p in pkgs:
            if p.get("PackageFamilyName") == FAMILY:
                return p.get("PackageFullName")
        for p in pkgs:
            if PKG_SUBSTR in (p.get("PackageFullName") or ""):
                return p.get("PackageFullName")
        return None

    def read_qr_url(self, pfn):
        """Return the current challenge URL string, or '' if not present yet.

        The LethalCo shim was built with GH_PKG_FAMILY hardcoded to the CheeseMP
        package family, so it writes gh_qr_url.txt to that family's LocalState
        path. The loader's VFS redirects such writes into
        `<LethalCo LocalState>\\vfs\\U\\Users\\UserMgr0\\AppData\\Local\\Packages\\Greenhouse.CheeseMP_mgw46vk3ppz9w\\LocalState\\`.
        Try both the direct LocalState path (in case a future shim rebuild fixes
        the hardcode) and the VFS-mirror fallback.
        """
        VFS_MIRROR = ("\\LocalState\\vfs\\U\\Users\\UserMgr0\\AppData\\Local"
                      "\\Packages\\Greenhouse.CheeseMP_mgw46vk3ppz9w\\LocalState")
        for folder in ("\\LocalState", VFS_MIRROR):
            path = ("/api/filesystem/apps/file?knownfolderid=LocalAppData"
                    f"&packagefullname={pfn}&path={quote(folder)}"
                    f"&filename={quote('gh_qr_url.txt')}")
            st, data = self.req("GET", path, expect_json=False)
            if st == 200 and data:
                s = data.decode("utf-8", "replace").replace("\x00", "").strip()
                if s.startswith("http"):
                    return s
        return ""


def render(url, png_path, do_term):
    sys.stdout.flush()  # keep our prints ordered relative to qrencode's direct-to-tty output
    if png_path:
        subprocess.run(["qrencode", "-o", png_path, "-s", "10", "-m", "3", url], check=True)
    if do_term:
        subprocess.run(["qrencode", "-t", "ANSIUTF8", "-m", "2", url], check=True)


def poll_until(fn, pred, timeout, interval=2.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
            if pred(v):
                return v
        except (ConnectionError, OSError, ssl.SSLError):
            pass
        time.sleep(interval)
    return None


def main():
    ap = argparse.ArgumentParser(description="Grab + render the freshest Steam login QR from the Xbox (LethalCo).")
    ap.add_argument("--now", action="store_true", help="use the current URL without waiting for a rotation")
    ap.add_argument("--watch", action="store_true", help="live mode: re-render on every rotation until Ctrl-C")
    ap.add_argument("--png", default="steam_qr.png", help="PNG output path (default: ./steam_qr.png)")
    ap.add_argument("--no-term", action="store_true", help="do not render the QR in the terminal")
    ap.add_argument("--host", default=DEFAULT_HOST, help="Xbox WDP host[:port] (default: $GH_XBOX)")
    args = ap.parse_args()
    if not args.host:
        print("ERROR: set GH_XBOX or pass --host <console-ip>:11443", file=sys.stderr)
        return 3

    if not shutil.which("qrencode"):
        print("ERROR: qrencode not found on PATH (install it: sudo pacman -S qrencode).", file=sys.stderr)
        return 4

    wdp = WDP(args.host)
    try:
        pfn = wdp.find_pfn()
    except (ConnectionError, OSError, ssl.SSLError) as e:
        print(f"ERROR: cannot reach Xbox at {args.host}: {e}", file=sys.stderr)
        return 3
    if not pfn:
        print(f"ERROR: cannot reach Xbox at {args.host} or no LethalCo package installed.", file=sys.stderr)
        return 3
    print(f"[qrtool_lethalco] package: {pfn}")

    # Wait for the app to reach the QR stage (gh_qr_url.txt present & non-empty).
    url = wdp.read_qr_url(pfn)
    if not url:
        print("[qrtool_lethalco] waiting for the app to reach the QR-login stage (boot + anon login ~40s)...")
        url = poll_until(lambda: wdp.read_qr_url(pfn), bool, timeout=120)
    if not url:
        print("ERROR: no QR URL yet. Is the app booted and at the login screen? (gh_qr_url.txt empty)",
              file=sys.stderr)
        return 2

    def grab_fresh(prev):
        """Wait for the URL to rotate away from `prev`. Returns (url, rotated?)."""
        nu = poll_until(lambda: wdp.read_qr_url(pfn), lambda v: v and v != prev, timeout=40)
        return (nu, True) if nu else (prev, False)

    if args.watch:
        print("[qrtool_lethalco] live mode — a fresh QR renders on every rotation. Ctrl-C to stop.\n")
        cur = None
        try:
            while True:
                nxt = wdp.read_qr_url(pfn)
                if nxt and nxt != cur:
                    cur = nxt
                    os.system("clear")
                    print(f"[qrtool_lethalco] {pfn}\n[qrtool_lethalco] scan now — rotates in ~12-18s\n")
                    render(cur, args.png, not args.no_term)
                    print(f"\nURL: {cur}\nPNG: {os.path.abspath(args.png)}")
                time.sleep(2)
        except KeyboardInterrupt:
            print("\n[qrtool_lethalco] stopped.")
            return 0

    if not args.now:
        print("[qrtool_lethalco] waiting for the next rotation so you get a full ~15s window...")
        url, rotated = grab_fresh(url)
        if not rotated:
            print("[qrtool_lethalco] WARNING: URL did not rotate in 40s — it may be STALE (already logged in, "
                  "or the app isn't at the login screen). Rendering it anyway; use --now to skip this wait.")

    render(url, args.png, not args.no_term)
    print(f"\nURL: {url}")
    if args.png:
        print(f"PNG: {os.path.abspath(args.png)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
