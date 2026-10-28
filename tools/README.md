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

## Picking targets (no typing)

**WiFi > Select APs / SSIDs** reads the device's AP list and shows it:
- **By SSID** — tap a network name to select **every** AP broadcasting it (one
  tap). The count (e.g. `MyWiFi (1/3)`) and check mark show how many are picked.
- **Individual APs** — tap one to toggle just it.
- **Refresh / Select ALL / Clear ALL** at the top.

First do **WiFi > Sniffers > Scan APs+STAs**, press **STOP**, then open the
picker (it runs `list -a` for you). Selections feed the attacks (deauth, etc.).

## Phone on a different network? Use a public link (`--tunnel`)

On a locked-down network (apartment/campus/office Wi-Fi that puts devices on
different subnets, or when you just want to use cellular), the same-WiFi URL
won't work. `--tunnel` gives you a **public https link** your phone can open from
**anywhere**:

```
brew install cloudflared          # one time (macOS; see cloudflare docs for others)
python3 marauder_remote.py --tunnel
```

**No Homebrew?** Download the `cloudflared` binary straight into the folder with
the script (the tool looks for it there -- no install, no PATH):

```
# macOS, run in the folder that has marauder_remote.py:
A=$([ "$(uname -m)" = arm64 ] && echo arm64 || echo amd64)
curl -L -o cloudflared.tgz \
  https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-$A.tgz
tar -xzf cloudflared.tgz && chmod +x cloudflared && rm cloudflared.tgz
python3 marauder_remote.py --tunnel
```
(or point the tool at a binary anywhere with `--cloudflared /path/to/cloudflared`).

It prints an `https://<random>.trycloudflare.com/?t=<token>` link (and a QR). Open
it on the phone over any network. Notes:
- A **token is required and auto-generated** for tunnels, since the link is on the
  internet. Keep the link private.
- The link lives only while the script runs -- **Ctrl-C closes it**.
- No Cloudflare account needed (it's a free quick tunnel).

## Same-WiFi not working?

The script prints **every** candidate URL on startup -- try each; the right one
is on your phone's subnet. If none work:
- phone + computer on the **same WiFi** (not a guest network, not cellular);
- **allow Python through the firewall** (inbound TCP on the port);
- some routers block device-to-device traffic ("AP/client isolation") -- disable it;
- if your phone and computer got **different subnets** (e.g. `10.60.29.x` vs
  `10.60.39.x`), the network blocks device-to-device -- use `--tunnel` instead.
- `pip install qrcode` to also print a scannable QR of the URL.

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
