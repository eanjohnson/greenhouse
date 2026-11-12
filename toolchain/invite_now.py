#!/usr/bin/env python3
# Remote invite TRIGGER for Lethal Company on the Xbox.
#
# The shim polls <CheeseMP-vfs LocalState>\gh_invite_now.txt on its ~10s heartbeat; when it
# appears AND a lobby is live (g_gh_hosted_lobby), it fires ONE targeted invite to the
# recipient stored in gh_invite_target.txt, then deletes the trigger. Pure edge-trigger.
#
# Usage:
#   python3 invite_now.py <steamid64>   # store recipient (once) AND fire the trigger now
#   python3 invite_now.py               # fire the trigger now (recipient already stored)
#   python3 invite_now.py --set <sid>   # store recipient only, do NOT fire
#
# Recipient stays exclusively in gh_invite_target.txt (single target, no fanout).
import sys, time, os, importlib.util
_tools = os.environ.get('GH_TOOLS')
if not _tools:
    sys.exit('set GH_TOOLS to the directory holding wdp.py (the Device Portal client)')
spec = importlib.util.spec_from_file_location('wdp', os.path.join(_tools, 'wdp.py'))
wdp = importlib.util.module_from_spec(spec); sys.argv = ['wdp']; spec.loader.exec_module(wdp)

PFN = 'Greenhouse.LethalCo_0.0.16.0_x64__mgw46vk3ppz9w'
# gh_target_path()/gh_trigger_path() now resolve to the LethalCo package's OWN LocalState
# (the direct app dir). This is the path WDP-pushed files are reliably READABLE from (the
# CheeseMP vfs-mirror path is write-redirected but external reads don't resolve back there).
SHIM_DIR = '\\LocalState'
TMP = _tools

def put(destname, localpath):
    wdp.fsupload(PFN, SHIM_DIR, localpath, destname)

def set_target(sid):
    sid = str(sid).strip()
    if not (sid.isdigit() and len(sid) == 17 and (int(sid) >> 32) == 0x01100001):
        print(f'REFUSING: {sid!r} is not a 17-digit SteamID64 (individual-account range)'); sys.exit(2)
    p = f'{TMP}/gh_invite_target.txt'
    with open(p, 'w') as f: f.write(sid)
    print(f'store recipient -> gh_invite_target.txt = {sid}')
    put('gh_invite_target.txt', p)

def fire():
    p = f'{TMP}/gh_invite_now.txt'
    with open(p, 'w') as f: f.write(f'fire {int(time.time())}\n')  # content ignored; pure "when"
    print('drop trigger    -> gh_invite_now.txt')
    put('gh_invite_now.txt', p)
    print('OK: shim will fire ONE invite within ~10s IF a lobby is live '
          '(watch gh_shim.log for "invite trigger -> firing" / "IGNORED: no active lobby").')

args = sys.argv[1:]
if args and args[0] == '--set':
    if len(args) < 2: print('usage: invite_now.py --set <steamid64>'); sys.exit(1)
    set_target(args[1])
elif args:
    set_target(args[0]); fire()
else:
    fire()
