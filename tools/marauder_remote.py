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
import socket
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
            if pos < self._base:
                pos = self._base
            start = pos - self._base
            chunk = bytes(self._buf[start:])
        return end, chunk.decode("latin-1")


# --- Marauder-style menu tree -------------------------------------------------
# Node keys:  t=title  c=color  m=submenu(list)  x=command  p=prompt(arg -> {})
# Colors map to CSS classes below. Commands are the real MarauderBox CLI.

MENU = [
  {"t": "WiFi", "c": "cy", "m": [
    {"t": "Sniffers", "c": "yel", "m": [
      {"t": "Scan APs + Stations", "c": "grn", "x": "scanall"},
      {"t": "Sniff Beacons", "c": "cy", "x": "sniffbeacon"},
      {"t": "Sniff Probes", "c": "cy", "x": "sniffprobe"},
      {"t": "Sniff Deauth", "c": "red", "x": "sniffdeauth"},
      {"t": "Sniff PMKID", "c": "grn", "x": "sniffpmkid"},
      {"t": "Sniff PMKID + Deauth", "c": "grn", "x": "sniffpmkid -d"},
      {"t": "Sniff Pwnagotchi", "c": "mag", "x": "sniffpwn"},
      {"t": "Sniff Pineapple", "c": "org", "x": "sniffpinescan"},
      {"t": "Sniff MultiSSID", "c": "cy", "x": "sniffmultissid"},
      {"t": "Sniff SAE", "c": "cy", "x": "sniffsae"},
      {"t": "Raw Capture", "c": "wht", "x": "sniffraw"},
      {"t": "Packet Count", "c": "grn", "x": "packetcount"},
      {"t": "Wardrive", "c": "cy", "x": "wardrive"},
      {"t": "Wardrive POI tag", "c": "org", "x": "wardrivepoi"},
      {"t": "MAC Tracker", "c": "mag", "x": "mactrack"},
    ]},
    {"t": "Attacks", "c": "red", "m": [
      {"t": "Deauth", "c": "red", "x": "attack -t deauth"},
      {"t": "Deauth (targeted)", "c": "red", "x": "attack -t deauth -c"},
      {"t": "Beacon: Rickroll", "c": "mag", "x": "attack -t beacon -r"},
      {"t": "Beacon: SSID List", "c": "mag", "x": "attack -t beacon -l"},
      {"t": "Beacon: Random", "c": "mag", "x": "attack -t beacon -a"},
      {"t": "Probe Flood", "c": "org", "x": "attack -t probe"},
      {"t": "Rick Roll", "c": "mag", "x": "attack -t rickroll"},
      {"t": "Bad Msg", "c": "red", "x": "attack -t badmsg"},
      {"t": "Bad Msg (targeted)", "c": "red", "x": "attack -t badmsg -c"},
      {"t": "Sleep / Assoc", "c": "org", "x": "attack -t sleep"},
      {"t": "SAE Flood", "c": "red", "x": "attack -t sae"},
      {"t": "CSA", "c": "red", "x": "attack -t csa"},
      {"t": "Quiet", "c": "red", "x": "attack -t quiet"},
      {"t": "Karma (index)", "c": "red", "x": "karma -p {}", "p": "SSID index"},
      {"t": "Evil Portal (start)", "c": "org", "x": "evilportal -c start"},
    ]},
    {"t": "Target List", "c": "blu", "m": [
      {"t": "List APs", "c": "cy", "x": "list -a"},
      {"t": "List Stations", "c": "cy", "x": "list -s"},
      {"t": "List SSIDs", "c": "cy", "x": "list -c"},
      {"t": "Info on AP (index)", "c": "cy", "x": "info -a {}", "p": "AP index"},
      {"t": "Select AP (index)", "c": "grn", "x": "select -a {}", "p": "AP index"},
      {"t": "Select ALL APs", "c": "grn", "x": "select -a all"},
      {"t": "Select Station (index)", "c": "grn", "x": "select -s {}", "p": "Station index"},
      {"t": "Select ALL Stations", "c": "grn", "x": "select -s all"},
      {"t": "Select SSID (index)", "c": "grn", "x": "select -c {}", "p": "SSID index"},
      {"t": "Select ALL SSIDs", "c": "grn", "x": "select -c all"},
      {"t": "Clear APs", "c": "red", "x": "clearlist -a"},
      {"t": "Clear Stations", "c": "red", "x": "clearlist -s"},
      {"t": "Clear SSIDs", "c": "red", "x": "clearlist -c"},
      {"t": "Save APs", "c": "blu", "x": "save -a"},
      {"t": "Save SSIDs", "c": "blu", "x": "save -s"},
      {"t": "Load APs", "c": "blu", "x": "load -a"},
      {"t": "Load SSIDs", "c": "blu", "x": "load -s"},
    ]},
    {"t": "SSID List", "c": "blu", "m": [
      {"t": "Add random SSIDs (count)", "c": "grn", "x": "ssid -a -g {}", "p": "how many"},
      {"t": "Add named SSID", "c": "grn", "x": "ssid -a -n {}", "p": "SSID name"},
      {"t": "Remove SSID (index)", "c": "red", "x": "ssid -r {}", "p": "SSID index"},
    ]},
    {"t": "Recon (passive)", "c": "grn", "m": [
      {"t": "Recon WiFi", "c": "cy", "x": "recon wifi"},
      {"t": "Recon BLE", "c": "cy", "x": "recon ble"},
      {"t": "Recon Status", "c": "grn", "x": "recon status"},
      {"t": "Recon Stop", "c": "red", "x": "recon stop"},
    ]},
    {"t": "Network (join first)", "c": "blu", "m": [
      {"t": "Join AP (index)", "c": "grn", "x": "join -a {}", "p": "AP index"},
      {"t": "Join saved network", "c": "grn", "x": "join -s"},
      {"t": "Ping Scan", "c": "cy", "x": "pingscan"},
      {"t": "ARP Scan", "c": "cy", "x": "arpscan"},
      {"t": "Port Scan (IP index)", "c": "cy", "x": "portscan -a -t {}", "p": "IP index"},
      {"t": "Scan HTTP", "c": "cy", "x": "portscan -s http"},
      {"t": "Scan HTTPS", "c": "cy", "x": "portscan -s https"},
      {"t": "Scan SSH", "c": "cy", "x": "portscan -s ssh"},
      {"t": "Scan Telnet", "c": "cy", "x": "portscan -s telnet"},
      {"t": "Scan DNS", "c": "cy", "x": "portscan -s dns"},
      {"t": "Scan SMTP", "c": "cy", "x": "portscan -s smtp"},
      {"t": "Scan RDP", "c": "cy", "x": "portscan -s rdp"},
      {"t": "Upload Wardrive -> WiGLE", "c": "grn", "x": "upload -d wigle"},
    ]},
    {"t": "CSI Motion", "c": "mag", "m": [
      {"t": "Start (AP index)", "c": "grn", "x": "csimotion start {}", "p": "AP index"},
      {"t": "Recalibrate", "c": "cy", "x": "csimotion recalibrate"},
      {"t": "Status", "c": "cy", "x": "csimotion status"},
    ]},
  ]},
  {"t": "Bluetooth", "c": "blu", "m": [
    {"t": "Sniffers", "c": "yel", "m": [
      {"t": "BLE Scan (all)", "c": "cy", "x": "sniffbt"},
      {"t": "Sniff AirTags", "c": "cy", "x": "sniffbt -t airtag"},
      {"t": "Sniff iBeacons", "c": "cy", "x": "sniffbt -t ibeacon"},
      {"t": "Sniff Flipper", "c": "org", "x": "sniffbt -t flipper"},
      {"t": "Sniff Flock (ALPR)", "c": "red", "x": "sniffbt -t flock"},
      {"t": "Sniff Meta / Rayban", "c": "mag", "x": "sniffbt -t meta"},
      {"t": "Sniff Skimmers", "c": "red", "x": "sniffskim"},
      {"t": "Find My (index)", "c": "cy", "x": "findmy -t {}", "p": "device index"},
    ]},
    {"t": "Attacks", "c": "red", "m": [
      {"t": "BLE Spam: Sour Apple", "c": "red", "x": "blespam -t sourapple"},
      {"t": "BLE Spam: Apple Juice", "c": "red", "x": "blespam -t applejuice"},
      {"t": "BLE Spam: Google", "c": "red", "x": "blespam -t google"},
      {"t": "BLE Spam: Samsung", "c": "red", "x": "blespam -t samsung"},
      {"t": "BLE Spam: Windows", "c": "red", "x": "blespam -t windows"},
      {"t": "BLE Spam: Flipper", "c": "red", "x": "blespam -t flipper"},
      {"t": "BLE Spam: ALL", "c": "red", "x": "blespam -t all"},
      {"t": "Spoof AirTag (index)", "c": "org", "x": "spoofat -t {}", "p": "AirTag index"},
    ]},
  ]},
  {"t": "Remote ID (Drone)", "c": "org", "m": [
    {"t": "Scan", "c": "grn", "x": "remoteid scan"},
    {"t": "List", "c": "cy", "x": "remoteid list"},
    {"t": "Track (index)", "c": "mag", "x": "remoteid track {}", "p": "drone index"},
    {"t": "Clear", "c": "red", "x": "remoteid clear"},
  ]},
  {"t": "GPS", "c": "red", "m": [
    {"t": "GPS Data", "c": "cy", "x": "gpsdata"},
    {"t": "NMEA Stream", "c": "cy", "x": "nmea"},
    {"t": "Fix Status", "c": "cy", "x": "gps -g fix"},
    {"t": "Satellites", "c": "cy", "x": "gps -g sat"},
    {"t": "Latitude", "c": "cy", "x": "gps -g lat"},
    {"t": "Longitude", "c": "cy", "x": "gps -g lon"},
    {"t": "Altitude", "c": "cy", "x": "gps -g alt"},
    {"t": "POI: Start log", "c": "grn", "x": "gpspoi -s"},
    {"t": "POI: Mark point", "c": "grn", "x": "gpspoi -m"},
    {"t": "POI: End log", "c": "red", "x": "gpspoi -e"},
    {"t": "Tracker: Start", "c": "grn", "x": "gpstracker -c start"},
    {"t": "Tracker: Stop", "c": "red", "x": "gpstracker -c stop"},
    {"t": "Geofence: List", "c": "cy", "x": "geofence list"},
  ]},
  {"t": "Device", "c": "blu", "m": [
    {"t": "Channel: 1", "c": "cy", "x": "channel -s 1"},
    {"t": "Channel: 6", "c": "cy", "x": "channel -s 6"},
    {"t": "Channel: 11", "c": "cy", "x": "channel -s 11"},
    {"t": "Channel: set...", "c": "cy", "x": "channel -s {}", "p": "channel #"},
    {"t": "Channel: show", "c": "cy", "x": "channel"},
    {"t": "Brightness: Max", "c": "yel", "x": "brightness -s 9"},
    {"t": "Brightness: Mid", "c": "yel", "x": "brightness -s 5"},
    {"t": "Brightness: Low", "c": "yel", "x": "brightness -s 1"},
    {"t": "Brightness: Cycle", "c": "yel", "x": "brightness -c cycle"},
    {"t": "LED color (hex)", "c": "mag", "x": "led -s {}", "p": "hex e.g. FF0000"},
    {"t": "LED rainbow", "c": "mag", "x": "led -p rainbow"},
    {"t": "Settings: Reset", "c": "red", "x": "settings -r"},
    {"t": "Settings: set...", "c": "cy", "x": "settings -s {}", "p": "e.g. ForcePMKID enable"},
    {"t": "List SD files", "c": "cy", "x": "ls /"},
    {"t": "Protocol Info", "c": "cy", "x": "protocolinfo"},
    {"t": "Backup SPIFFS -> SD", "c": "blu", "x": "backupspiffs"},
    {"t": "Backup Status", "c": "blu", "x": "backupstatus"},
    {"t": "Restore SPIFFS", "c": "org", "x": "restorespiffs"},
    {"t": "Update (SD)", "c": "org", "x": "update -s"},
    {"t": "Help (full list)", "c": "grn", "x": "help"},
    {"t": "Reboot", "c": "red", "x": "reboot"},
  ]},
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
  .row{display:flex;align-items:center;gap:10px;padding:12px 12px;border-radius:9px;
    background:#0d1620;border:1px solid #172334;border-left-width:5px;cursor:pointer;
    color:#dfe8f2;font-size:15px;min-height:46px;}
  .row:active{background:#15202e;}
  .row .chev{margin-left:auto;color:#7f93a8;font-size:18px;}
  .row.back{background:#0a1118;border-left-color:#7f93a8;color:#9fb4c8;font-weight:600;}
  .row.c-cy{border-left-color:#2fd3e6;} .row.c-red{border-left-color:#ff5c5c;color:#ffd7d7;}
  .row.c-grn{border-left-color:#45e08a;} .row.c-mag{border-left-color:#d07bff;}
  .row.c-yel{border-left-color:#ffd23f;} .row.c-org{border-left-color:#ffae45;}
  .row.c-blu{border-left-color:#5aa0ff;} .row.c-wht{border-left-color:#dfe8f2;}
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
  #log{background:#04070b;border:1px solid #223;border-radius:9px;padding:9px;height:26vh;
    overflow:auto;white-space:pre-wrap;word-break:break-word;color:#bfe0f0;
    font:12px/1.35 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;}
  .tools{display:flex;gap:8px;align-items:center;margin-top:7px;color:#7f93a8;font-size:12px;}
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
<div class="console">
  <div class="cmdbar">
    <input id="cmd" placeholder="type any command (e.g. select -a 0)" autocomplete="off"
      autocapitalize="off" spellcheck="false">
    <button onclick="sendInput()">Send</button>
  </div>
  <pre id="log"></pre>
  <div class="tools">
    <button onclick="document.getElementById('log').textContent=''">Clear</button>
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

  function send(c){
    fetch("/cmd?c=" + encodeURIComponent(c) + tok())
      .then(r => { if(!r.ok) throw 0; flash(); })
      .catch(() => setStat("send failed"));
  }
  function run(node){
    let c = node.x;
    if(c.indexOf("{}") >= 0){
      const v = prompt(node.p || "value", "");
      if(v === null || v === "") return;
      c = c.replace("{}", v.trim());
    }
    send(c);
  }
  function sendInput(){ const i=$("#cmd"); if(i.value.trim()){ send(i.value.trim()); i.value=""; } }
  $("#cmd").addEventListener("keydown", e => { if(e.key==="Enter") sendInput(); });
  function flash(){ const d=$("#dot"); d.style.boxShadow="0 0 9px #2fd3e6"; setTimeout(()=>d.style.boxShadow="",160); }
  function setStat(s){ $("#stat").textContent=s; }

  function render(){
    const cur = stack[stack.length-1];
    $("#title").textContent = cur.title;
    const list = $("#list"); list.innerHTML = "";
    if(stack.length > 1){
      const b = rowEl("‹ Back", "back", ""); b.onclick = () => { stack.pop(); render(); };
      list.appendChild(b);
    }
    for(const n of cur.nodes){
      const sub = !!n.m;
      const el = rowEl(n.t, "c-" + (n.c||"cy"), sub ? "›" : "");
      el.onclick = sub
        ? () => { stack.push({title:n.t, nodes:n.m}); render(); }
        : () => run(n);
      list.appendChild(el);
    }
    list.scrollTop = 0;
  }
  function rowEl(text, cls, chev){
    const d=document.createElement("div"); d.className="row "+cls;
    const s=document.createElement("span"); s.textContent=text; d.appendChild(s);
    if(chev){ const c=document.createElement("span"); c.className="chev"; c.textContent=chev; d.appendChild(c); }
    return d;
  }

  stack.push({title:"MarauderBox", nodes:MENU});
  render();

  // live serial console
  let pos=0, first=true;
  function poll(){
    fetch("/out?pos=" + pos + tok()).then(r=>r.json()).then(j=>{
      $("#dot").classList.add("ok");
      if(j.port) $("#port").textContent=j.port;
      if(first){ pos=j.pos; first=false; }
      else if(j.data){ const log=$("#log"); log.textContent+=j.data; pos=j.pos;
        if($("#auto").checked) log.scrollTop=log.scrollHeight; }
    }).catch(()=>{ $("#dot").classList.remove("ok"); setStat("disconnected"); })
      .finally(()=>setTimeout(poll,700));
  }
  poll();
</script>
</body></html>"""


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


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser(description="Drive MarauderBox from your phone over WiFi.")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    ap.add_argument("--http-port", type=int, default=8080)
    ap.add_argument("--token", default="", help="optional shared secret required on the URL")
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

    handler = make_handler(bridge, args.token)
    httpd = ThreadingHTTPServer((args.host, args.http_port), handler)

    ip = lan_ip()
    url = f"http://{ip}:{args.http_port}/"
    if args.token:
        url += f"?t={args.token}"
    print("=" * 60)
    print("  MarauderBox Remote ready")
    print(f"  Serial : {port} @ {args.baud}")
    print(f"  Open on your phone (same WiFi):  {url}")
    if not args.token:
        print("  (anyone on this network can control it -- add --token to lock down)")
    print("=" * 60)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
