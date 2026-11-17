# MarauderBox Remote (`Remote.py`)

Drive your MarauderBox from your **phone's browser**. A computer with the
Marauder plugged in via USB runs `Remote.py`; it bridges the Marauder's serial
command line to a web page you open on your phone — a tappable Marauder-style
menu, a command box, and a live serial console.

```
[ Marauder ] --USB serial--> [ computer runs Remote.py ] <--WiFi/internet--> [ phone browser ]
```

> **Authorized use only.** This is a remote control for a device you own.

---

## Control from your phone, anywhere (`--tunnel`)

This is the easiest way — your phone does **not** need to be on the same WiFi
(works on cellular too). Assuming `Remote.py` is in your **Downloads** folder:

```
cd ~/Downloads
python3 Remote.py --tunnel
```

That prints a public link and a QR code:

```
  OPEN THIS ON YOUR PHONE (any network, even cellular):
      https://something-random.trycloudflare.com/?t=XXXXXX

  ^ or scan the QR above.
```

**Scan the QR** (or open the link) on your phone and you're in. Press **Ctrl-C**
in the terminal to stop — the link dies the moment you quit.

**First time?** You need two things in place:

1. **pyserial** (one-time):
   ```
   pip3 install pyserial
   ```
2. **cloudflared** — the tunnel helper. Easiest: install it once with Homebrew:
   ```
   brew install cloudflared
   ```
   No Homebrew? Download the binary straight into Downloads next to `Remote.py`:
   ```
   cd ~/Downloads
   A=$(uname -m | sed 's/x86_64/amd64/;s/arm64/arm64/')
   curl -L -o cloudflared.tgz \
     https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-$A.tgz
   tar -xzf cloudflared.tgz && chmod +x cloudflared && rm cloudflared.tgz
   ```
   `Remote.py` auto-finds `cloudflared` on your PATH **or** sitting next to it in
   Downloads — nothing else to configure.

> The tunnel URL is internet-reachable, so a secret token is auto-added to it.
> Keep the link private; it stops working as soon as you Ctrl-C.

---

## Control from your phone on the same WiFi (no internet)

If your phone and computer are on the **same WiFi**, you don't need the tunnel:

```
cd ~/Downloads
python3 Remote.py
```

It prints a local address — open it on your phone:

```
  MarauderBox Remote ready
  Serial : /dev/cu.usbserial-XXXX @ 115200    HTTP port: 8080
  Open on your phone (same WiFi):
      http://192.168.1.42:8080
```

(Only needs **pyserial** — `pip3 install pyserial`. No cloudflared required.)

---

## Requirements

- **Python 3.8+**
- **pyserial** — `pip3 install pyserial`
- A Marauder flashed with firmware that has the **serial CLI** (this fork does).
- For `--tunnel` only: **cloudflared** (see above).
- Optional: **qrcode** (`pip3 install qrcode`) to print a scannable QR in the terminal.

---

## Options

| Flag | Default | What it does |
|------|---------|--------------|
| `--tunnel` | off | Public https link via cloudflared — phone reachable from anywhere. |
| `--port PORT` | auto-detect | Serial port (e.g. `/dev/cu.usbserial-XXXX`, `/dev/ttyUSB0`, `COM5`). |
| `--baud N` | `115200` | Serial baud rate. |
| `--http-port N` | `8080` | Web server port. |
| `--host ADDR` | `0.0.0.0` | Bind address (all interfaces by default). |
| `--token SECRET` | *(none)* | Require `?t=SECRET` on the URL — a simple guard on a shared LAN. |
| `--cloudflared PATH` | auto | Path to the `cloudflared` binary (otherwise PATH or next to `Remote.py`). |
| `--assert-dtr` | off | Hold DTR/RTS asserted on open (legacy) — only if a board stays silent. |
| `--list-ports` | — | List serial ports and exit. |

### More examples
```
python3 Remote.py --port COM5                  # Windows, explicit port
python3 Remote.py --port /dev/ttyUSB0 --baud 115200
python3 Remote.py --token mysecret             # require ?t=mysecret on the URL
```

---

## Finding the serial port

Usually auto-detected. To see what's connected:
```
python3 Remote.py --list-ports
```
Typical names:
- **macOS:** `/dev/cu.usbserial-XXXX` or `/dev/cu.usbmodemXXXX` (the `cu.*` form is preferred automatically)
- **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`
- **Windows:** `COM3`, `COM5`, …

Then pass it with `--port`.

---

## Native-USB boards (T-Dongle C5) — the DTR note

By default `Remote.py` leaves the DTR/RTS lines **low** on open, so native
USB-Serial-JTAG boards (the **LilyGo T-Dongle C5**) don't get reset into the
bootloader when you connect. That's what you want almost always. Only if a board
connects but stays **completely silent**, try:
```
python3 Remote.py --assert-dtr
```

---

## What it can and can't do

- ✅ Everything on the Marauder's **serial command line**: WiFi / BLE / GPS
  scans, the attack commands, list management, Evil Portal (with AP + HTML
  pickers), wardrive, etc.
- ❌ **Touch-only screens** have no serial command and can't be driven remotely:
  Device Scout, Camera Detect, the Drone Remote ID detail views, Cast.

---

## Troubleshooting

- **"No serial port found."** Plug the Marauder in with a **data** cable, close
  any other serial monitor using the port, then pass `--port` (see `--list-ports`).
- **`--tunnel` says cloudflared not found.** Install it (`brew install cloudflared`)
  or drop the `cloudflared` binary in Downloads next to `Remote.py`.
- **Phone can't load the local (same-WiFi) page.** Confirm the phone is on the
  **same WiFi**, and that your computer's **firewall** allows inbound on port 8080.
  Guest/corporate WiFi often blocks device-to-device — use `--tunnel` instead.
- **Connected but the console is empty.** Check `--baud 115200`. On a native-USB
  board that stays silent, try `--assert-dtr`.
- **macOS "port busy".** Another app (Arduino serial monitor, `screen`) holds the
  port — close it.
