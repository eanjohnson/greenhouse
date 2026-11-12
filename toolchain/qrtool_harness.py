#!/usr/bin/env python3
r"""
qrtool_harness.py — grab the freshest Steam login QR from the standalone VM harness and render it.

Sibling of qrtool_lethalco.py / qrtool.py, but the source is the control VM, not the Xbox.
The games-played harness (src/spikes/gh_gp_harness.cpp) writes its current Steam QR-login
challenge URL to
  C:\gh\gh_qr_url.txt   (on the win11 control VM)
and follows Steam's challenge rotation (~every 30s). This tool reads that file over SSH to
the VM host + the QEMU guest agent (the same path gexec.py uses)
and renders a scannable QR — as a PNG and/or directly in the terminal. Because the challenge
rotates, the default mode WAITS for the next rotation so you scan a QR at the very start of its
window (maximum time to scan).

Requirements: `qrencode` on PATH here; SSH access to the VM host with gexec.py in its home directory
and the win11 VM running. No other deps (pure stdlib).

Usage:
  qrtool_harness.py                 # wait for a fresh rotation, render once (terminal + PNG), print URL
  qrtool_harness.py --now           # don't wait; render whatever URL is current right now
  qrtool_harness.py --watch         # live mode: keep re-rendering each time it rotates (Ctrl-C to stop)
  qrtool_harness.py --png out.png   # also write the PNG here (default: ./steam_qr.png)
  qrtool_harness.py --no-term       # skip the terminal render (PNG only)
  qrtool_harness.py --ssh <host>    # SSH host that can reach the VM guest agent
Env: GH_VM_HOST=<ssh host> (required unless --ssh is given).
Exit codes: 0 ok, 2 harness not at QR stage / no URL yet, 3 VM/SSH unreachable, 4 qrencode missing.
"""
import sys, os, time, argparse, subprocess, shutil

SSH_HOST = os.environ.get("GH_VM_HOST", "")
QR_GUEST_PATH = r"C:\gh\gh_qr_url.txt"

# Runs ON the VM host (fed to `python3 -` over SSH stdin, so no shell-quoting hell). Reads the
# guest file via the same gexec helper used everywhere else and prints the URL (or nothing).
_REMOTE = r'''
import os, sys
sys.path.insert(0, os.path.expanduser("~"))
try:
    import gexec
    data = gexec.get(r"%s")
    s = data.decode("utf-8", "replace").replace("\x00", "").strip()
    if s.startswith("http"):
        print(s)
except Exception as e:
    sys.stderr.write("REMOTE_ERR: %%s\n" %% e)
''' % QR_GUEST_PATH


class VMUnreachable(Exception):
    pass


def read_qr_url(ssh_host):
    """Return the current challenge URL string, or '' if not present yet.

    Raises VMUnreachable if SSH itself fails or the guest agent is down (vs. the file
    simply not existing yet, which returns '').
    """
    try:
        p = subprocess.run(
            ["ssh", "-o", "ConnectTimeout=8", "-o", "BatchMode=yes", ssh_host, "python3", "-"],
            input=_REMOTE, capture_output=True, text=True, timeout=40,
        )
    except (subprocess.TimeoutExpired, OSError) as e:
        raise VMUnreachable(str(e))
    err = (p.stderr or "").strip()
    if p.returncode != 0 and "REMOTE_ERR" not in err:
        # ssh transport failure (host down, auth, etc.) — not just a missing file.
        raise VMUnreachable(err or f"ssh exit {p.returncode}")
    if "domain is not running" in err or "guest agent" in err:
        raise VMUnreachable(err)
    out = (p.stdout or "").strip()
    return out if out.startswith("http") else ""


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
        except VMUnreachable:
            pass
        time.sleep(interval)
    return None


def main():
    ap = argparse.ArgumentParser(description="Grab + render the freshest Steam login QR from the VM harness.")
    ap.add_argument("--now", action="store_true", help="use the current URL without waiting for a rotation")
    ap.add_argument("--watch", action="store_true", help="live mode: re-render on every rotation until Ctrl-C")
    ap.add_argument("--png", default="steam_qr.png", help="PNG output path (default: ./steam_qr.png)")
    ap.add_argument("--no-term", action="store_true", help="do not render the QR in the terminal")
    ap.add_argument("--ssh", default=SSH_HOST, help="SSH host that reaches the VM guest agent (default: $GH_VM_HOST)")
    args = ap.parse_args()
    if not args.ssh:
        print("ERROR: set GH_VM_HOST or pass --ssh <host> (the machine running the VM).", file=sys.stderr)
        return 3

    if not shutil.which("qrencode"):
        print("ERROR: qrencode not found on PATH (install it: sudo pacman -S qrencode).", file=sys.stderr)
        return 4

    try:
        url = read_qr_url(args.ssh)
    except VMUnreachable as e:
        print(f"ERROR: cannot reach the VM harness via {args.ssh}: {e}", file=sys.stderr)
        return 3

    if not url:
        print("[qrtool_harness] waiting for the harness to reach the QR-login stage (bring-up + anon login ~20s)...")
        url = poll_until(lambda: read_qr_url(args.ssh), bool, timeout=120)
    if not url:
        print("ERROR: no QR URL yet. Is the harness running and past anon login? (C:\\gh\\gh_qr_url.txt empty)",
              file=sys.stderr)
        return 2

    if args.watch:
        print("[qrtool_harness] live mode — a fresh QR renders on every rotation. Ctrl-C to stop.\n")
        cur = None
        try:
            while True:
                try:
                    nxt = read_qr_url(args.ssh)
                except VMUnreachable:
                    nxt = None
                if nxt and nxt != cur:
                    cur = nxt
                    os.system("clear")
                    print("[qrtool_harness] VM harness\n[qrtool_harness] scan now — rotates in ~30s\n")
                    render(cur, args.png, not args.no_term)
                    print(f"\nURL: {cur}\nPNG: {os.path.abspath(args.png)}")
                time.sleep(2)
        except KeyboardInterrupt:
            print("\n[qrtool_harness] stopped.")
            return 0

    if not args.now:
        print("[qrtool_harness] waiting for the next rotation so you get a full ~30s window...")
        nu = poll_until(lambda: read_qr_url(args.ssh), lambda v: v and v != url, timeout=45)
        if nu:
            url = nu
        else:
            print("[qrtool_harness] WARNING: URL did not rotate in 45s — it may be STALE (already logged in, "
                  "or the harness isn't at the login screen). Rendering it anyway; use --now to skip this wait.")

    render(url, args.png, not args.no_term)
    print(f"\nURL: {url}")
    if args.png:
        print(f"PNG: {os.path.abspath(args.png)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
