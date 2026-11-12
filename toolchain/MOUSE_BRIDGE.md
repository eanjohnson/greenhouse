# Remote mouse bridge for Greenhouse on Xbox

Streams a physical USB mouse from a Linux laptop to a Greenhouse loader running
on an Xbox dev-mode console. Motion + buttons + wheel arrive as raw-input events
inside the guest game, so titles on Unity's new Input System (Lethal Company,
etc.) see real clicks without a controller.

## What you need

- A Linux box with the mouse plugged in over USB and your user in the `input`
  group (`groups | grep input`). If not: `sudo usermod -aG input $USER`, then
  log out and back in.
- Python 3 on that box (stdlib only, no deps).
- Network route from the laptop to the Xbox on TCP 55123 (same LAN, or a
  VPN subnet router, or anything else that lets a socket dial through).
- A Greenhouse guest already running on the Xbox with the mouse bridge listener
  compiled in — the loader listens on `0.0.0.0:55123` and accepts one connection
  at a time.

The script does **not** require anything from the developer's own environment.
No `wdp.py`, no Cecil, no build tools. Just a mouse and a network path.

## Find your mouse device

```
ls /dev/input/by-id/ | grep -i mouse
```

You want the `*-event-mouse` entry (evdev, not the older `-mouse` node).
Example outputs:

- `usb-Logitech_G502_HERO_Gaming_Mouse_146A33463431-event-mouse`
- `usb-_Mouse_for_Windows-event-mouse`
- `usb-Razer_Razer_DeathAdder_V2-event-mouse`

Copy the full path — `/dev/input/by-id/<that>`.

Quick sanity check that you can read it:

```
head -c 24 /dev/input/by-id/usb-_Mouse_for_Windows-event-mouse >/dev/null && echo OK
```

If you get a permission error, you're not in `input` yet.

## Run it

```
python3 mouse_stream_client.py <xbox-ip> [port] [device]
```

Defaults: port `55123`, device `/dev/input/by-id/usb-_Mouse_for_Windows-event-mouse`
(or `GH_MOUSE_DEV`). The console IP is required: the first argument, or `GH_XBOX`.
Arguments are positional. Typical:

```
python3 mouse_stream_client.py <console-ip>
```

Expected output on connect:

```
reading /dev/input/by-id/usb-_Mouse_for_Windows-event-mouse
connected to Xbox <console-ip>:55123 -- move the mouse
```

Move the mouse. On the Xbox, the cursor tracks in the game; left/right/middle
clicks arrive as raw-input mouse-button edges. The bridge sends at ~125 Hz and
auto-reconnects if the socket drops or the mouse re-enumerates.

`Ctrl-C` to stop.

## Troubleshooting

- **`open failed: [Errno 13] Permission denied`** — user isn't in `input`. Fix
  as above, or run under `sudo` for a one-off (`sudo -E python3 …`).
- **`connect …:55123 failed: [Errno 111] Connection refused`** — the guest
  isn't running, or the loader isn't the mouse-bridge-enabled build. Launch
  the app; wait a few seconds for the listener to come up; the script
  auto-retries every 2 s.
- **`connect …:55123 failed: [Errno 113] No route to host`** — you're on a
  network that can't reach the Xbox. Try a `ping <xbox-ip>` first.
- **Cursor moves but clicks do nothing** — old loader build (pre-raw-input).
  Modern classic-Input-Manager games see clicks via `WM_LBUTTONDOWN`; new
  Input System games see them only via raw input. Update the loader.
- **Wrong device streaming** — you can plug several USB mice into the same
  laptop; each shows up separately in `/dev/input/by-id/`. Pass the exact
  `-event-mouse` path as the third argument to pick one.

## Wire protocol (if you want to write your own client)

Little-endian, one packet per state sample, ~125 Hz:

```
struct { u8 magic; u8 buttons; s16 dx; s16 dy; s16 wheel; };
```

- `magic` = `0x5A`
- `buttons` bitmask: `1<<0` = left, `1<<1` = right, `1<<2` = middle
- `dx`, `dy` in device units (relative), clamped to `[-32768, 32767]`
- `wheel` in ticks, same clamp

That's the whole protocol. TCP, `TCP_NODELAY`, no framing beyond the fixed
8-byte record.
