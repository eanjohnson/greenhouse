#!/usr/bin/env python3
# Greenhouse mouse bridge -- PC side, CLIENT mode (connects TO the Xbox).
#
# The loader now LISTENS on the Xbox (0.0.0.0:55123). We connect to it and push
# the physical mouse's relative motion + buttons. This is the direction that
# routes when the Xbox is reached remotely via a VPN subnet router (same
# path WDP uses), unlike the old model where the Xbox dialed out to a LAN PC.
#
# Packet: <B B h h h> = magic(0x5A), buttons, dx, dy, wheel  (little-endian)
#   buttons bit0=Left bit1=Right bit2=Middle
#
# Usage: mouse_stream_client.py <xbox_ip> [port] [device]
#   xbox_ip may also come from GH_XBOX; device from GH_MOUSE_DEV.
import os, socket, struct, threading, sys, time

XBOX = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("GH_XBOX", "")
if not XBOX:
    sys.exit("usage: mouse_stream_client.py <xbox_ip> [port] [device]  (or set GH_XBOX)")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 55123
DEV  = sys.argv[3] if len(sys.argv) > 3 else \
    os.environ.get("GH_MOUSE_DEV", "/dev/input/by-id/usb-_Mouse_for_Windows-event-mouse")

FMT = "qqHHi"; SZ = struct.calcsize(FMT)
EV_SYN, EV_KEY, EV_REL = 0x00, 0x01, 0x02
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
BTN = {0x110: 1, 0x111: 2, 0x112: 4}

st = {"dx": 0, "dy": 0, "wheel": 0, "buttons": 0}
lock = threading.Lock()

def reader():
    while True:
        try:
            f = open(DEV, "rb", buffering=0)
        except Exception as e:
            print("open failed:", e, "-- retrying", flush=True); time.sleep(1); continue
        print("reading", DEV, flush=True)
        dx = dy = wheel = 0
        try:
            while True:
                data = f.read(SZ)
                if not data or len(data) < SZ: break
                _, _, etype, code, value = struct.unpack(FMT, data)
                if etype == EV_REL:
                    if code == REL_X: dx += value
                    elif code == REL_Y: dy += value
                    elif code == REL_WHEEL: wheel += value
                elif etype == EV_KEY and code in BTN:
                    with lock:
                        if value: st["buttons"] |= BTN[code]
                        else:     st["buttons"] &= ~BTN[code]
                elif etype == EV_SYN:
                    if dx or dy or wheel:
                        with lock:
                            st["dx"] += dx; st["dy"] += dy; st["wheel"] += wheel
                        dx = dy = wheel = 0
        except OSError as e:
            # The mouse re-enumerated (e.g. Errno 19 ENODEV: event21 -> event22).
            # Break out, close, and re-open the by-id symlink (udev repoints it to
            # the live node) so the reader survives USB re-enumeration.
            print("read error:", e, "-- reopening device", flush=True)
        finally:
            try: f.close()
            except Exception: pass
        time.sleep(0.4)

def clamp16(v): return max(-32768, min(32767, v))

def stream():
    while True:
        try:
            c = socket.create_connection((XBOX, PORT), timeout=10)
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"connected to Xbox {XBOX}:{PORT} -- move the mouse", flush=True)
        except Exception as e:
            print(f"connect {XBOX}:{PORT} failed: {e} -- retry", flush=True); time.sleep(2); continue
        try:
            while True:
                with lock:
                    dx, dy, wheel, b = st["dx"], st["dy"], st["wheel"], st["buttons"]
                    st["dx"] = st["dy"] = st["wheel"] = 0
                c.sendall(struct.pack("<BBhhh", 0x5A, b & 0xFF, clamp16(dx), clamp16(dy), clamp16(wheel)))
                time.sleep(0.008)   # ~125 Hz
        except Exception as e:
            print("disconnected:", e, "-- reconnecting", flush=True)
            try: c.close()
            except Exception: pass
            time.sleep(1)

threading.Thread(target=reader, daemon=True).start()
stream()
