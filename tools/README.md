# MarauderBox Remote (phone control over WiFi)

Drive your MarauderBox from your phone's browser, no app and no firmware
changes. The Marauder stays plugged into a computer by USB; this script bridges
its serial CLI to a web page your phone opens over WiFi.

The page looks and navigates like the Marauder's own screen: tap a category
(WiFi / Bluetooth / Remote ID / GPS / Device), tap into submenus, tap an action
to run it. ~120 commands are one tap away (ones that need an index/value prompt
you for it), there's a free-text box for anything else, a big STOP / FORCE STOP,
and a live serial console.

```
[ Marauder ] --USB--> [ computer running marauder_remote.py ] <--WiFi--> [ phone browser ]
```

**What it can and can't do:** it drives the Marauder's SERIAL command-line, so it
covers the WiFi / Bluetooth / GPS / attack / list / device commands. The
touch-only NyanBox screens (Device Scout, Camera Detect, Drone Remote ID views,
Cast) have no serial command, so they can't be triggered remotely.

## Setup

1. Install Python 3.8+ and pyserial:
   ```
   pip install pyserial
   ```
2. Plug the Marauder into the computer with a USB **data** cable. Close the
   Marauder flasher / any serial monitor (only one program can hold the port).
3. Run it:
   ```
   python3 marauder_remote.py
   ```
   It auto-detects the port and prints a URL like `http://192.168.1.42:8080/`.
4. On your phone (same WiFi), open that URL. You get command buttons, a
   free-text command box, and a live serial console.

## Options

```
python3 marauder_remote.py --list-ports            # show serial ports
python3 marauder_remote.py --port /dev/ttyUSB0     # pick the port (or COM5 on Windows)
python3 marauder_remote.py --baud 115200
python3 marauder_remote.py --http-port 8080
python3 marauder_remote.py --token mysecret        # require ?t=mysecret on the URL
```

## Notes

- Navigate like the device: tap **WiFi > Attacks > Deauth**, etc. The title bar
  shows where you are; **‹ Back** goes up a level.
- The big **STOP** sends `stopscan` (**FORCE STOP** = `stopscan -f`). Attacks like
  deauth / beacon-list need targets first: **WiFi > Sniffers > Scan APs+STAs**,
  then **WiFi > Target List > Select ALL APs** (or Select AP by index).
- Actions that need a value (an AP index, a channel, a hex color) pop a prompt.
- The free-text box runs **any** CLI command, so you are not limited to the menu.
  Type `help` for the device's full command list.
- By default the server listens on all interfaces so your phone can reach it.
  Anyone on the same network can control it -- use `--token` on untrusted WiFi,
  or bind to a specific interface with `--host`.
- Authorized use only: this is a remote control for a device you own.
