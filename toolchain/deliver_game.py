#!/usr/bin/env python3
# Greenhouse on-device game delivery: push a staged guest game tree into a slot's
# LocalState via WDP per-file upload, so the game lives on the console and only the
# ~600KB loader is repackaged per iteration. Proven viable by GhProbeLocalStateLoad
# (container maps unsigned DLLs from LocalState). The loader resolves the guest from
# <LocalState>\game\<name> when present (see guest_loader.cpp ResolveGuestGamePath).
#
# WDP notes: fsupload does NOT create parent dirs (500 "cannot find the path"), so we
# mkdir each dir first via /api/filesystem/apps/folder?...&newfoldername=<name>.
# Resumable: skips files already present on-device with matching size.
#
# Usage: deliver_game.py <PFN> <local_root> [ondevice_relbase]
#   e.g. deliver_game.py Greenhouse.LethalCo_0.0.1.0_x64__mgw46vk3ppz9w \
#          artifacts/msix_stage_lethalco/guest/LethalCompany  game/LethalCompany
import sys, os, time, uuid, http.client
sys.path.insert(0, '$HOME/.claude/jobs/49fb5998/tmp')
import wdp
from urllib.parse import quote

CHUNK_LIMIT = 300 * 1024 * 1024   # files bigger than this are split into parts (WDP large-POST unreliable)

def conn(timeout=1800):
    return http.client.HTTPSConnection(wdp.HOST, wdp.PORT, context=wdp.ctx, timeout=timeout)

def mkdir(pfn, parent, name):
    p = (f'/api/filesystem/apps/folder?knownfolderid=LocalAppData'
         f'&packagefullname={pfn}&path={quote(parent)}&newfoldername={quote(name)}')
    try: return wdp.req('POST', p, headers={'Content-Length': '0'})
    except Exception as e: return -1, str(e)

def mkdirp(pfn, rel):
    """Create every level of a '\'-joined path under \\LocalState (idempotent)."""
    cur = '\\LocalState'
    for part in rel.split('\\'):
        if not part: continue
        mkdir(pfn, cur, part)   # ignore 'already exists'
        cur = cur + '\\' + part

def listdir(pfn, folder):
    """Return {name: size} on-device, for resume-skip. Empty on error."""
    p = (f'/api/filesystem/apps/files?knownfolderid=LocalAppData'
         f'&packagefullname={pfn}&path={quote(folder)}')
    try:
        st, d = wdp.req('GET', p)
        if st == 200 and isinstance(d, dict):
            return {i.get('Id'): i.get('FileSize', -1) for i in d.get('Items', []) if i.get('Type') != 16}
    except Exception:
        pass
    return {}

def upload_one(pfn, folder, localpath, destname, timeout=1800):
    with open(localpath, 'rb') as f: content = f.read()
    boundary = '----ghd' + uuid.uuid4().hex
    pre = (f'--{boundary}\r\nContent-Disposition: form-data; name="x"; filename="{destname}"\r\n'
           f'Content-Type: application/octet-stream\r\n\r\n').encode()
    post = f'\r\n--{boundary}--\r\n'.encode()
    body = pre + content + post
    h = dict(wdp.HDR); h['Content-Type'] = f'multipart/form-data; boundary={boundary}'; h['Content-Length'] = str(len(body))
    path = (f'/api/filesystem/apps/file?knownfolderid=LocalAppData'
            f'&packagefullname={pfn}&path={quote(folder)}')
    c = conn(timeout); c.request('POST', path, body=body, headers=h)
    r = c.getresponse(); d = r.read(); c.close()
    return r.status, d[:200]

def upload_retry(pfn, folder, localpath, destname, size, tries=4):
    # scale socket timeout to size: ~0.3 MB/s floor, 180s min, 2400s cap.
    timeout = max(180, min(2400, int(size / 300000) + 120))
    for k in range(tries):
        try:
            st, d = upload_one(pfn, folder, localpath, destname, timeout=timeout)
            if st in (200, 202): return True, st
            if b'not enough space' in (d if isinstance(d, bytes) else str(d).encode()).lower():
                return 'NOSPACE', d   # hard capacity stop -- retrying is pointless
            print(f"      try{k} -> {st} {d!r}", flush=True)
        except Exception as e:
            print(f"      try{k} EXC {e}", flush=True)
        time.sleep(3 + k * 3)
    return False, None

def main():
    pfn = sys.argv[1]; local_root = os.path.abspath(sys.argv[2])
    relbase = (sys.argv[3] if len(sys.argv) > 3 else 'game/' + os.path.basename(local_root)).replace('/', '\\')

    files = []
    for dp, _dn, fns in os.walk(local_root):
        for fn in fns:
            full = os.path.join(dp, fn)
            rel = os.path.relpath(full, local_root).replace('/', '\\')   # e.g. Lethal Company_Data\Managed\x.dll
            files.append((full, rel, os.path.getsize(full)))
    print(f"[deliver] {len(files)} files, root={local_root} -> \\LocalState\\{relbase}")

    # 1. create all directories (relbase + every file's parent chain), depth-sorted
    dirs = {relbase}
    for _f, rel, _s in files:
        parts = rel.split('\\')[:-1]
        for i in range(len(parts)):
            dirs.add(relbase + '\\' + '\\'.join(parts[:i + 1]))
    for d in sorted(dirs, key=lambda x: x.count('\\')):
        mkdirp(pfn, d)
    print(f"[deliver] ensured {len(dirs)} dirs")

    # 2. upload, smallest first, resumable (skip present w/ matching size)
    files.sort(key=lambda t: t[2])
    ok = skip = fail = 0; failed = []; listcache = {}
    for idx, (full, rel, sz) in enumerate(files):
        subdir = '\\'.join(rel.split('\\')[:-1]); name = rel.split('\\')[-1]
        folder = '\\LocalState\\' + relbase + (('\\' + subdir) if subdir else '')
        if folder not in listcache: listcache[folder] = listdir(pfn, folder)
        if listcache[folder].get(name) == sz:
            skip += 1; continue
        print(f"[{idx+1}/{len(files)}] {rel}  ({sz/1048576:.1f} MB)", flush=True)
        good, st = upload_retry(pfn, folder, full, name, sz)
        if good == 'NOSPACE':
            print(f"[deliver] ABORT: device out of space at {rel} "
                  f"(delivered {ok} this run). Free space and re-run (resumable).", flush=True)
            sys.exit(3)
        if good: ok += 1
        else: fail += 1; failed.append(rel)
    print(f"[deliver] DONE ok={ok} skipped={skip} failed={fail}", flush=True)
    if failed:
        print("[deliver] FAILED FILES:"); [print("   ", x) for x in failed]
        sys.exit(1)

if __name__ == '__main__':
    main()
