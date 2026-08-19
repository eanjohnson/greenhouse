#!/usr/bin/env python3
# Greenhouse mouse bridge -- PC side.
#
# Reads a USB mouse via Linux evdev and streams its relative motion + button
# state over TCP to the Xbox app (which cannot receive hardware mouse through
# the platform HID stack, but CAN open an outbound socket). The Xbox app
# connects to us; we push fixed 8-byte packets:
#     <B B h h h>  = magic(0x5A), buttons, dx, dy, wheel   (little-endian)
# buttons bit0=Left bit1=Right bit2=Middle.
#
# Usage: mouse_stream.py [device] [port]
#   device defaults to the Logitech G502 by-id node; port 55123.
import socket, struct, threading, sys, time, os

DEV  = sys.argv[1] if len(sys.argv) > 1 else "/dev/input/by-id/usb-Logitech_G502_HERO_Gaming_Mouse_146A33463431-event-mouse"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 55123

# struct input_event on 64-bit: tv_sec(q) tv_usec(q) type(H) code(H) value(i)
FMT = "qqHHi"; SZ = struct.calcsize(FMT)
EV_SYN, EV_KEY, EV_REL = 0x00, 0x01, 0x02
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
BTN = {0x110: 1, 0x111: 2, 0x112: 4}   # LEFT, RIGHT, MIDDLE -> bitmask

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

def clamp16(v): return max(-32768, min(32767, v))

def serve():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT)); s.listen(1)
    print(f"listening on 0.0.0.0:{PORT}", flush=True)
    while True:
        c, addr = s.accept(); print("client connected:", addr, flush=True)
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            while True:
                with lock:
                    dx, dy, wheel, b = st["dx"], st["dy"], st["wheel"], st["buttons"]
                    st["dx"] = st["dy"] = st["wheel"] = 0
                c.sendall(struct.pack("<BBhhh", 0x5A, b & 0xFF, clamp16(dx), clamp16(dy), clamp16(wheel)))
                time.sleep(0.008)   # ~125 Hz
        except Exception as e:
            print("client disconnected:", e, flush=True)

threading.Thread(target=reader, daemon=True).start()
serve()
