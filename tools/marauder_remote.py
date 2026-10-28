#!/usr/bin/env python3
"""
marauder_remote.py -- drive your MarauderBox from your phone over WiFi.

How it works:
  [ Marauder ] --USB serial--> [ this computer runs this script ] <--WiFi--> [ phone browser ]

Plug the Marauder into a computer with USB, run this script, then open the URL
it prints on your phone's browser (same WiFi network). You get a Marauder-style
menu you navigate by tapping, a free-text command box, and a live serial console.

Requires: Python 3.8+ and pyserial  ->  pip install pyserial

Examples:
  python3 marauder_remote.py                 # auto-detect port, serve on :8080
  python3 marauder_remote.py --port COM5
  python3 marauder_remote.py --port /dev/ttyUSB0 --baud 115200 --http-port 8080
  python3 marauder_remote.py --token mysecret # require ?t=mysecret (simple LAN guard)

Note: this drives the Marauder's SERIAL command-line, so it covers the WiFi /
BLE / GPS / attack / list commands. The touch-only NyanBox screens (Device
Scout, Camera Detect, Drone Remote ID views, Cast) have no serial command and
cannot be triggered remotely.

Authorized use only: this is a remote control for a tool you own.
"""
import argparse
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote_plus

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required.  Install it with:  pip install pyserial")

# --- serial plumbing ----------------------------------------------------------

BUF_MAX = 250_000  # keep ~last 250 KB of serial output in memory


class SerialBridge:
    def __init__(self, port, baud):
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.port = port
        self.baud = baud
        self._buf = bytearray()
        self._base = 0  # absolute offset of _buf[0]
        self._lock = threading.Lock()
        self._alive = True
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        while self._alive:
            try:
                data = self.ser.read(4096)
            except Exception as e:
                data = f"\n[serial error: {e}]\n".encode()
                self._alive = False
            if data:
                with self._lock:
                    self._buf.extend(data)
                    if len(self._buf) > BUF_MAX:
                        drop = len(self._buf) - BUF_MAX
                        del self._buf[:drop]
                        self._base += drop
            else:
                time.sleep(0.02)

    def send(self, command: str):
        line = (command.rstrip("\r\n") + "\n").encode("utf-8", "replace")
        self.ser.write(line)
        self.ser.flush()

    def read_since(self, pos: int):
        with self._lock:
            end = self._base + len(self._buf)
            if pos < 0:  # tail: last ~4 KB, for the console on first load
                pos = max(self._base, end - 4000)
            if pos < self._base:
                pos = self._base
            start = pos - self._base
            chunk = bytes(self._buf[start:])
        return end, chunk.decode("latin-1")

    def _end(self):
        with self._lock:
            return self._base + len(self._buf)

    def send_and_collect(self, command, settle=0.5, timeout=2.5):
        """Send a command and return the output it produces (until the device
        goes quiet for `settle` seconds, or `timeout` elapses)."""
        start = self._end()
        self.send(command)
        last_len, idle_since = start, time.time()
        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(0.06)
            end = self._end()
            if end != last_len:
                last_len, idle_since = end, time.time()
            elif time.time() - idle_since >= settle and end > start:
                break
        _, text = self.read_since(start)
        return text


# --- Marauder-style menu tree (mirrors the device menu, exact same order) -----
# Node keys: t=title c=color ic=icon m=submenu x=command p=prompt(arg -> {})
#            pick="ap"|"ssid"|"html"   dev=True (device-screen only, no CLI)

MENU = [
  {"t": "Recon", "c": "mag", "ic": "\U0001F4E1", "m": [
    {"t": "Start WiFi Recon", "c": "grn", "x": "recon wifi"},
    {"t": "Start BLE Recon", "c": "cy", "x": "recon ble"},
    {"t": "Recon Status", "c": "grn", "x": "recon status"},
    {"t": "Stop Recon", "c": "red", "x": "recon stop"},
  ]},
  {"t": "WiFi", "c": "grn", "ic": "\U0001F4F6", "m": [
    {"t": "Sniffers", "c": "yel", "ic": "\U0001F3A7", "m": [
      {"t": "Probe Request Sniff", "c": "cy", "x": "sniffprobe"},
      {"t": "Beacon Sniff", "c": "mag", "x": "sniffbeacon"},
      {"t": "Deauth Sniff", "c": "red", "x": "sniffdeauth"},
      {"t": "Packet Count", "c": "org", "x": "packetcount"},
      {"t": "EAPOL/PMKID Scan", "c": "pur", "x": "sniffpmkid"},
      {"t": "Packet Monitor", "c": "blu", "dev": True},
      {"t": "Channel Analyzer", "c": "cy", "dev": True},
      {"t": "Channel Summary", "c": "org", "dev": True},
      {"t": "Raw Capture", "c": "wht", "x": "sniffraw"},
      {"t": "Detect Pwnagotchi", "c": "red", "x": "sniffpwn"},
      {"t": "Detect Pineapple", "c": "yel", "x": "sniffpinescan"},
      {"t": "Detect MultiSSID", "c": "org", "x": "sniffmultissid"},
      {"t": "Scan AP/STA", "c": "lime", "x": "scanall"},
      {"t": "MAC Monitor", "c": "mag", "x": "mactrack"},
      {"t": "SAE Commit", "c": "lime", "x": "sniffsae"},
      {"t": "Wardrive", "c": "grn", "x": "wardrive"},
    ]},
    {"t": "Scanners", "c": "org", "ic": "\U0001F50E", "m": [
      {"t": "Ping Scan", "c": "grn", "x": "pingscan"},
      {"t": "ARP Scan", "c": "cy", "x": "arpscan"},
      {"t": "Port Scan All (IP idx)", "c": "mag", "x": "portscan -a -t {}", "p": "IP index"},
      {"t": "SSH Scan", "c": "org", "x": "portscan -s ssh"},
      {"t": "Telnet Scan", "c": "red", "x": "portscan -s telnet"},
      {"t": "SMTP Scan", "c": "wht", "x": "portscan -s smtp"},
      {"t": "DNS Scan", "c": "lime", "x": "portscan -s dns"},
      {"t": "HTTP Scan", "c": "sky", "x": "portscan -s http"},
      {"t": "HTTPS Scan", "c": "yel", "x": "portscan -s https"},
      {"t": "RDP Scan", "c": "pur", "x": "portscan -s rdp"},
    ]},
    {"t": "Attacks", "c": "red", "ic": "\U0001F4A5", "m": [
      {"t": "Beacon Spam List", "c": "red", "x": "attack -t beacon -l"},
      {"t": "Beacon Spam Random", "c": "org", "x": "attack -t beacon -a"},
      {"t": "Funny SSID Beacon", "c": "cy", "x": "attack -t funny"},
      {"t": "Rick Roll Beacon", "c": "yel", "x": "attack -t rickroll"},
      {"t": "Probe Req Flood", "c": "red", "x": "attack -t probe"},
      {"t": "Evil Portal (pick AP)", "c": "org", "pick": "ep"},
      {"t": "Deauth Flood", "c": "red", "x": "attack -t deauth"},
      {"t": "AP Clone Spam", "c": "mag", "dev": True},
      {"t": "Deauth Targeted", "c": "red", "x": "attack -t deauth"},
      {"t": "Karma (SSID idx)", "c": "org", "x": "karma -p {}", "p": "probe SSID index"},
      {"t": "Bad Msg", "c": "red", "x": "attack -t badmsg"},
      {"t": "Bad Msg Targeted", "c": "yel", "x": "attack -t badmsg -c"},
      {"t": "Assoc Sleep", "c": "red", "x": "attack -t sleep"},
      {"t": "Assoc Sleep Targ", "c": "mag", "x": "attack -t sleep -c"},
      {"t": "SAE Commit Flood", "c": "lime", "x": "attack -t sae"},
      {"t": "Channel Switch", "c": "org", "x": "attack -t csa"},
      {"t": "Quiet Time", "c": "red", "x": "attack -t quiet"},
    ]},
    {"t": "General", "c": "pur", "ic": "\U0001F9F0", "m": [
      {"t": "Select EP HTML File", "c": "org", "pick": "html"},
      {"t": "Select APs", "c": "grn", "pick": "ap"},
      {"t": "Select by SSID", "c": "grn", "pick": "ssid"},
      {"t": "View AP Info (idx)", "c": "cy", "x": "info -a {}", "p": "AP index"},
      {"t": "Select Stations (idx)", "c": "grn", "x": "select -s {}", "p": "Station index"},
      {"t": "Join WiFi (AP idx)", "c": "grn", "x": "join -a {}", "p": "AP index"},
      {"t": "Join Saved WiFi", "c": "grn", "x": "join -s"},
      {"t": "Set MACs", "c": "wht", "dev": True},
      {"t": "Shutdown WiFi", "c": "red", "x": "stopscan"},
      {"t": "Upload Wardrive Logs", "c": "grn", "x": "upload -d wigle"},
    ]},
  ]},
  {"t": "Bluetooth", "c": "cy", "ic": "\U0001F535", "m": [
    {"t": "Sniffers", "c": "yel", "ic": "\U0001F3A7", "m": [
      {"t": "Bluetooth Sniff", "c": "grn", "x": "sniffbt"},
      {"t": "Flipper Sniff", "c": "org", "x": "sniffbt -t flipper"},
      {"t": "FindMy Sniff", "c": "wht", "x": "sniffbt -t airtag"},
      {"t": "iBeacon Sniff", "c": "cy", "x": "sniffbt -t ibeacon"},
      {"t": "FindMy Monitor", "c": "wht", "x": "sniffbt -t airtag"},
      {"t": "Detect Card Skimmers", "c": "mag", "x": "sniffskim"},
      {"t": "Bluetooth Analyzer", "c": "cy", "dev": True},
      {"t": "Flock Sniff", "c": "org", "x": "sniffbt -t flock"},
      {"t": "Meta Detect", "c": "wht", "x": "sniffbt -t meta"},
    ]},
    {"t": "Bluetooth Attacks", "c": "red", "ic": "\U0001F4A5", "m": [
      {"t": "Sour Apple", "c": "red", "x": "blespam -t sourapple"},
      {"t": "Apple Juice", "c": "red", "x": "blespam -t applejuice"},
      {"t": "Swiftpair Spam", "c": "red", "x": "blespam -t windows"},
      {"t": "Samsung BLE Spam", "c": "red", "x": "blespam -t samsung"},
      {"t": "Google BLE Spam", "c": "red", "x": "blespam -t google"},
      {"t": "Flipper BLE Spam", "c": "red", "x": "blespam -t flipper"},
      {"t": "BLE Spam All", "c": "red", "x": "blespam -t all"},
      {"t": "Spoof Airtag (idx)", "c": "org", "x": "spoofat -t {}", "p": "device index"},
      {"t": "FindMy Sound (idx)", "c": "cy", "x": "findmy -t {}", "p": "device index"},
    ]},
  ]},
  {"t": "GPS", "c": "red", "ic": "\U0001F4CD", "m": [
    {"t": "GPS Data", "c": "red", "x": "gpsdata"},
    {"t": "NMEA Stream", "c": "org", "x": "nmea"},
    {"t": "GPS Tracker Start", "c": "grn", "x": "gpstracker -c start"},
    {"t": "GPS Tracker Stop", "c": "red", "x": "gpstracker -c stop"},
    {"t": "GPS POI Start", "c": "cy", "x": "gpspoi -s"},
    {"t": "GPS POI Mark", "c": "cy", "x": "gpspoi -m"},
    {"t": "GPS POI End", "c": "red", "x": "gpspoi -e"},
  ]},
  {"t": "NyanBox", "c": "mag", "ic": "\U0001F431", "m": [
    {"t": "Sniffers", "c": "yel", "ic": "\U0001F3A7", "m": [
      {"t": "Device Scout", "c": "grn", "dev": True},
      {"t": "Jam Detector", "c": "red", "dev": True},
      {"t": "Camera Detect", "c": "cy", "dev": True},
      {"t": "Drone Remote ID Scan", "c": "org", "x": "remoteid scan"},
      {"t": "Drone Remote ID List", "c": "cy", "x": "remoteid list"},
      {"t": "Drone Track (idx)", "c": "mag", "x": "remoteid track {}", "p": "drone index"},
      {"t": "Drone Remote ID Clear", "c": "red", "x": "remoteid clear"},
    ]},
    {"t": "Attacks", "c": "red", "ic": "\U0001F4A5", "m": [
      {"t": "Camera Deauther", "c": "red", "dev": True},
      {"t": "Drone Spoof", "c": "red", "dev": True},
      {"t": "Drone Spoof (SD)", "c": "red", "dev": True},
    ]},
    {"t": "BLE Detectors", "c": "cy", "ic": "\U0001F535", "m": [
      {"t": "Meshtastic Sniff", "c": "grn", "dev": True},
      {"t": "MeshCore Sniff", "c": "cy", "dev": True},
      {"t": "SmartTag Sniff", "c": "yel", "dev": True},
      {"t": "Tile Sniff", "c": "org", "dev": True},
      {"t": "Axon Sniff", "c": "red", "dev": True},
      {"t": "NyanBOX Sniff", "c": "mag", "dev": True},
    ]},
  ]},
  {"t": "Device", "c": "blu", "ic": "⚙", "m": [
    {"t": "Update Firmware (SD)", "c": "org", "x": "update -s"},
    {"t": "Backup SPIFFS", "c": "grn", "x": "backupspiffs"},
    {"t": "Restore SPIFFS", "c": "org", "x": "restorespiffs"},
    {"t": "Save APs", "c": "cy", "x": "save -a"},
    {"t": "Save SSIDs", "c": "cy", "x": "save -s"},
    {"t": "Load APs", "c": "cy", "x": "load -a"},
    {"t": "Load SSIDs", "c": "cy", "x": "load -s"},
    {"t": "Brightness Max", "c": "yel", "x": "brightness -s 9"},
    {"t": "Brightness Mid", "c": "yel", "x": "brightness -s 5"},
    {"t": "Brightness Low", "c": "yel", "x": "brightness -s 1"},
    {"t": "Device Info", "c": "wht", "x": "protocolinfo"},
    {"t": "Settings Reset", "c": "red", "x": "settings -r"},
    {"t": "Setting set...", "c": "cy", "x": "settings -s {}", "p": "e.g. ForcePMKID enable"},
    {"t": "Geofences List", "c": "grn", "x": "geofence list"},
    {"t": "Channel set...", "c": "cy", "x": "channel -s {}", "p": "channel #"},
    {"t": "LED color (hex)", "c": "mag", "x": "led -s {}", "p": "hex e.g. FF0000"},
    {"t": "SD File List", "c": "cy", "x": "ls /"},
    {"t": "Help (full list)", "c": "grn", "x": "help"},
  ]},
  {"t": "Reboot", "c": "red", "ic": "↻", "x": "reboot"},
]

PAGE = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>MarauderBox Remote</title>
<style>
  :root{
    --bg:#05070a; --bezel:#1a1f27; --bezel2:#2a313c; --screen:#04070b;
    --cy:#2fd3e6; --red:#ff5c5c; --grn:#45e08a; --mag:#d07bff; --yel:#ffd23f;
    --org:#ffae45; --blu:#5aa0ff; --wht:#dfe8f2; --navy:#13213f; --mut:#7f93a8;
  }
  *{box-sizing:border-box;-webkit-tap-highlight-color:transparent;}
  body{margin:0;background:#05070a;color:#dfe8f2;
    font:15px/1.4 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
    display:flex;flex-direction:column;align-items:center;min-height:100vh;}
  .device{width:100%;max-width:420px;margin:10px auto 0;background:#1a1f27;
    border:1px solid #2a313c;border-radius:22px;padding:14px;
    box-shadow:0 10px 40px rgba(0,0,0,.6);}
  .screen{background:#04070b;border:2px solid #2a313c;border-radius:10px;overflow:hidden;
    height:62vh;min-height:360px;display:flex;flex-direction:column;}
  .statusbar{background:#13213f;color:#2fd3e6;display:flex;align-items:center;gap:8px;
    padding:7px 10px;font-size:13px;border-bottom:1px solid #223;}
  .statusbar .ttl{font-weight:700;letter-spacing:.3px;flex:1;
    overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
  #dot{width:9px;height:9px;border-radius:50%;background:#ff5c5c;flex:0 0 auto;}
  #dot.ok{background:#45e08a;}
  .list{flex:1;overflow-y:auto;padding:7px;display:flex;flex-direction:column;gap:6px;}
  .row{display:flex;align-items:center;gap:9px;padding:11px 11px;border-radius:9px;
    background:#0c141d;border:1px solid #16212e;border-left-width:4px;cursor:pointer;
    font-size:15px;min-height:46px;}
  .row:active{background:#14202c;}
  .row .ic{width:20px;text-align:center;font-size:15px;flex:0 0 auto;}
  .row .lab{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;
    color:#e6edf3;font-weight:500;}
  .row .chev{color:#7f93a8;font-size:17px;flex:0 0 auto;}
  .row.back{background:#0a1118;border-left-color:#7f93a8;}
  .row.back .lab{color:#9fb4c8;font-weight:600;}
  .row.on{background:#0f2018;}
  .row.dev{opacity:.5;}
  .pbar{display:flex;gap:6px;margin:2px 0 4px;}
  .pbtn{flex:1;background:#13213f;border:1px solid #244;color:#bfe0f0;border-radius:8px;
    padding:10px 4px;font-size:13px;font-weight:600;cursor:pointer;}
  .pbtn:active{background:#1b2d50;}
  .plbl{color:#7f93a8;font-size:11px;text-transform:uppercase;letter-spacing:.06em;
    margin:8px 2px 1px;}
  .c-cy{border-left-color:#35d9ea;}  .c-cy  .lab,.c-cy  .ic{color:#35d9ea;}
  .c-red{border-left-color:#ff6b6b;} .c-red .lab,.c-red .ic{color:#ff8e8e;}
  .c-grn{border-left-color:#46e08a;} .c-grn .lab,.c-grn .ic{color:#5fe59c;}
  .c-mag{border-left-color:#d98bff;} .c-mag .lab,.c-mag .ic{color:#d98bff;}
  .c-yel{border-left-color:#ffd84a;} .c-yel .lab,.c-yel .ic{color:#ffe070;}
  .c-org{border-left-color:#ffb057;} .c-org .lab,.c-org .ic{color:#ffc07a;}
  .c-blu{border-left-color:#6aa6ff;} .c-blu .lab,.c-blu .ic{color:#8fbcff;}
  .c-wht{border-left-color:#dfe8f2;} .c-wht .lab,.c-wht .ic{color:#eef4fb;}
  .c-lime{border-left-color:#b6e34a;} .c-lime .lab,.c-lime .ic{color:#c6ea6f;}
  .c-pur{border-left-color:#b98bff;} .c-pur .lab,.c-pur .ic{color:#c4a0ff;}
  .c-sky{border-left-color:#67c6ff;} .c-sky .lab,.c-sky .ic{color:#8ad3ff;}
  .stopwrap{padding:7px;border-top:1px solid #223;display:flex;gap:7px;}
  .stop{flex:1;background:#3a1113;border:1px solid #6e2024;color:#ff8a8a;font-weight:800;
    border-radius:9px;padding:13px;font-size:16px;cursor:pointer;letter-spacing:.5px;}
  .stop:active{background:#4a1518;}
  .stop.alt{flex:0 0 40%;font-weight:700;font-size:13px;color:#ffb;background:#2a2410;border-color:#5a4a1a;}
  .console{width:100%;max-width:420px;margin:10px auto 16px;}
  .cmdbar{display:flex;gap:7px;margin-bottom:7px;}
  .cmdbar input{flex:1;min-width:0;font:inherit;color:#dfe8f2;background:#0b1118;
    border:1px solid #223;border-radius:9px;padding:11px;}
  .cmdbar button{background:#2fd3e6;color:#042026;border:none;border-radius:9px;
    font-weight:700;padding:0 15px;cursor:pointer;}
  .conhead{display:flex;align-items:center;gap:7px;padding:7px 11px;background:#0a1016;
    border:1px solid #1b2a38;border-bottom:none;border-radius:10px 10px 0 0;color:#6f8498;
    font-size:11px;letter-spacing:.09em;text-transform:uppercase;}
  .conhead .cdot{width:7px;height:7px;border-radius:50%;background:#45e08a;
    box-shadow:0 0 6px #45e08a;flex:0 0 auto;}
  .conhead b{color:#cfe3f2;font-weight:700;}
  .conhead .cport{margin-left:auto;color:#6f8498;text-transform:none;letter-spacing:0;
    font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;}
  #log{background:linear-gradient(180deg,#04080c,#060b11);border:1px solid #1b2a38;
    border-top:none;border-radius:0 0 10px 10px;padding:9px 11px;height:27vh;overflow:auto;
    word-break:break-word;box-shadow:inset 0 10px 22px rgba(0,0,0,.45);
    font:12px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;}
  #log .ln{white-space:pre-wrap;padding:1px 3px;border-radius:3px;margin:0 -1px;}
  #log .cmd{color:#38dced;font-weight:600;}
  #log .cmd::before{content:"\276F ";color:#2a7f8c;}
  #log .ap{color:#5fe59c;}
  #log .ok{color:#ffe070;}
  #log .err{color:#ff8e8e;background:rgba(255,90,90,.08);}
  #log .dim{color:#9ab4cc;}
  /* activity banner: what the console is actively doing */
  .activity{display:flex;align-items:center;gap:10px;margin:11px 0;padding:11px 13px;
    border-radius:12px;border:1px solid #1b2a38;background:#0a1016;color:#9fb4c8;
    font-size:13px;transition:background .25s,border-color .25s,color .25s;}
  .activity .apulse{width:10px;height:10px;border-radius:50%;background:#45506a;flex:0 0 auto;}
  .activity .atext{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}
  .activity .atext b{font-weight:400;opacity:.72;margin-right:5px;}
  .activity .astop{display:none;background:#ff5a5a;color:#2a0505;border:none;border-radius:8px;
    font-weight:800;letter-spacing:.03em;padding:6px 13px;cursor:pointer;font-size:12px;flex:0 0 auto;}
  .activity.run{background:linear-gradient(90deg,rgba(69,224,138,.13),rgba(10,16,22,.6));
    border-color:#2f6e55;color:#e3f7ec;}
  .activity.atk{background:linear-gradient(90deg,rgba(255,90,90,.15),rgba(10,16,22,.6));
    border-color:#7a2f2f;color:#ffe2e2;}
  .activity.run .apulse{background:#45e08a;animation:apulse 1.3s ease-in-out infinite;}
  .activity.atk .apulse{background:#ff6b6b;animation:apulse 1s ease-in-out infinite;}
  .activity.run .astop,.activity.atk .astop{display:inline-block;}
  .activity.shot{border-color:#2f6e8c;color:#dcefff;}
  .activity.shot .apulse{background:#38a9ff;}
  @keyframes apulse{0%,100%{transform:scale(1);opacity:1;}50%{transform:scale(1.55);opacity:.5;}}
  .tools{display:flex;gap:8px;align-items:center;margin-top:8px;color:#7f93a8;font-size:12px;}
  .tools button{padding:6px 10px;background:#0d1620;border:1px solid #223;border-radius:7px;
    color:#9fb4c8;font-size:12px;cursor:pointer;}
</style></head>
<body>
<div class="device">
  <div class="screen">
    <div class="statusbar"><span id="dot"></span>
      <span class="ttl" id="title">MarauderBox</span>
      <span id="port" style="color:#7f93a8;font-size:11px"></span></div>
    <div class="list" id="list"></div>
    <div class="stopwrap">
      <button class="stop" onclick="send('stopscan')">STOP</button>
      <button class="stop alt" onclick="send('stopscan -f')">FORCE STOP</button>
    </div>
  </div>
</div>
<div class="activity idle" id="activity">
  <span class="apulse"></span>
  <span class="atext" id="atext">Idle — tap an action to begin</span>
  <button class="astop" onclick="send('stopscan')">Stop</button>
</div>
<div class="console">
  <div class="cmdbar">
    <input id="cmd" placeholder="type any command (e.g. select -a 0)" autocomplete="off"
      autocapitalize="off" spellcheck="false">
    <button onclick="sendInput()">Send</button>
  </div>
  <div class="conhead"><span class="cdot"></span><b>Serial Console</b>
    <span class="cport" id="cport">—</span></div>
  <div id="log"></div>
  <div class="tools">
    <button onclick="clearLog()">Clear</button>
    <label><input type="checkbox" id="auto" checked> autoscroll</label>
    <span id="stat"></span>
  </div>
</div>
<script>
  const TOKEN = __TOKEN__;
  const MENU = __MENU__;
  const $ = s => document.querySelector(s);
  const tok = () => TOKEN ? "&t=" + encodeURIComponent(TOKEN) : "";
  const stack = [];            // breadcrumb of {title, nodes}
  let picker = null;           // AP picker state when active

  function send(c, label){
    toast(c);
    appendLog("# " + c + "\n");
    setActivity(label || c, classifyAction(c));
    return fetch("/cmd?c=" + encodeURIComponent(c) + tok())
      .then(r => { if(!r.ok) throw 0; flash(); })
      .catch(() => { setStat("send failed"); failActivity(); });
  }
  function run(node){
    let c = node.x;
    if(c.indexOf("{}") >= 0){
      const v = prompt(node.p || "value", "");
      if(v === null || v === "") return;
      c = c.replace("{}", v.trim());
    }
    send(c, node.t);
  }
  function sendInput(){ const i=$("#cmd"); if(i.value.trim()){ send(i.value.trim()); i.value=""; } }
  $("#cmd").addEventListener("keydown", e => { if(e.key==="Enter") sendInput(); });
  function flash(){ const d=$("#dot"); d.style.boxShadow="0 0 9px #2fd3e6"; setTimeout(()=>d.style.boxShadow="",160); }
  let statT;
  function setStat(s){ $("#stat").textContent=s; }
  function toast(c){ setStat("sent: " + c); clearTimeout(statT); statT=setTimeout(()=>setStat(""),2200); }

  // ---- activity banner: tell the user what the console is actively doing ----
  let shotT, curAct = {label:"", kind:"idle"};
  function classifyAction(c){
    const s = c.trim().toLowerCase();
    if(/^(stopscan|reboot)\b/.test(s) || s==="recon stop"
       || /gpstracker\s+-c\s+stop/.test(s) || /gpspoi\s+-e\b/.test(s)
       || /remoteid\s+clear\b/.test(s)) return "stop";
    if(/^(attack|blespam|karma|spoofat)\b/.test(s) || /evilportal\s+-c\s+start/.test(s))
      return "atk";
    if(/^(scanall|sniff|packetcount|mactrack|wardrive|pingscan|arpscan|portscan|recon|nmea|gpsdata|findmy)\b/.test(s)
       || /gpstracker\s+-c\s+start/.test(s) || /gpspoi\s+-s\b/.test(s)
       || /remoteid\s+(scan|track)\b/.test(s)) return "run";
    return "shot";   // one-shot: select/save/load/info/join/ls/help/settings/etc.
  }
  function setActivity(label, kind){
    const bar=$("#activity"), txt=$("#atext");
    clearTimeout(shotT);
    if(kind==="stop"){
      curAct={label:"", kind:"idle"};
      bar.className="activity idle";
      txt.innerHTML = /reboot/i.test(label) ? "<b>Rebooting…</b>" : "<b>Idle —</b> stopped";
      return;
    }
    if(kind==="shot"){                       // transient flash, then restore running state
      bar.className="activity shot";
      txt.innerHTML = "<b>Ran</b>" + esc(label);
      shotT=setTimeout(applyActivity, 2600);
      return;
    }
    curAct={label, kind};                    // run / atk are persistent
    applyActivity();
  }
  function applyActivity(){
    const bar=$("#activity"), txt=$("#atext");
    if(curAct.kind==="idle"){
      bar.className="activity idle";
      txt.innerHTML = "<b>Idle —</b> tap an action to begin";
      return;
    }
    bar.className = "activity " + curAct.kind;
    txt.innerHTML = "<b>" + (curAct.kind==="atk" ? "Attacking" : "Running") + "</b>"
                    + esc(curAct.label);
  }
  function failActivity(){
    clearTimeout(shotT);
    const bar=$("#activity"); bar.className="activity idle";
    $("#atext").innerHTML = "<b>Send failed —</b> check the connection";
  }
  function esc(s){ const d=document.createElement("div"); d.textContent=String(s); return d.innerHTML; }

  function render(){
    picker = null;
    const cur = stack[stack.length-1];
    $("#title").textContent = (cur.ic ? cur.ic + "  " : "") + cur.title;
    const list = $("#list"); list.innerHTML = "";
    if(stack.length > 1){
      const b = rowEl("‹ Back", "back", ""); b.onclick = () => { stack.pop(); render(); };
      list.appendChild(b);
    }
    for(const n of cur.nodes){
      const sub = !!n.m, pick = !!n.pick, dev = !!n.dev;
      const el = rowEl(n.t, "c-" + (n.c||"cy") + (dev?" dev":""),
                       sub ? "›" : (pick ? "⠿" : ""), n.ic);
      el.onclick = sub  ? () => { stack.push({title:n.t, nodes:n.m, ic:n.ic}); render(); }
                 : pick ? () => (n.pick==="html" ? openHtmlPicker() : openPicker(n.pick))
                 : dev  ? () => setStat("“"+n.t+"” is a device-screen feature (no remote command)")
                 : () => run(n);
      list.appendChild(el);
    }
    list.scrollTop = 0;
  }
  function rowEl(text, cls, chev, icon){
    const d=document.createElement("div"); d.className="row "+cls;
    if(icon){ const i=document.createElement("span"); i.className="ic"; i.textContent=icon; d.appendChild(i); }
    const s=document.createElement("span"); s.className="lab"; s.textContent=text; d.appendChild(s);
    if(chev){ const c=document.createElement("span"); c.className="chev"; c.textContent=chev; d.appendChild(c); }
    return d;
  }

  // ---- AP / SSID picker ----  (mode: "ap"/"ssid" = select targets, "ep" = host Evil Portal)
  let pickerMode = "ap";
  function openPicker(mode){
    pickerMode = mode || "ap";
    $("#title").textContent = pickerMode==="ep" ? "Evil Portal: pick AP" : "Select APs";
    const list = $("#list"); list.innerHTML = "";
    const l = rowEl("Loading AP list…", "c-cy", ""); list.appendChild(l);
    fetch("/aplist" + (TOKEN ? "?t=" + encodeURIComponent(TOKEN) : ""))
      .then(r => r.json()).then(j => { picker = j.aps || []; renderPicker(); })
      .catch(() => { list.innerHTML=""; list.appendChild(rowEl("Could not read AP list.", "c-red", "")); });
  }
  function hostPortal(i){
    const target = picker[i];
    // Make ONLY this AP selected (select toggles), then start the portal.
    const toggles = [];
    picker.forEach((a,k)=>{ if(a.sel && k!==i) toggles.push(a.i); }); // turn others off
    if(!target.sel) toggles.push(target.i);                           // turn this on
    const go = () => send("evilportal -c start", "Evil Portal on “" + (target.ssid||"(hidden)") + "”");
    (toggles.length ? send("select -a " + toggles.join(",")) : Promise.resolve()).then(go);
    setStat("Hosting Evil Portal on “" + (target.ssid||"(hidden)") + "”");
  }
  function selCsv(indices){ return indices.join(","); }
  function applySel(indices, want){
    // toggle only the ones that need changing so we reach `want` state
    const todo = indices.filter(i => picker[i].sel !== want);
    if(!todo.length) return;
    send("select -a " + selCsv(todo.map(i=>picker[i].i)));
    todo.forEach(i => picker[i].sel = want);
    renderPicker();
  }
  function renderPicker(){
    const list = $("#list"); list.innerHTML = "";
    const back = rowEl("‹ Back", "back", ""); back.onclick = () => render(); list.appendChild(back);
    // action bar
    const bar = document.createElement("div"); bar.className = "pbar";
    bar.appendChild(mkBtn("⟳ Refresh", ()=>openPicker(pickerMode)));
    if(pickerMode!=="ep"){
      bar.appendChild(mkBtn("Select ALL", ()=>applySel(picker.map((_,i)=>i), true)));
      bar.appendChild(mkBtn("Clear ALL", ()=>applySel(picker.map((_,i)=>i), false)));
    }
    list.appendChild(bar);
    if(!picker.length){
      list.appendChild(rowEl("No APs. Run WiFi ▸ Sniffers ▸ Scan, press STOP, then Refresh.", "c-org", ""));
      return;
    }
    if(pickerMode==="ep"){
      list.appendChild(lbl("Tap the AP to host the Evil Portal on"));
      picker.forEach((a,i)=>{
        const el = pickRow("📶  " + (a.ssid||"(hidden)"), "CH"+a.ch+"  "+a.rssi+"dBm", false);
        el.onclick = () => hostPortal(i);
        list.appendChild(el);
      });
      list.scrollTop = 0;
      return;
    }
    // group by SSID
    const groups = {};
    picker.forEach((a,i)=>{ const k=a.ssid||"(hidden)"; (groups[k]=groups[k]||[]).push(i); });
    list.appendChild(lbl("By SSID — tap to select every matching AP"));
    Object.keys(groups).sort((a,b)=>a.toLowerCase()<b.toLowerCase()?-1:1).forEach(k=>{
      const idxs = groups[k];
      const selN = idxs.filter(i=>picker[i].sel).length;
      const mark = selN===idxs.length ? "✓" : (selN? "◪" : "·");
      const el = pickRow(mark + "  " + k, "("+selN+"/"+idxs.length+")", selN===idxs.length);
      el.onclick = () => applySel(idxs, selN!==idxs.length); // all->clear else select all
      list.appendChild(el);
    });
    // individual APs
    list.appendChild(lbl("Individual APs — tap to toggle one"));
    picker.forEach((a,i)=>{
      const mark = a.sel ? "✓" : "·";
      const el = pickRow(mark + "  [" + a.i + "] " + (a.ssid||"(hidden)"),
                         a.rssi + "dBm CH" + a.ch, a.sel);
      el.onclick = () => applySel([i], !a.sel);
      list.appendChild(el);
    });
    list.scrollTop = 0;
  }
  function pickRow(text, right, on){
    const d=document.createElement("div"); d.className="row "+(on?"c-grn on":"c-blu");
    const s=document.createElement("span"); s.textContent=text; s.style.overflow="hidden";
    s.style.textOverflow="ellipsis"; s.style.whiteSpace="nowrap"; d.appendChild(s);
    const r=document.createElement("span"); r.className="chev"; r.textContent=right; r.style.fontSize="12px";
    d.appendChild(r); return d;
  }
  function lbl(t){ const d=document.createElement("div"); d.className="plbl"; d.textContent=t; return d; }
  function mkBtn(t,fn){ const b=document.createElement("button"); b.className="pbtn"; b.textContent=t; b.onclick=fn; return b; }

  // ---- Evil Portal HTML picker ----
  function openHtmlPicker(){
    $("#title").textContent = "Evil Portal HTML";
    const list = $("#list"); list.innerHTML = "";
    list.appendChild(rowEl("Reading SD card…", "c-cy", "", ""));
    fetch("/htmls" + (TOKEN ? "?t=" + encodeURIComponent(TOKEN) : ""))
      .then(r => r.json()).then(j => renderHtmls(j.htmls || []))
      .catch(() => { list.innerHTML=""; list.appendChild(rowEl("Could not read SD card.", "c-red", "", "")); });
  }
  function renderHtmls(files){
    const list = $("#list"); list.innerHTML = "";
    const back = rowEl("‹ Back", "back", "", ""); back.onclick = () => render(); list.appendChild(back);
    const bar = document.createElement("div"); bar.className = "pbar";
    bar.appendChild(mkBtn("⟳ Refresh", ()=>openHtmlPicker()));
    list.appendChild(bar);
    list.appendChild(lbl("Tap an .html to arm Evil Portal with it"));
    if(!files.length){
      list.appendChild(rowEl("No .html files on the SD card root.", "c-org", "", ""));
      return;
    }
    files.forEach(f => {
      const el = rowEl(f, "c-org", "set", "📄");
      el.onclick = () => { send("evilportal -c sethtml " + f);
        setStat("armed: " + f + "  (now WiFi ▸ Attacks ▸ Evil Portal)"); };
      list.appendChild(el);
    });
  }

  stack.push({title:"MarauderBox", nodes:MENU});
  render();

  // live serial console: color-coded, line-buffered terminal
  let pos=-1, first=true, pend="", liveEl=null;
  const RE_AP=/^\[\d+\]\[CH:\d+\]/;
  function classifyLine(s){
    const t=s.trim();
    if(!t) return "dim";
    if(t[0]==="#") return "cmd";
    if(RE_AP.test(t)) return "ap";
    if(/\b(error|errors|failed|failure|invalid|unknown|can't|cannot|not found|no such|too large|denied|timeout|timed out)\b/i.test(t)) return "err";
    if(/\b(start|starting|started|stop|stopped|stopping|found|done|complete|completed|finished|ready|saved|loaded|connected|launched|success|successful|enabled|disabled|ok)\b/i.test(t)) return "ok";
    return "dim";
  }
  function addLine(s){
    const log=$("#log"), cls=classifyLine(s);
    const div=document.createElement("div");
    div.className="ln "+cls;
    div.textContent = cls==="cmd" ? s.replace(/^#\s?/,"") : s;
    log.appendChild(div);
    while(log.childNodes.length>1500) log.removeChild(log.firstChild);
  }
  function appendLog(text){
    const log=$("#log");
    if(liveEl && liveEl.parentNode===log) log.removeChild(liveEl);
    liveEl=null;
    pend += text;
    const parts=pend.split("\n");
    pend=parts.pop();
    for(const ln of parts) addLine(ln.replace(/\r$/,""));
    if(pend){ liveEl=document.createElement("div"); liveEl.className="ln "+classifyLine(pend);
      liveEl.textContent=pend; log.appendChild(liveEl); }
    if($("#auto").checked) log.scrollTop=log.scrollHeight;
  }
  function clearLog(){ $("#log").innerHTML=""; pend=""; liveEl=null; }
  function poll(){
    fetch("/out?pos=" + pos + tok()).then(r=>r.json()).then(j=>{
      $("#dot").classList.add("ok");
      if(j.port){ $("#port").textContent=j.port; const cp=$("#cport"); if(cp) cp.textContent=j.port; }
      if(first){ first=false; pos=j.pos; if(j.data) appendLog(j.data); }
      else if(j.data){ pos=j.pos; appendLog(j.data); }
    }).catch(()=>{ $("#dot").classList.remove("ok"); setStat("disconnected"); })
      .finally(()=>setTimeout(poll,700));
  }
  poll();
</script>
</body></html>"""


# --- parse `list -a` output ("[0][CH:6] MyWiFi -42 (selected)") ---------------

AP_RE = re.compile(r"^\[(\d+)\]\[CH:(\d+)\]\s(.*)$")


def parse_ap_list(text):
    aps = []
    for raw in text.splitlines():
        m = AP_RE.match(raw.strip())
        if not m:
            continue
        idx, ch, rest = int(m.group(1)), int(m.group(2)), m.group(3)
        sel = rest.endswith("(selected)")
        if sel:
            rest = rest[: -len("(selected)")].rstrip()
        parts = rest.rsplit(None, 1)  # rssi is the trailing token; essid is the rest
        if len(parts) == 2 and parts[1].lstrip("-").isdigit():
            ssid, rssi = parts[0], int(parts[1])
        else:
            ssid, rssi = rest, 0
        aps.append({"i": idx, "ch": ch, "ssid": ssid, "rssi": rssi, "sel": sel})
    return aps


HTML_RE = re.compile(r"[\w.\-/]+\.html\b", re.IGNORECASE)


def parse_html_list(text):
    """Pull .html filenames out of an `ls /` listing (Evil Portal templates)."""
    seen, out = set(), []
    for tok in HTML_RE.findall(text):
        name = tok.rsplit("/", 1)[-1]
        if name.lower() not in seen:
            seen.add(name.lower())
            out.append(name)
    return out


# --- HTTP server --------------------------------------------------------------

def make_handler(bridge, token):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _auth_ok(self, qs):
            return (not token) or (qs.get("t", [""])[0] == token)

        def _send(self, code, body, ctype="text/html; charset=utf-8"):
            data = body.encode("utf-8") if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urlparse(self.path)
            qs = parse_qs(u.query)
            if u.path == "/":
                # When a token is set (always true for a public tunnel), the page
                # itself requires it, so a bare link can't drive the Marauder.
                if token and not self._auth_ok(qs):
                    return self._send(403, "Add ?t=<token> to the URL.", "text/plain")
                page = (PAGE
                        .replace("__TOKEN__", json.dumps(token or ""))
                        .replace("__MENU__", json.dumps(MENU)))
                return self._send(200, page)
            if u.path == "/cmd":
                if not self._auth_ok(qs):
                    return self._send(403, "forbidden", "text/plain")
                cmd = unquote_plus(qs.get("c", [""])[0])
                if cmd:
                    try:
                        bridge.send(cmd)
                    except Exception as e:
                        return self._send(500, f"serial error: {e}", "text/plain")
                return self._send(200, "ok", "text/plain")
            if u.path == "/aplist":
                if not self._auth_ok(qs):
                    return self._send(403, "forbidden", "text/plain")
                try:
                    text = bridge.send_and_collect("list -a")
                except Exception as e:
                    return self._send(500, f"serial error: {e}", "text/plain")
                return self._send(200, json.dumps({"aps": parse_ap_list(text)}),
                                   "application/json")
            if u.path == "/htmls":
                if not self._auth_ok(qs):
                    return self._send(403, "forbidden", "text/plain")
                try:
                    text = bridge.send_and_collect("ls /")
                except Exception as e:
                    return self._send(500, f"serial error: {e}", "text/plain")
                return self._send(200, json.dumps({"htmls": parse_html_list(text)}),
                                   "application/json")
            if u.path == "/out":
                if not self._auth_ok(qs):
                    return self._send(403, "forbidden", "text/plain")
                try:
                    pos = int(qs.get("pos", ["0"])[0])
                except ValueError:
                    pos = 0
                end, data = bridge.read_since(pos)
                return self._send(200,
                    json.dumps({"pos": end, "data": data,
                                "port": f"{bridge.port} @ {bridge.baud}"}),
                    "application/json")
            return self._send(404, "not found", "text/plain")
    return Handler


# --- helpers ------------------------------------------------------------------

def autodetect_port():
    hints = ("usbserial", "wchusbserial", "ttyusb", "ttyacm", "slab", "cp210",
             "ch340", "ch910", "cu.usb", "usbmodem")
    ports = list(list_ports.comports())
    for p in ports:
        blob = f"{p.device} {p.description} {p.manufacturer}".lower()
        if any(h in blob for h in hints):
            return p.device
    return ports[0].device if ports else None


def lan_ips():
    """All candidate LAN IPv4 addresses, the internet-routable one first."""
    ips = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.append(s.getsockname()[0])
        s.close()
    except Exception:
        pass
    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            ips.append(ip)
    except Exception:
        pass
    out = []
    for ip in ips:
        if ip and not ip.startswith("127.") and ip not in out:
            out.append(ip)
    return out or ["127.0.0.1"]


def print_qr(url):
    try:
        import qrcode
    except Exception:
        return False
    try:
        qr = qrcode.QRCode(border=1)
        qr.add_data(url)
        qr.make(fit=True)
        qr.print_ascii(invert=True)
        return True
    except Exception:
        return False


TUNNEL_RE = re.compile(r"https://[a-z0-9][a-z0-9-]*\.trycloudflare\.com")


def find_cloudflared(explicit=None):
    """Locate the cloudflared binary: an explicit --cloudflared path, then PATH,
    then next to the script or in the current folder (so you can just drop the
    downloaded binary beside marauder_remote.py -- no PATH/install needed)."""
    import os
    if explicit:
        return explicit if os.path.isfile(explicit) else shutil.which(explicit)
    w = shutil.which("cloudflared")
    if w:
        return w
    here = os.path.dirname(os.path.abspath(__file__))
    for base in (os.getcwd(), here):
        for name in ("cloudflared", "cloudflared.exe"):
            p = os.path.join(base, name)
            if os.path.isfile(p):
                return p
    return None


def start_tunnel(port, timeout=30, exe=None):
    """Open a free Cloudflare quick tunnel so the phone can reach the server from
    anywhere (cellular / any network). Returns (public_url, process, error)."""
    exe = find_cloudflared(exe)
    if not exe:
        return None, None, "cloudflared not found"
    try:
        os.chmod(exe, 0o755)  # in case the downloaded binary isn't executable yet
    except Exception:
        pass
    try:
        proc = subprocess.Popen(
            [exe, "tunnel", "--url", f"http://localhost:{port}", "--no-autoupdate"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    except Exception as e:
        return None, None, f"could not launch cloudflared: {e}"
    found = {"url": None}

    def reader():
        for line in proc.stdout:
            m = TUNNEL_RE.search(line)
            if m and not found["url"]:
                found["url"] = m.group(0)
    threading.Thread(target=reader, daemon=True).start()
    deadline = time.time() + timeout
    while time.time() < deadline and found["url"] is None and proc.poll() is None:
        time.sleep(0.2)
    if found["url"]:
        return found["url"], proc, None
    return None, proc, "tunnel URL not seen (cloudflared may have failed to connect)"


def main():
    ap = argparse.ArgumentParser(description="Drive MarauderBox from your phone over WiFi.")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--token", default="", help="shared secret required on the URL")
    ap.add_argument("--tunnel", action="store_true",
                    help="expose a public https link (via cloudflared) reachable from "
                         "anywhere -- phone does NOT need the same WiFi")
    ap.add_argument("--cloudflared", default="",
                    help="path to the cloudflared binary (otherwise found on PATH or "
                         "next to this script)")
    ap.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    args = ap.parse_args()

    if args.list_ports:
        for p in list_ports.comports():
            print(f"  {p.device:20} {p.description}")
        return

    port = args.port or autodetect_port()
    if not port:
        sys.exit("No serial port found. Plug in the Marauder, or pass --port (see --list-ports).")

    try:
        bridge = SerialBridge(port, args.baud)
    except Exception as e:
        sys.exit(f"Could not open {port}: {e}")

    # A public tunnel is internet-reachable, so always require a token there.
    if args.tunnel and not args.token:
        args.token = secrets.token_urlsafe(6)

    httpd = ThreadingHTTPServer((args.host, args.http_port), make_handler(bridge, args.token))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    suffix = f"?t={args.token}" if args.token else ""
    tunnel_proc = None
    print("=" * 64)
    print("  MarauderBox Remote ready")
    print(f"  Serial : {port} @ {args.baud}    HTTP port: {args.http_port}")

    if args.tunnel:
        print("  Opening a public tunnel (cloudflared)...")
        url, tunnel_proc, err = start_tunnel(args.http_port, exe=args.cloudflared or None)
        if url:
            public = url + "/" + suffix
            print("")
            print(f"  OPEN THIS ON YOUR PHONE (any network, even cellular):")
            print(f"      {public}")
            print("")
            if print_qr(public):
                print("  ^ or scan the QR above.")
            else:
                print("  (tip: `pip install qrcode` to print a scannable QR here)")
            print("  This link controls your Marauder from the internet. Keep it private;")
            print("  it stops working the moment you quit this script (Ctrl-C).")
        else:
            print(f"  Tunnel failed: {err}")
            if "not found" in (err or ""):
                print("  cloudflared isn't here. No Homebrew needed -- download it next to")
                print("  this script (one time), then re-run with --tunnel:")
                print('    A=$([ "$(uname -m)" = arm64 ] && echo arm64 || echo amd64)')
                print("    curl -L -o cloudflared.tgz \\")
                print("      https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-$A.tgz")
                print("    tar -xzf cloudflared.tgz && chmod +x cloudflared && rm cloudflared.tgz")
            print("  Falling back to local-network URLs below.")
            args.tunnel = False  # show LAN info instead

    if not args.tunnel:
        ips = lan_ips()
        primary = f"http://{ips[0]}:{args.http_port}/{suffix}"
        print("  Open ONE of these on your phone (same WiFi as this computer):")
        for ip in ips:
            print(f"      http://{ip}:{args.http_port}/{suffix}")
        print("")
        if print_qr(primary):
            print("  ^ or scan the QR above with your phone camera.")
        else:
            print("  (tip: `pip install qrcode` to print a scannable QR here)")
        print("  Can't connect? same WiFi (not guest/cellular); allow Python through the")
        print("  firewall; disable router AP/client isolation. On a locked-down network")
        print("  (phone + laptop on different subnets), use --tunnel instead.")
        if not args.token:
            print("   (no token: anyone on this network can control it -- use --token to lock)")
    print("=" * 64)
    try:
        while True:
            time.sleep(1)
            if tunnel_proc and tunnel_proc.poll() is not None:
                print("cloudflared exited; tunnel is down. Restart the script.")
                break
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        if tunnel_proc and tunnel_proc.poll() is None:
            tunnel_proc.terminate()
        httpd.shutdown()


if __name__ == "__main__":
    main()
