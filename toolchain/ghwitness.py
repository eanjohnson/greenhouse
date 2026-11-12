#!/usr/bin/env python3
"""Out-of-process witness: watch the console from the HOST while a guest runs.

Why this exists. Dead as Disco (app 3763830) stops dead: in s998 not one thread
in the process ran for 86.2 s -- no FRAME, no MEMORY sample, no PADPOLL, no diag
line of any kind -- with 32,736 faults and 2.18 GB physical free, and `PLM
Suspending` arrived AFTER the freeze rather than causing it. Every instrument we
have is INSIDE that process, so when it freezes they all freeze with it and the
log says nothing. Ten more launches cannot answer this; a witness outside it can.

The four questions it answers, none of which the app's own diag can:

  1. Does the process still exist, and is it still burning CPU?
     app CPU > 0 through the gap  => threads ARE running, they just stopped
     logging (so the stall is in our diag path, or in a lock around it).
     app CPU == 0                 => genuinely not executing.
  2. Is anything else eating the box?  CpuLoad high with our app at 0 means we
     were descheduled, not blocked.
  3. Is it storage?  IOReadSpeed/IOWriteSpeed are system-wide. 32k faults with
     memory free smells like paging -- our own spill pager (#233) writes a
     user-mode page file. A read-speed spike across the gap would say so.
  4. Is it the whole console?  Every sample records the round-trip latency of
     its own request. If WDP (which runs in SystemOS, not our container) also
     stalls, the freeze is system-wide and not ours at all.

Honesty rules this thing is built around, learned the hard way (§24):
  * It records its own blind spots. A tick that fails, or arrives late, is
    written to the log as a row with `err`/`late_ms` -- never dropped. A gap in
    the data is itself a measurement, and the report prints it as one.
  * Laptop-side stalls are distinguished from console-side ones: each row keeps
    the monotonic time BEFORE the request and the request's own duration, so
    "python didn't get scheduled" cannot be read as "the console stalled".
  * It never says "clean" for an absence. The report prints coverage first --
    how many samples landed out of how many were due -- and every verdict is
    qualified by it.

Usage:
  ghwitness.py watch --out w.jsonl --secs 480 [--match Pagoda] [--interval 0.5]
                     [--proc-every 8]
  ghwitness.py report w.jsonl [--gap-ms 3000] [--app-epoch <unix float>]

`watch` runs until --secs elapse, then exits 0. Run it BEHIND the launch harness
(both in the background, witness first) and keep the clocks: the report takes
--app-epoch to put console-relative diag milliseconds on the same axis.
"""
import argparse
import http.client
import json
import os
import ssl
import sys
import time

HOST = os.environ.get("GH_XBOX", "")   # console IP; required for watch
PORT = int(os.environ.get("GH_XBOX_PORT", "11443"))
CTX = ssl._create_unverified_context()

# A witness must fail fast. A long socket timeout here would turn "the console
# stalled" into "the witness stopped sampling", which is the one failure mode
# that would make the whole instrument lie (run 84: one poll blocked 11 minutes
# with no output and no exception).
REQ_TIMEOUT = float(os.environ.get("GH_WITNESS_TIMEOUT", "8"))


class Wdp:
    """One persistent connection, re-made on any error, with a CSRF token."""

    def __init__(self):
        self.c = None
        self.csrf = None

    def _connect(self):
        self.c = http.client.HTTPSConnection(HOST, PORT, context=CTX,
                                             timeout=REQ_TIMEOUT)
        self.c.request("GET", "/api/os/machinename")
        r = self.c.getresponse()
        r.read()
        for k, v in r.getheaders():
            if k.lower() == "set-cookie" and "CSRF-Token=" in v:
                self.csrf = v.split("CSRF-Token=")[1].split(";")[0]

    def get(self, path):
        """-> (json_or_None, latency_ms, err_or_None). Never raises."""
        for attempt in (0, 1):
            t0 = time.monotonic()
            try:
                if self.c is None:
                    self._connect()
                h = {"X-CSRF-Token": self.csrf or "",
                     "Cookie": f"CSRF-Token={self.csrf or ''}"}
                self.c.request("GET", path, headers=h)
                r = self.c.getresponse()
                body = r.read()
                lat = (time.monotonic() - t0) * 1000.0
                if r.status != 200:
                    return None, lat, f"HTTP {r.status}"
                return json.loads(body), lat, None
            except Exception as e:                      # noqa: BLE001
                lat = (time.monotonic() - t0) * 1000.0
                try:
                    if self.c:
                        self.c.close()
                except Exception:                        # noqa: BLE001
                    pass
                self.c = None
                if attempt:          # the retry failed too: report it
                    return None, lat, f"{type(e).__name__}: {e}"
        return None, 0.0, "unreachable"


def mb(pages, page_size=4096):
    return round(pages * page_size / (1024 * 1024), 1) if pages is not None else None


def watch(args):
    if not HOST:
        print("witness: set GH_XBOX to the console IP", file=sys.stderr)
        return 4
    w = Wdp()
    t_start = time.monotonic()
    tick = 0
    seen_app = False
    out = open(args.out, "a", buffering=1)
    print(f"witness: {HOST}:{PORT} every {args.interval}s, processes every "
          f"{args.proc_every} ticks, {args.secs}s, match={args.match!r}")

    due = t_start
    while True:
        now = time.monotonic()
        if now - t_start >= args.secs:
            break
        # How late is this tick? A late tick is OUR fault (laptop scheduling or
        # a slow previous request), not the console's, and the row says which.
        late_ms = max(0.0, (now - due) * 1000.0)
        row = {"t": round(time.time(), 3),
               "mono": round(now - t_start, 3),
               "tick": tick,
               "late_ms": round(late_ms, 1)}

        sp, lat, err = w.get("/api/resourcemanager/systemperf")
        row["sp_ms"] = round(lat, 1)
        if err:
            row["err"] = err
        else:
            ps = sp.get("PageSize", 4096)
            row["cpu"] = sp.get("CpuLoad")
            row["avail_mb"] = mb(sp.get("AvailablePages"), ps)
            row["commit_mb"] = mb(sp.get("CommittedPages"), ps)
            row["ior"] = sp.get("IOReadSpeed")
            row["iow"] = sp.get("IOWriteSpeed")
            row["ioo"] = sp.get("IOOtherSpeed")
            g = (sp.get("GPUData") or {}).get("AvailableAdapters") or []
            if g:
                a = g[0]
                row["gpu_ded_mb"] = round(
                    (a.get("DedicatedMemoryUsed") or 0) / (1024 * 1024), 1)
                row["gpu_sys_mb"] = round(
                    (a.get("SystemMemoryUsed") or 0) / (1024 * 1024), 1)
                row["gpu_eng"] = a.get("EnginesUtilization")

        if args.proc_every and tick % args.proc_every == 0:
            pr, lat2, err2 = w.get("/api/resourcemanager/processes")
            row["pr_ms"] = round(lat2, 1)
            if err2:
                row["pr_err"] = err2
            else:
                procs = pr.get("Processes") or []
                hit = None
                top = []
                for p in procs:
                    nm = p.get("ImageName") or ""
                    if args.match and args.match.lower() in nm.lower():
                        hit = p
                    cu = p.get("CPUUsage") or 0
                    if cu > 0:
                        top.append((cu, nm))
                row["nproc"] = len(procs)
                top.sort(reverse=True)
                row["top"] = [[round(c, 1), n] for c, n in top[:4]]
                if hit:
                    seen_app = True
                    row["app"] = {
                        "pid": hit.get("ProcessId"),
                        "cpu": hit.get("CPUUsage"),
                        "ws_mb": round((hit.get("WorkingSetSize") or 0) / 2**20, 1),
                        "pws_mb": round((hit.get("PrivateWorkingSet") or 0) / 2**20, 1),
                        "pf_mb": round((hit.get("PageFileUsage") or 0) / 2**20, 1),
                        "va_mb": round((hit.get("VirtualSize") or 0) / 2**20, 1),
                    }
                else:
                    # Absence is data: say whether it was ever there.
                    row["app"] = None
                    row["app_gone"] = bool(seen_app)

        out.write(json.dumps(row) + "\n")
        tick += 1
        due += args.interval
        slack = due - time.monotonic()
        if slack > 0:
            time.sleep(slack)
        else:
            due = time.monotonic()      # fell behind: resync, do not burst
    out.close()
    print(f"witness: {tick} samples -> {args.out}")
    return 0


def report(args):
    rows = []
    with open(args.path) as fh:
        for line in fh:
            line = line.strip()
            if line:
                try:
                    rows.append(json.loads(line))
                except Exception:        # noqa: BLE001
                    pass
    if not rows:
        print("no samples")
        return 1

    span = rows[-1]["mono"] - rows[0]["mono"]
    errs = [r for r in rows if r.get("err")]
    print(f"=== coverage (read this before any verdict below)")
    print(f"samples {len(rows)} over {span:.1f}s"
          f"   failed {len(errs)}   max req {max(r.get('sp_ms', 0) for r in rows):.0f}ms"
          f"   max late {max(r.get('late_ms', 0) for r in rows):.0f}ms")

    # Sampling holes, split by cause. A hole we caused is not evidence about
    # the console; a hole the console caused IS the headline.
    holes = []
    for a, b in zip(rows, rows[1:]):
        d = (b["mono"] - a["mono"]) * 1000.0
        if d >= args.gap_ms:
            holes.append((a["mono"], d, a.get("sp_ms", 0), b.get("late_ms", 0),
                          a.get("err") or b.get("err")))
    if holes:
        print(f"--- {len(holes)} sampling hole(s) >= {args.gap_ms}ms")
        for t, d, req_ms, late, err in holes[:12]:
            who = "CONSOLE (request itself blocked)" if req_ms >= d * 0.5 \
                else "host/witness (tick arrived late)"
            print(f"  t={t:7.1f}s  hole {d:7.0f}ms  req {req_ms:6.0f}ms  "
                  f"late {late:6.0f}ms  {who}"
                  + (f"  err={err}" if err else ""))
    else:
        print(f"--- no sampling hole >= {args.gap_ms}ms: WDP answered "
              f"throughout, so the console as a whole never stalled")

    # The app's own track.
    app = [r for r in rows if "app" in r]
    live = [r for r in app if r.get("app")]
    if not app:
        print("--- no process samples (--proc-every 0?)")
    elif not live:
        print("--- the matched process was NEVER seen: check --match")
    else:
        gone = [r for r in app if r.get("app_gone")]
        print(f"--- app: {len(live)}/{len(app)} process samples saw it"
              + (f", GONE from t={gone[0]['mono']:.1f}s" if gone else ""))
        print("  t(s)   cpu%   ws_MB  pws_MB   pf_MB  |  sysCPU  availMB "
              "ioRead/s  ioWrite/s")
        step = max(1, len(live) // 30)
        for r in live[::step]:
            a = r["app"]
            print(f"  {r['mono']:6.1f} {a['cpu'] or 0:6.1f} {a['ws_mb']:7.0f} "
                  f"{a['pws_mb']:7.0f} {a['pf_mb']:7.0f}  |  "
                  f"{r.get('cpu', -1):6} {r.get('avail_mb', -1):8} "
                  f"{r.get('ior') or 0:9} {r.get('iow') or 0:10}")

    # The freeze window, if the caller can place it.
    if args.app_epoch and args.freeze_ms:
        f0, f1 = args.freeze_ms
        w0 = args.app_epoch + f0 / 1000.0
        w1 = args.app_epoch + f1 / 1000.0
        inw = [r for r in rows if w0 <= r["t"] <= w1]
        print(f"\n=== THE WINDOW: diag {f0}-{f1}ms  ({len(inw)} samples)")
        if not inw:
            print("  no samples in the window -- the witness could not see it")
        else:
            acpu = [r["app"]["cpu"] for r in inw if r.get("app")]
            print(f"  app CPU%:    {acpu if acpu else 'no process sample in window'}")
            print(f"  system CPU%: {[r.get('cpu') for r in inw]}")
            print(f"  ioRead/s:    {[r.get('ior') for r in inw]}")
            print(f"  ioWrite/s:   {[r.get('iow') for r in inw]}")
            print(f"  availMB:     {[r.get('avail_mb') for r in inw]}")
            print(f"  req latency: {[r.get('sp_ms') for r in inw]}")
            if acpu and max(acpu) > 1.0:
                print("  => the process WAS burning CPU: threads ran and stopped "
                      "LOGGING. Look at the diag path, not the scheduler.")
            elif acpu:
                print("  => the process was NOT executing. If system CPU is also "
                      "low and WDP answered fast, nothing on the box was busy.")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    w = sub.add_parser("watch")
    w.add_argument("--out", required=True)
    w.add_argument("--secs", type=float, default=480)
    w.add_argument("--interval", type=float, default=0.5)
    w.add_argument("--proc-every", type=int, default=8,
                   help="take the 580ms process list every Nth tick (0 = never)")
    w.add_argument("--match", default="",
                   help="substring of the guest ImageName to track")
    w.set_defaults(fn=watch)

    r = sub.add_parser("report")
    r.add_argument("path")
    r.add_argument("--gap-ms", type=float, default=3000)
    r.add_argument("--app-epoch", type=float, default=None,
                   help="unix time of loader start, to place diag ms on this axis")
    r.add_argument("--freeze-ms", type=float, nargs=2, default=None,
                   metavar=("FROM", "TO"), help="the diag-relative window to dissect")
    r.set_defaults(fn=report)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
