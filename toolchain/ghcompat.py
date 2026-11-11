#!/usr/bin/env python3
"""ghcompat.py -- the one record of what Greenhouse runs, checked and pushed.

  ghcompat.py check [--file corpus/compat/compat.json] [--strict]
  ghcompat.py push  --url https://... --token-file PATH [--dry-run]

`corpus/compat/compat.json` (schema greenhouse.compat/1) is the source of truth for
every public claim about what runs. `check` is what keeps it honest; `push` sends it
to the website, which holds no compatibility opinion of its own beyond the community
reports people submit there.

The console gets the same file a different way -- inside a signed `compat` package
through the ordinary repository (ghpkg.py) -- so nothing here talks to a console, and
no console talks to the website. Data flows one way, out of this file.

Exit codes: 0 ok, 1 the file is wrong (or --strict and there were warnings),
2 the push failed. Warnings alone are not a failure unless --strict.
"""
import argparse
import datetime
import json
import os
import re
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFAULT_FILE = os.path.join(ROOT, "corpus", "compat", "compat.json")
DEFAULT_CORPUS = os.path.join(ROOT, "corpus", "titles")
DEFAULT_MANIFEST = os.path.join(ROOT, "src", "host", "AppxManifest.launcher.xml")

# The website's own enums, verbatim (prisma/schema.prisma: CompatStatus, ConsoleModel).
# If these two lists ever stop matching the schema, the push will 400 and say so --
# which is the intended failure, not a silent mapping.
STATUSES = ("PERFECT", "PLAYABLE", "MENU", "BOOT", "WONT_RUN")
CONSOLES = ("ONE", "SERIES_S", "SERIES_X")

# A protection we will not defeat. A WONT_RUN without one of these is a bug we have
# not fixed yet, which is a different public claim and reads differently on the site.
WALLS = ("steamstub", "themida", "eac")

ID_RE = re.compile(r"^[a-z0-9._-]{1,64}$")
VER_RE = re.compile(r"^\d+\.\d+\.\d+\.\d+$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")

TOP_KEYS = {"schema", "serial", "generated", "titles", "//"}
TITLE_KEYS = {"appId", "id", "name", "results", "//"}
RESULT_KEYS = {"console", "status", "version", "verified", "note", "wall", "//"}

# The console's validator refuses a compat file larger than this (gh_repo.h kMaxDoc).
MAX_BYTES = 1 << 20

# How far behind the current build a result may be before `check` says so. Deliberately
# generous: the point is to catch a result that has gone stale by a release or two, not
# to nag about every build.
DEFAULT_STALE = 150


class Problems:
    """Errors fail the run; warnings are printed and counted."""

    def __init__(self):
        self.errors = []
        self.warnings = []

    def err(self, where, msg):
        self.errors.append("%s: %s" % (where, msg))

    def warn(self, where, msg):
        self.warnings.append("%s: %s" % (where, msg))

    def report(self, strict):
        for w in self.warnings:
            print("warning  %s" % w)
        for e in self.errors:
            print("ERROR    %s" % e, file=sys.stderr)
        if self.errors:
            print("\n%d error(s), %d warning(s)" % (len(self.errors), len(self.warnings)),
                  file=sys.stderr)
            return 1
        if self.warnings and strict:
            print("\n%d warning(s), and --strict" % len(self.warnings), file=sys.stderr)
            return 1
        print("\nok: %d warning(s)" % len(self.warnings))
        return 0


def vtuple(s):
    return tuple(int(x) for x in s.split("."))


def manifest_version(path):
    """The launcher's current version, or None if the manifest is not where we looked."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    m = re.search(r'Version="(\d+\.\d+\.\d+\.\d+)"', text)
    return m.group(1) if m else None


def corpus_app_ids(path):
    """{steamAppId: id} for every corpus/titles/*.ghtitle.json that names one."""
    out = {}
    if not os.path.isdir(path):
        return out
    for name in sorted(os.listdir(path)):
        if not name.endswith(".ghtitle.json"):
            continue
        try:
            with open(os.path.join(path, name), "r", encoding="utf-8") as f:
                j = json.load(f)
        except (OSError, ValueError):
            continue
        app = j.get("match", {}).get("steamAppId")
        if isinstance(app, int) and app > 0:
            out[app] = j.get("id") or name
    return out


def check_keys(d, allowed, where, p):
    for k in d:
        if k not in allowed:
            p.err(where, 'unknown key "%s"' % k)


def check_date(value, where, field, p, today):
    if not isinstance(value, str) or not DATE_RE.match(value):
        p.err(where, "%s is not YYYY-MM-DD" % field)
        return None
    try:
        d = datetime.date.fromisoformat(value)
    except ValueError:
        p.err(where, "%s is not a real date" % field)
        return None
    if d > today:
        p.err(where, "%s is in the future" % field)
    return d


def load(path, p):
    try:
        raw = open(path, "rb").read()
    except OSError as e:
        p.err(path, "cannot read (%s)" % e.strerror)
        return None
    if len(raw) > MAX_BYTES:
        p.err(path, "is %d bytes; the console refuses anything over %d" % (len(raw), MAX_BYTES))
    try:
        return json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as e:
        p.err(path, "is not valid UTF-8 JSON (%s)" % e)
        return None


def validate(doc, path, p, corpus, current, stale):
    today = datetime.date.today()

    if not isinstance(doc, dict):
        p.err(path, "top level is not an object")
        return
    check_keys(doc, TOP_KEYS, path, p)

    if doc.get("schema") != "greenhouse.compat/1":
        p.err(path, 'schema is not "greenhouse.compat/1"')
    serial = doc.get("serial")
    if not isinstance(serial, int) or isinstance(serial, bool) or serial < 1:
        p.err(path, "serial is not a positive integer")
    check_date(doc.get("generated"), path, "generated", p, today)

    titles = doc.get("titles")
    if not isinstance(titles, list) or not titles:
        p.err(path, "titles is not a non-empty array")
        return

    seen_app = {}
    seen_name = {}
    seen_id = {}
    cur = vtuple(current) if current else None

    for i, t in enumerate(titles):
        where = "%s[%d]" % (path, i)
        if not isinstance(t, dict):
            p.err(where, "is not an object")
            continue
        check_keys(t, TITLE_KEYS, where, p)

        app = t.get("appId")
        if not isinstance(app, int) or isinstance(app, bool) or app < 1 or app > 0xFFFFFFFF:
            p.err(where, "appId is not a Steam app id")
            app = None
        else:
            where = "%s (app %d)" % (path, app)
            if app in seen_app:
                p.err(where, "appId appears twice")
            seen_app[app] = t

        name = t.get("name")
        if not isinstance(name, str) or not name.strip() or len(name) > 120:
            p.err(where, "name is missing or not 1-120 characters")
        else:
            # The website's Title.name is unique, so two entries sharing a name would
            # collapse into one row there and silently lose a result.
            key = name.strip().lower()
            if key in seen_name:
                p.err(where, 'name "%s" is already used by app %s' % (name, seen_name[key]))
            seen_name[key] = app

        tid = t.get("id")
        if tid is not None:
            if not isinstance(tid, str) or not ID_RE.match(tid):
                p.err(where, "id is not [a-z0-9._-]{1,64}")
            else:
                if tid in seen_id:
                    p.err(where, 'id "%s" appears twice' % tid)
                seen_id[tid] = app
                cfg = os.path.join(DEFAULT_CORPUS, tid + ".ghtitle.json")
                if not os.path.exists(cfg):
                    p.err(where, 'id "%s" has no corpus/titles/%s.ghtitle.json' % (tid, tid))
                elif app is not None and corpus.get(app) not in (None, tid):
                    p.err(where, 'app %d is id "%s" in the corpus, not "%s"'
                          % (app, corpus[app], tid))

        results = t.get("results")
        if not isinstance(results, list) or not results:
            p.err(where, "results is not a non-empty array")
            continue

        seen_console = set()
        for r in results:
            if not isinstance(r, dict):
                p.err(where, "a result is not an object")
                continue
            check_keys(r, RESULT_KEYS, where, p)

            con = r.get("console")
            if con not in CONSOLES:
                p.err(where, "console is not one of %s" % ", ".join(CONSOLES))
            elif con in seen_console:
                p.err(where, "two results for %s" % con)
            else:
                seen_console.add(con)

            status = r.get("status")
            if status not in STATUSES:
                p.err(where, "status is not one of %s" % ", ".join(STATUSES))

            ver = r.get("version")
            if not isinstance(ver, str) or not VER_RE.match(ver):
                p.err(where, "version is not a.b.c.d")
            elif cur is not None:
                rv = vtuple(ver)
                if rv > cur:
                    p.err(where, "version %s is ahead of the build (%s)" % (ver, current))
                elif rv[:2] != cur[:2]:
                    p.warn(where, "version %s is from a different line than %s" % (ver, current))
                elif cur[2] - rv[2] > stale:
                    p.warn(where, "verified on %s, %d builds behind %s -- re-check or re-word it"
                           % (ver, cur[2] - rv[2], current))

            check_date(r.get("verified"), where, "verified", p, today)

            note = r.get("note")
            if note is not None:
                if not isinstance(note, str) or not note.strip():
                    p.err(where, "note is present but empty")
                elif len(note) > 400:
                    # The website would take 4000, but a note this long is a paragraph
                    # nobody reads, and the console shows one line of its own anyway.
                    p.err(where, "note is %d characters; keep it under 400" % len(note))

            wall = r.get("wall")
            if wall is not None:
                if wall not in WALLS:
                    p.err(where, "wall is not one of %s" % ", ".join(WALLS))
                elif status != "WONT_RUN":
                    p.err(where, "wall is set but the status is %s, not WONT_RUN" % status)

    # The check this file exists for: a title was brought up and nobody wrote it down.
    for app, tid in sorted(corpus.items()):
        if app not in seen_app:
            p.warn(path, 'corpus/titles/%s.ghtitle.json is app %d, which has no result here'
                   % (tid, app))


def do_check(args):
    p = Problems()
    doc = load(args.file, p)
    current = manifest_version(args.manifest)
    if current is None:
        p.warn(args.manifest, "no Version= found; version staleness not checked")
    corpus = corpus_app_ids(args.corpus)
    if doc is not None:
        validate(doc, args.file, p, corpus, current, args.stale)
    rc = p.report(args.strict)
    if doc is not None and not p.errors:
        n = sum(len(t.get("results", [])) for t in doc.get("titles", []))
        print("%d titles, %d results, build %s" % (len(doc["titles"]), n, current or "?"))
    return rc


def read_token(path):
    """The push token, from a file outside the tree. Never an argument, never printed."""
    with open(path, "r", encoding="utf-8") as f:
        tok = f.read().strip()
    if not tok:
        raise ValueError("the token file is empty")
    return tok


def do_push(args):
    p = Problems()
    doc = load(args.file, p)
    current = manifest_version(args.manifest)
    corpus = corpus_app_ids(args.corpus)
    if doc is not None:
        validate(doc, args.file, p, corpus, current, args.stale)
    if p.report(args.strict) != 0:
        print("not pushing a file that does not check out", file=sys.stderr)
        return 1

    body = json.dumps({"serial": doc["serial"], "generated": doc["generated"],
                       "titles": doc["titles"]}, ensure_ascii=False).encode("utf-8")
    url = args.url.rstrip("/") + "/api/compatibility/bulk"

    if args.dry_run:
        print("would POST %d bytes to %s" % (len(body), url))
        return 0

    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("User-Agent", "ghcompat/1")
    if args.token_file:
        try:
            req.add_header("Authorization", "Bearer " + read_token(args.token_file))
        except (OSError, ValueError) as e:
            print("cannot read the token file: %s" % e, file=sys.stderr)
            return 2

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            out = json.loads(resp.read().decode("utf-8", "replace"))
            print("HTTP %d  titles=%s results=%s removed=%s"
                  % (resp.status, out.get("titles"), out.get("results"), out.get("removed")))
            return 0
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:2000]
        print("HTTP %d from %s\n%s" % (e.code, url, detail), file=sys.stderr)
        return 2
    except (urllib.error.URLError, OSError, ValueError) as e:
        print("push failed: %s" % e, file=sys.stderr)
        return 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)

    def common(s):
        s.add_argument("--file", default=DEFAULT_FILE)
        s.add_argument("--corpus", default=DEFAULT_CORPUS)
        s.add_argument("--manifest", default=DEFAULT_MANIFEST)
        s.add_argument("--stale", type=int, default=DEFAULT_STALE,
                       help="builds behind the current one before a result is called stale")
        s.add_argument("--strict", action="store_true", help="treat warnings as failures")

    s = sp.add_parser("check", help="validate the dataset")
    common(s)
    s.set_defaults(fn=do_check)

    s = sp.add_parser("push", help="send the dataset to the website")
    common(s)
    s.add_argument("--url", required=True, help="the site's base URL")
    s.add_argument("--token-file", help="file holding COMPAT_PUSH_TOKEN; outside the tree")
    s.add_argument("--dry-run", action="store_true")
    s.set_defaults(fn=do_push)

    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
