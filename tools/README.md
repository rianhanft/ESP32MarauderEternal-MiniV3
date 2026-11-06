# MarauderBox Remote (`marauder_remote.py`)

Drive your MarauderBox from your **phone's browser** over WiFi. A computer with
the Marauder plugged in via USB runs this script; it bridges the Marauder's
serial command line to a small web page you open on your phone.

```
[ Marauder ] --USB serial--> [ computer running this script ] <--WiFi--> [ phone browser ]
```

You get a Marauder-style menu you navigate by tapping, a free-text command box,
and a live, color-coded serial console.

> **Authorized use only.** This is a remote control for a device you own.

---

## Requirements

- **Python 3.8+**
- **pyserial** — `pip install pyserial`
- A Marauder flashed with firmware that has the **serial CLI** (this fork does).
- Optional:
  - **qrcode** (`pip install qrcode`) — prints a scannable QR for the URL.
  - **cloudflared** — only needed for the public `--tunnel` option (see below).

---

## Quick start (same WiFi)

1. Plug the Marauder into your computer with a USB **data** cable.
2. Install the dependency:
   ```
   pip install pyserial
   ```
3. Run it (the serial port is auto-detected):
   ```
   python3 marauder_remote.py
   ```
4. It prints something like:
   ```
   ================================================================
     MarauderBox Remote ready
     Serial : /dev/ttyUSB0 @ 115200    HTTP port: 8080
     Open on your phone (same WiFi):
         http://192.168.1.42:8080
   ================================================================
   ```
5. On your **phone** (connected to the **same WiFi**), open that
   `http://<computer-ip>:8080` URL.
6. Tap through the menu, type commands, watch the console. Press **Ctrl-C** in
   the terminal to stop.

If auto-detect picks the wrong port, or nothing is found, see
[Finding the serial port](#finding-the-serial-port).

---

## Options

| Flag | Default | What it does |
|------|---------|--------------|
| `--port PORT` | auto-detect | Serial port (e.g. `COM5`, `/dev/ttyUSB0`, `/dev/cu.usbmodem1101`). |
| `--baud N` | `115200` | Serial baud rate. |
| `--http-port N` | `8080` | Web server port. |
| `--host ADDR` | `0.0.0.0` | Bind address (all interfaces by default). |
| `--token SECRET` | *(none)* | Require `?t=SECRET` on the URL — a simple guard on a shared LAN. |
| `--tunnel` | off | Expose a **public https link** via cloudflared (phone does **not** need the same WiFi). |
| `--cloudflared PATH` | auto | Path to the `cloudflared` binary (otherwise found on `PATH` or next to this script). |
| `--assert-dtr` | off | Hold DTR/RTS asserted on open (legacy). Only use it if a board stays silent — see note below. |
| `--list-ports` | — | List serial ports and exit. |

### Examples
```
python3 marauder_remote.py                              # auto port, serve on :8080
python3 marauder_remote.py --port COM5                  # Windows, explicit port
python3 marauder_remote.py --port /dev/ttyUSB0 --baud 115200 --http-port 8080
python3 marauder_remote.py --token mysecret             # require ?t=mysecret on the URL
python3 marauder_remote.py --tunnel                     # public link, reachable anywhere
```

---

## Finding the serial port

List what's connected and exit:
```
python3 marauder_remote.py --list-ports
```
Typical names:
- **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`
- **macOS:** `/dev/cu.usbserial-XXXX` or `/dev/cu.usbmodemXXXX` (the script prefers the `cu.*` form automatically)
- **Windows:** `COM3`, `COM5`, …

Then pass it with `--port`.

---

## Remote over the internet (`--tunnel`)

`--tunnel` opens a public HTTPS link through Cloudflare, so your phone can reach
the Marauder from **any** network (even cellular) — no port forwarding.

```
python3 marauder_remote.py --tunnel
```

- It prints a `https://<random>.trycloudflare.com/...` link (and a QR if `qrcode`
  is installed).
- A **token is auto-generated** and baked into the link, because a public URL is
  internet-reachable — keep the link private.
- The link **stops working the moment you quit the script** (Ctrl-C).

**cloudflared** must be available. Either:
- install it (`brew install cloudflared`, your package manager, or the
  [Cloudflare releases](https://github.com/cloudflare/cloudflared/releases)),
- drop the `cloudflared` binary next to this script, or
- point to it with `--cloudflared /path/to/cloudflared`.

If it's missing, the script prints exactly how to fetch it for your OS.

---

## Native-USB boards (T-Dongle C5) — the DTR note

By default the script leaves the DTR/RTS lines **low** on open, so native
USB-Serial-JTAG boards (like the **LilyGo T-Dongle C5**) don't get yanked into
the bootloader / reset when you connect. This is what you want almost always.

Only if a board connects but **stays completely silent** should you try:
```
python3 marauder_remote.py --assert-dtr
```

---

## What it can and can't do

- ✅ Everything on the Marauder's **serial command line**: WiFi / BLE / GPS
  scans, the attack commands, list management, Evil Portal (with AP + HTML
  pickers), wardrive, etc.
- ❌ **Touch-only screens** have no serial command and can't be triggered
  remotely: Device Scout, Camera Detect, the Drone Remote ID detail views, Cast.

---

## Troubleshooting

- **"No serial port found."** Plug the Marauder in with a data cable, close any
  other serial monitor using the port, then pass `--port` (see `--list-ports`).
- **Phone can't load the page.** Confirm the phone is on the **same WiFi**, and
  that your computer's **firewall** allows inbound connections on the HTTP port
  (8080). Corporate/guest networks often block device-to-device traffic — use
  `--tunnel` instead.
- **Connected but the console is empty / no response.** Check the baud
  (`--baud 115200`). On a native-USB board that stays silent, try `--assert-dtr`.
- **`--tunnel` says cloudflared not found.** Install it or pass `--cloudflared`
  (see above).
- **macOS "port busy".** Another app (Arduino IDE serial monitor, screen) is
  holding the port — close it.
