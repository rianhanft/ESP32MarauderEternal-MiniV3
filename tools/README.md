# MarauderBox Remote (phone control over WiFi)

Drive your MarauderBox from your phone's browser, no app and no firmware
changes. The Marauder stays plugged into a computer by USB; this script bridges
its serial CLI to a small web page your phone opens over WiFi.

```
[ Marauder ] --USB--> [ computer running marauder_remote.py ] <--WiFi--> [ phone browser ]
```

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

- The big **STOP** button sends `stopscan`. Attacks like deauth/beacon-list need
  you to `select -a <n>` (or "Select ALL APs") after a `scanall` + `list -a`.
- The free-text box runs **any** CLI command, so you are not limited to the
  buttons. Type `help` for the full list.
- By default the server listens on all interfaces so your phone can reach it.
  Anyone on the same network can control it -- use `--token` on untrusted WiFi,
  or bind to a specific interface with `--host`.
- Authorized use only: this is a remote control for a device you own.
