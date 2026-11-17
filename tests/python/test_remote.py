#!/usr/bin/env python3
"""Native tests for tools/Remote.py parsing + helpers.

Remote.py is the phone/web serial bridge. These cover its pure logic -- the
`list -a` / `ls /` output parsers, the cloudflared tunnel-URL regex, and serial
port autodetection -- with no hardware and no real serial port.

Remote.py imports pyserial at module load, so we stub `serial` into sys.modules
first; the test then runs anywhere, with or without pyserial installed, and can
never open a port.
"""
import os
import sys
import types

# --- stub pyserial BEFORE importing Remote -----------------------------------
_serial = types.ModuleType("serial")
_serial.Serial = type("Serial", (), {})
_tools = types.ModuleType("serial.tools")
_list_ports = types.ModuleType("serial.tools.list_ports")
_list_ports.comports = lambda: []
_tools.list_ports = _list_ports
_serial.tools = _tools
sys.modules.setdefault("serial", _serial)
sys.modules.setdefault("serial.tools", _tools)
sys.modules.setdefault("serial.tools.list_ports", _list_ports)

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.normpath(os.path.join(_HERE, "..", "..", "tools"))
sys.path.insert(0, _TOOLS)

import Remote  # noqa: E402


def test_parse_ap_list():
    text = "\n".join([
        "[0][CH:6] MyWiFi -42",
        "[3][CH:11] Home Net -55 (selected)",   # multi-word SSID + selected flag
        "[4][CH:5] Cafe 5G",                     # trailing token not numeric -> rssi 0
        "[7][CH:1] (hidden) -80",
        "[8][CH:36] 12345 -40",                  # all-numeric SSID with rssi
        "random noise line",                     # not an AP row -> ignored
        "[9][CH:2] NoRssi",                      # single token -> rssi 0
    ])
    aps = Remote.parse_ap_list(text)
    assert len(aps) == 6, aps
    assert aps[0] == {"i": 0, "ch": 6, "ssid": "MyWiFi", "rssi": -42, "sel": False}
    assert aps[1] == {"i": 3, "ch": 11, "ssid": "Home Net", "rssi": -55, "sel": True}
    assert aps[2] == {"i": 4, "ch": 5, "ssid": "Cafe 5G", "rssi": 0, "sel": False}
    assert aps[3] == {"i": 7, "ch": 1, "ssid": "(hidden)", "rssi": -80, "sel": False}
    assert aps[4] == {"i": 8, "ch": 36, "ssid": "12345", "rssi": -40, "sel": False}
    assert aps[5] == {"i": 9, "ch": 2, "ssid": "NoRssi", "rssi": 0, "sel": False}


def test_parse_ap_list_empty():
    assert Remote.parse_ap_list("") == []
    assert Remote.parse_ap_list("no brackets here\njust text") == []


def test_parse_html_list():
    text = "login.html  portal.HTML  /sd/dir/login.html  notes.txt  fake.htmlx"
    out = Remote.parse_html_list(text)
    # directory stripped, case-insensitive dedupe keeps the first spelling, and
    # "fake.htmlx" is rejected (no word boundary after .html).
    assert out == ["login.html", "portal.HTML"], out


def test_parse_html_list_none():
    assert Remote.parse_html_list("nothing .htm or bare html here") == []


def test_tunnel_regex():
    line = "2026-01-01 INF |  https://brave-fox-42.trycloudflare.com  | your url"
    m = Remote.TUNNEL_RE.search(line)
    assert m and m.group(0) == "https://brave-fox-42.trycloudflare.com"
    assert Remote.TUNNEL_RE.search("https://example.com/not-a-tunnel") is None


def test_autodetect_port():
    def ports(devs):
        return [types.SimpleNamespace(device=d, description=desc, manufacturer=mfr)
                for (d, desc, mfr) in devs]
    # A recognized USB-serial hint wins.
    Remote.list_ports.comports = lambda: ports([("/dev/ttyUSB0", "USB Serial", "")])
    assert Remote.autodetect_port() == "/dev/ttyUSB0"
    # Hint match beats position: the CP210x port is chosen over the first entry.
    Remote.list_ports.comports = lambda: ports([
        ("/dev/ttyS0", "generic", ""),
        ("COM5", "Silicon Labs CP210x UART Bridge", "")])
    assert Remote.autodetect_port() == "COM5"
    # No hint anywhere -> fall back to the first port.
    Remote.list_ports.comports = lambda: ports([("/dev/ttyS9", "mystery", "")])
    assert Remote.autodetect_port() == "/dev/ttyS9"
    # No ports at all -> None.
    Remote.list_ports.comports = lambda: []
    assert Remote.autodetect_port() is None


def test_prefer_callout_linux():
    # On Linux (this runner) the macOS /dev/tty.* -> /dev/cu.* rewrite is a no-op.
    assert Remote._prefer_callout("/dev/ttyUSB0") == "/dev/ttyUSB0"
    assert Remote._prefer_callout("/dev/cu.usbserial-1") == "/dev/cu.usbserial-1"
    assert Remote._prefer_callout(None) is None


def main():
    tests = [(k, v) for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    for name, fn in tests:
        fn()
    print(f"Remote.py: {len(tests)} tests passed "
          "(AP list parse, HTML list parse, tunnel regex, port autodetect)")


if __name__ == "__main__":
    main()
