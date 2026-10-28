#!/usr/bin/env python3
"""
marauder_remote.py -- drive your MarauderBox from your phone over WiFi.

How it works:
  [ Marauder ] --USB serial--> [ this computer runs this script ] <--WiFi--> [ phone browser ]

Plug the Marauder into a computer with USB, run this script, then open the URL
it prints on your phone's browser (same WiFi network). You get big command
buttons, a free-text command box, and a live serial console.

Requires: Python 3.8+ and pyserial  ->  pip install pyserial

Examples:
  python3 marauder_remote.py                 # auto-detect port, serve on :8080
  python3 marauder_remote.py --port COM5
  python3 marauder_remote.py --port /dev/ttyUSB0 --baud 115200 --http-port 8080
  python3 marauder_remote.py --token mysecret # require ?t=mysecret (simple LAN guard)

Authorized use only: this is a remote control for a tool you own.
"""
import argparse
import html
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


# --- command palette (real MarauderBox CLI commands) --------------------------

PALETTE = [
    ("Scan / Sniff", [
        ("Scan APs+STAs", "scanall"),
        ("Sniff Beacons", "sniffbeacon"),
        ("Sniff Probes", "sniffprobe"),
        ("Sniff Deauth", "sniffdeauth"),
        ("Sniff PMKID", "sniffpmkid"),
        ("Sniff Pwnagotchi", "sniffpwn"),
        ("Wardrive", "wardrive"),
    ]),
    ("Attack (select APs first)", [
        ("Deauth", "attack -t deauth"),
        ("Beacon: Rickroll", "attack -t beacon -r"),
        ("Beacon: List", "attack -t beacon -l"),
        ("Probe flood", "attack -t probe"),
    ]),
    ("Bluetooth", [
        ("BLE Sniff", "sniffbt"),
        ("BLE Spam (all)", "blespam -t all"),
        ("Find My / AirTag", "findmy"),
        ("Skimmer Sniff", "sniffskim"),
    ]),
    ("Lists", [
        ("List APs", "list -a"),
        ("List Stations", "list -s"),
        ("List SSIDs", "list -c"),
        ("Select ALL APs", "select -a all"),
        ("Clear AP list", "clearlist -a"),
    ]),
    ("System", [
        ("Channel 1", "channel -s 1"),
        ("Channel 6", "channel -s 6"),
        ("Channel 11", "channel -s 11"),
        ("GPS Data", "gpsdata"),
        ("Help", "help"),
        ("Reboot", "reboot"),
    ]),
]

PAGE = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>MarauderBox Remote</title>
<style>
  :root { --bg:#0b0f14; --panel:#131a22; --line:#243041; --txt:#e6edf3;
          --cy:#35d0e0; --grn:#3ddc84; --red:#ff5c5c; --amber:#ffb454; --mut:#8aa0b4; }
  * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
  body { margin:0; background:var(--bg); color:var(--txt);
         font:15px/1.4 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif; }
  header { position:sticky; top:0; z-index:5; background:var(--panel);
           border-bottom:1px solid var(--line); padding:10px 14px; display:flex;
           align-items:center; gap:10px; }
  header b { font-size:16px; letter-spacing:.3px; }
  #dot { width:10px; height:10px; border-radius:50%; background:var(--red); }
  #dot.ok { background:var(--grn); }
  .wrap { padding:12px; max-width:720px; margin:0 auto; }
  .grp { margin-bottom:14px; }
  .grp h3 { margin:0 0 7px; font-size:12px; text-transform:uppercase;
            letter-spacing:.08em; color:var(--mut); }
  .grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(150px,1fr)); gap:8px; }
  button { font:inherit; color:var(--txt); background:#1b2430; border:1px solid var(--line);
           border-radius:10px; padding:13px 10px; cursor:pointer; min-height:48px; }
  button:active { background:#243244; }
  button.stop { grid-column:1/-1; background:#3a1414; border-color:#6b2020; color:var(--red);
                font-weight:700; font-size:16px; }
  .cmdbar { display:flex; gap:8px; margin:10px 0 14px; }
  .cmdbar input { flex:1; min-width:0; font:inherit; color:var(--txt); background:#0e141b;
                  border:1px solid var(--line); border-radius:10px; padding:12px; }
  .cmdbar button { background:var(--cy); color:#04222a; border:none; font-weight:700; padding:0 16px; }
  #log { background:#06090d; border:1px solid var(--line); border-radius:10px; padding:10px;
         height:42vh; overflow:auto; white-space:pre-wrap; word-break:break-word;
         font:12px/1.35 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; color:#cfe3f2; }
  .bar2 { display:flex; gap:8px; align-items:center; margin-top:8px; color:var(--mut); font-size:12px; }
  .bar2 button { padding:7px 10px; min-height:0; font-size:12px; }
</style></head>
<body>
<header><span id="dot"></span><b>MarauderBox Remote</b>
  <span id="port" style="color:var(--mut);font-size:12px;margin-left:auto"></span></header>
<div class="wrap">
  <div class="cmdbar">
    <input id="cmd" placeholder="type any command, e.g. select -a 0" autocomplete="off"
           autocapitalize="off" spellcheck="false">
    <button onclick="sendInput()">Send</button>
  </div>
  <div id="pal"></div>
  <pre id="log"></pre>
  <div class="bar2">
    <button onclick="document.getElementById('log').textContent=''">Clear console</button>
    <label><input type="checkbox" id="auto" checked> autoscroll</label>
    <span id="stat"></span>
  </div>
</div>
<script>
  const TOKEN = __TOKEN__;
  const PALETTE = __PALETTE__;
  const q = s => document.querySelector(s);
  const tok = () => TOKEN ? "&t=" + encodeURIComponent(TOKEN) : "";

  function send(c){
    fetch("/cmd?c=" + encodeURIComponent(c) + tok())
      .then(r => { if(!r.ok) throw 0; flash(); })
      .catch(() => setStat("send failed"));
  }
  function sendInput(){ const i=q("#cmd"); if(i.value.trim()){ send(i.value.trim()); i.value=""; } }
  q("#cmd").addEventListener("keydown", e => { if(e.key==="Enter") sendInput(); });
  function flash(){ const d=q("#dot"); d.style.boxShadow="0 0 8px var(--cy)"; setTimeout(()=>d.style.boxShadow="",150); }
  function setStat(s){ q("#stat").textContent=s; }

  // build palette
  const pal=q("#pal");
  pal.appendChild(mk("button","STOP  (stopscan)","stop",()=>send("stopscan")));
  for(const [title,cmds] of PALETTE){
    const g=document.createElement("div"); g.className="grp";
    const h=document.createElement("h3"); h.textContent=title; g.appendChild(h);
    const grid=document.createElement("div"); grid.className="grid";
    for(const [label,cmd] of cmds) grid.appendChild(mk("button",label,"",()=>send(cmd)));
    g.appendChild(grid); pal.appendChild(g);
  }
  function mk(tag,text,cls,fn){ const b=document.createElement(tag); b.textContent=text;
    if(cls)b.className=cls; b.onclick=fn; return b; }

  // poll serial output
  let pos=0, first=true;
  function poll(){
    fetch("/out?pos=" + pos + tok()).then(r=>r.json()).then(j=>{
      q("#dot").classList.add("ok"); setStat("");
      if(j.port) q("#port").textContent=j.port;
      if(first){ pos=j.pos; first=false; }          // skip backlog on first load
      else if(j.data){ const log=q("#log"); log.textContent+=j.data; pos=j.pos;
        if(q("#auto").checked) log.scrollTop=log.scrollHeight; }
    }).catch(()=>{ q("#dot").classList.remove("ok"); setStat("disconnected"); })
      .finally(()=>setTimeout(poll,700));
  }
  poll();
</script>
</body></html>"""


# --- HTTP server --------------------------------------------------------------

def make_handler(bridge, token):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass  # quiet

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
                        .replace("__PALETTE__", json.dumps(PALETTE)))
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
                    json.dumps({"pos": end, "data": data, "port": f"{bridge.port} @ {bridge.baud}"}),
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
    print(f"  MarauderBox Remote ready")
    print(f"  Serial : {port} @ {args.baud}")
    print(f"  Open on your phone (same WiFi):  {url}")
    if not args.token:
        print(f"  (anyone on this network can control it -- add --token to lock down)")
    print("=" * 60)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
