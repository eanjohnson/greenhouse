#!/usr/bin/env python3
r"""
ghcmd.py -- drive the Launcher's game/package command channel (#303) over the Device Portal.

Until this existed, installing, repairing or removing a game on the console meant
driving the picker blind: type a search, screenshot it, hope the highlight is on
the title you meant, press A. That addresses SCREEN POSITION, not title identity.
On 2026-11-08 a two-letter query left the highlight on the wrong game with A one
press away from a multi-gigabyte download nobody asked for.

Every command here takes a Steam app id instead.

How it works: the Launcher polls <app>\LocalState\gh_cmd.txt while the picker is
up, renames it to *.done, runs the batch on its UI thread, and writes the result
lines to <app>\LocalState\gh_cmd_out.txt. This tool deletes the stale output
first, uploads the batch, then waits for the new output and prints it.

Commands (one per line; '#' comments and blank lines are ignored):
  list                                  app id, state, name, for every row
  install   <appid>                     download; size-matching files are kept
  resume    <appid>                     repair an incomplete install
  verify    <appid>                     hash every file and repair in place
  uninstall <appid> --confirm <appid>   delete the tree; the id must appear twice
  help                                  the command list, from the Launcher

Usage:
  ghcmd.py list
  ghcmd.py install 285900
  ghcmd.py --timeout 2400 install 3949040      # a 3.5 GB title needs the headroom
  ghcmd.py -f batch.txt                        # a file of commands
  ghcmd.py --no-wait list                      # fire and forget
  echo "verify 1966720" | ghcmd.py -f -
Env: GH_XBOX=<host[:port]>. The package is found on the device; GH_LAUNCHER_PFN overrides.
Exit codes: 0 all commands ok, 1 at least one command failed ('!' or FAILED),
            2 timed out waiting for the Launcher, 3 device unreachable, 4 bad usage.
"""
import sys, os, ssl, json, http.client, time, uuid
from urllib.parse import quote

DEFAULT_HOST = os.environ.get("GH_XBOX", "192.0.2.12")
# Matched against PackageFullName, not PackageFamilyName: this console's Device
# Portal reports the family as bare "Greenhouse.Launcher", with no publisher hash.
IDENTITY = os.environ.get("GH_LAUNCHER_IDENTITY", "Greenhouse.Launcher_")
PFN = os.environ.get("GH_LAUNCHER_PFN")     # None: ask the device, so a version bump needs no edit
LOCALSTATE = "\\LocalState"                 # the leading backslash is required
CTX = ssl._create_unverified_context()

# The verbs the Launcher knows. Checked here so a typo costs one line of output
# instead of a round trip; the Launcher validates again on its side.
VERBS = {"list": 0, "help": 0, "install": 1, "resume": 1, "verify": 1, "uninstall": 3}


class WDP:
    def __init__(self, host, timeout=120):
        self.host, self.port = (host.rsplit(":", 1)[0], int(host.rsplit(":", 1)[1])) \
            if ":" in host else (host, 11443)
        self.timeout = timeout
        self.csrf = None

    def _conn(self):
        if os.environ.get("GH_XBOX_SCHEME") == "http":      # via toolchain/wdp_relay.py
            return http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        return http.client.HTTPSConnection(self.host, self.port, context=CTX, timeout=self.timeout)

    def _get_csrf(self):
        c = self._conn(); c.request("GET", "/api/os/machinename"); r = c.getresponse(); r.read()
        tok = None
        for k, v in r.getheaders():
            if k.lower() == "set-cookie" and "CSRF-Token=" in v:
                tok = v.split("CSRF-Token=")[1].split(";")[0]
        c.close(); return tok

    def req(self, method, path, body=None, headers=None):
        if self.csrf is None:
            self.csrf = self._get_csrf()
        h = {"X-CSRF-Token": self.csrf or "", "Cookie": f"CSRF-Token={self.csrf or ''}"}
        if headers:
            h.update(headers)
        c = self._conn(); c.request(method, path, body=body, headers=h); r = c.getresponse()
        data = r.read(); c.close()
        return r.status, data

    def pfn(self):
        """The installed Launcher's package full name. Cached for the run."""
        global PFN
        if PFN:
            return PFN
        st, data = self.req("GET", "/api/app/packagemanager/packages")
        pkgs = (json.loads(data) if st == 200 and data else {}).get("InstalledPackages", [])
        hits = sorted(p["PackageFullName"] for p in pkgs
                      if str(p.get("PackageFullName", "")).startswith(IDENTITY))
        if len(hits) == 1:
            PFN = hits[0]
            return PFN
        if not hits:
            raise LookupError(f"no package named {IDENTITY}* is installed "
                              f"({len(pkgs)} package(s) listed)")
        raise LookupError(f"{len(hits)} packages match {IDENTITY}*: {hits} "
                          f"-- set GH_LAUNCHER_PFN to pick one")

    def _file(self, name=None):
        p = (f"/api/filesystem/apps/file?knownfolderid=LocalAppData"
             f"&packagefullname={self.pfn()}&path={quote(LOCALSTATE)}")
        return p + (f"&filename={quote(name)}" if name else "")

    def get(self, name):
        st, data = self.req("GET", self._file(name))
        return st, data

    def delete(self, name):
        st, _ = self.req("DELETE", self._file(name))
        return st

    def put(self, name, content):
        b = "----ghcmd" + uuid.uuid4().hex
        body = ((f'--{b}\r\nContent-Disposition: form-data; name="{name}"; filename="{name}"\r\n'
                 f"Content-Type: application/octet-stream\r\n\r\n").encode()
                + content + f"\r\n--{b}--\r\n".encode())
        st, data = self.req("POST", self._file(),
                            body=body,
                            headers={"Content-Type": f"multipart/form-data; boundary={b}",
                                     "Content-Length": str(len(body))})
        return st, data


def check(lines):
    """Reject what the Launcher would reject anyway, before uploading."""
    bad = []
    for ln in lines:
        a = ln.split()
        if a[0] not in VERBS:
            bad.append(f"{ln!r}: unknown command {a[0]!r} (try: {', '.join(sorted(VERBS))})")
            continue
        if len(a) - 1 < VERBS[a[0]]:
            bad.append(f"{ln!r}: {a[0]} needs {VERBS[a[0]]} argument(s)")
            continue
        if a[0] in ("install", "resume", "verify", "uninstall") and not a[1].isdigit():
            bad.append(f"{ln!r}: {a[1]!r} is not an app id")
            continue
        # An uninstall that is not spelled out in full deletes nothing on the
        # device; say so here rather than after a round trip.
        if a[0] == "uninstall" and (a[2] != "--confirm" or a[3] != a[1]):
            bad.append(f"{ln!r}: uninstall must read 'uninstall {a[1]} --confirm {a[1]}'")
    return bad


class Opts:
    file = None
    host = DEFAULT_HOST
    timeout = 1800
    no_wait = False


def parse_argv(argv):
    """Our own scan, not argparse: the command text contains `--confirm`, and
    argparse would claim it as an unknown option of ours and exit."""
    o, rest, i = Opts(), [], 0
    while i < len(argv):
        a = argv[i]
        if a in ("-h", "--help"):
            return None, None
        elif a in ("-f", "--file"):
            i += 1; o.file = argv[i]
        elif a == "--host":
            i += 1; o.host = argv[i]
        elif a == "--timeout":
            i += 1; o.timeout = int(argv[i])
        elif a == "--no-wait":
            o.no_wait = True
        else:
            rest = argv[i:]             # from here on it is all command text
            break
        i += 1
    return o, rest


def main():
    try:
        o, rest = parse_argv(sys.argv[1:])
    except (IndexError, ValueError):
        print("ghcmd: bad options", file=sys.stderr); return 4
    if o is None:
        print(__doc__); return 0

    if o.file:
        text = sys.stdin.read() if o.file == "-" else open(o.file, encoding="utf-8").read()
    elif rest:
        # One command per argument group: "install 285900" or "install 285900 ; list".
        text = " ".join(rest).replace(";", "\n")
    else:
        print(__doc__); return 4

    lines = [l.strip() for l in text.splitlines()]
    lines = [l for l in lines if l and not l.startswith("#")]
    if not lines:
        print("ghcmd: nothing to send", file=sys.stderr); return 4
    bad = check(lines)
    if bad:
        for b in bad:
            print("ghcmd: " + b, file=sys.stderr)
        return 4

    w = WDP(o.host)
    try:
        # A stale gh_cmd_out.txt is indistinguishable from this batch's output,
        # so clear it first; 404 (never written) is the normal case.
        w.delete("gh_cmd_out.txt")
        st, data = w.put("gh_cmd.txt", ("\n".join(lines) + "\n").encode())
    except OSError as e:
        print(f"ghcmd: {o.host} unreachable: {e}", file=sys.stderr); return 3
    except LookupError as e:
        print(f"ghcmd: {e}", file=sys.stderr); return 3
    if st != 200:
        print(f"ghcmd: upload failed {st} {data[:200]!r}", file=sys.stderr); return 3
    print(f"ghcmd: sent {len(lines)} command(s) to {o.host}")
    for l in lines:
        print("  " + l)
    if o.no_wait:
        return 0

    # The Launcher polls every 2 s, so the file disappears quickly once the
    # picker has it. If it never does, the picker is not up (a game is running,
    # or the Launcher is on another screen) -- say that instead of "timed out".
    t0 = time.time()
    warned = t0
    taken = False
    while time.time() - t0 < o.timeout:
        time.sleep(4)
        try:
            st, data = w.get("gh_cmd_out.txt")
        except OSError as e:
            print(f"ghcmd: poll error {e}", file=sys.stderr); continue
        if st == 200 and data:
            out = data.decode("utf-8", "replace")
            print(f"--- gh_cmd_out.txt  ({time.time() - t0:.0f}s)")
            print(out.rstrip())
            fail = [l for l in out.splitlines() if l.startswith("!") or l.startswith("FAILED")]
            return 1 if fail else 0
        if not taken:
            sc, _ = w.get("gh_cmd.txt")
            if sc == 404:
                taken = True
                print("ghcmd: picker took the batch; waiting for it to finish")
            elif time.time() - warned > 30:
                print("ghcmd: gh_cmd.txt is still sitting there -- is the picker up "
                      "(no game running, Launcher on the game list)?", file=sys.stderr)
                warned = time.time()    # warn at most every 30 s; the timeout keeps running
    print(f"ghcmd: no output after {o.timeout}s", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
