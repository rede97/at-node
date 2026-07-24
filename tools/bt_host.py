#!/usr/bin/env python3
"""bt_host.py — Linux Bluetooth host-side test tool for AT-Node keyboards.

Solidifies the ad-hoc bluetoothctl/evdev/btmon workflow (2026-07-24):
the CSR dongle (hci1) pairs the Linux box as a BLE host of the kbd /
kbd_multi board; key reports are asserted on the evdev node.

Subcommands:
    scan [sec]                 discover AT-Node devices (default 8s)
    pair [--mac MAC]           register Just-Works agent + pair + trust
    status                     connection + evdev node of the AT-Node
    listen [sec] [--key N]     listen evdev; PASS if key code N seen down+up
                               (default key 59 = F1 = board SW1 button)
    sniff [sec]                btmon capture; count ATT notifications
                               (decides "board didn't send" vs "host didn't deliver")

Defaults: MAC auto-discovered by scanning for name "AT-Node" (exact).
Env: ATNODE_MAC overrides discovery, ATNODE_HCI overrides hci1.

Requires: mxq in groups bluetooth + input (usermod -aG bluetooth,input mxq).
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time

HCI = os.environ.get("ATNODE_HCI", "hci1")
DEFAULT_KEY = 59  # KEY_F1 — kbd board SW1 sends HID 0x3A (F1)


def sh(cmd, timeout=20):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          timeout=timeout).stdout


def find_mac(name="AT-Node", secs=8):
    """Scan for an exact-named device, return MAC or None."""
    env_mac = os.environ.get("ATNODE_MAC")
    if env_mac:
        return env_mac.upper()
    out = sh(f"timeout {secs} bluetoothctl --timeout {secs - 1} scan le", secs + 5)
    for line in out.splitlines():
        m = re.search(r"Device ([0-9A-F:]{17}) " + re.escape(name) + r"\s*$",
                      line.strip())
        if m:
            return m.group(1)
    # already known to bluez?
    for line in sh("bluetoothctl devices").splitlines():
        m = re.match(r"Device ([0-9A-F:]{17}) " + re.escape(name) + r"\s*$",
                     line.strip())
        if m:
            return m.group(1)
    return None


def connected(mac):
    return "Connected: yes" in sh(f"bluetoothctl info {mac}")


def ensure_connected(mac, tries=2):
    for _ in range(tries):
        if connected(mac):
            return True
        sh(f"timeout 15 bluetoothctl connect {mac}", 20)
        time.sleep(1)
    return connected(mac)


def find_evdev(name="AT-Node"):
    """evdev node path of the named input device, or None."""
    try:
        txt = open("/proc/bus/input/devices").read()
    except OSError:
        return None
    m = re.search(r'Name="' + re.escape(name) +
                  r'".*?Handlers=.*?\b(event\d+)', txt, re.S)
    return "/dev/input/" + m.group(1) if m else None


def cmd_scan(args):
    mac = find_mac(secs=args.secs)
    if mac:
        print(f"AT-Node found: {mac}")
        return 0
    print("AT-Node not found")
    return 1


def cmd_pair(args):
    mac = args.mac or find_mac()
    if not mac:
        print("AT-Node not found — scan first"); return 1
    # agent + pair via our dbus-next agent (bluetoothctl agent is broken
    # on this VM)
    r = subprocess.run(["uv", "run", "python", "tools/bt_agent.py",
                        "--pair", mac],
                       capture_output=True, text=True, timeout=30)
    print(r.stdout.strip())
    if r.returncode != 0:
        print(r.stderr.strip()); return 1
    sh(f"bluetoothctl trust {mac}")
    ok = ensure_connected(mac)
    node = find_evdev()
    print(f"paired={ok} evdev={node}")
    return 0 if ok else 1


def cmd_status(args):
    mac = find_mac(secs=3)
    if not mac:
        print("AT-Node unknown"); return 1
    info = sh(f"bluetoothctl info {mac}")
    for k in ("Paired", "Trusted", "Connected"):
        m = re.search(k + r": (\w+)", info)
        print(f"{k.lower()}: {m.group(1) if m else '?'}", end="  ")
    print(f"\nmac: {mac}  evdev: {find_evdev()}")
    return 0


def listen_events(node, secs):
    """Yield (code, value) EV_KEY events for secs seconds."""
    fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
    end = time.time() + secs
    try:
        while time.time() < end:
            try:
                data = os.read(fd, 24)
                if len(data) == 24:
                    _, _, et, code, val = struct.unpack("llHHi", data)
                    if et == 1:
                        yield code, val
            except BlockingIOError:
                time.sleep(0.05)
    finally:
        os.close(fd)


def cmd_listen(args):
    if not ensure_connected(find_mac(secs=3) or ""):
        print("FAIL: not connected"); return 1
    node = find_evdev()
    if not node:
        print("FAIL: no evdev node (device not paired as HID?)"); return 1
    print(f"listening on {node} for {args.secs}s — press the key now")
    got = list(listen_events(node, args.secs))
    for code, val in got:
        print(f"  EV_KEY code={code} value={val}")
    vals = [v for c, v in got if c == args.key]
    if 1 in vals and 0 in vals:
        print(f"PASS: key {args.key} down+up received")
        return 0
    print(f"FAIL: key {args.key} not received (events={got})")
    return 1


def cmd_sniff(args):
    log = f"/tmp/bt_sniff_{os.getpid()}.log"
    mon = subprocess.Popen(["btmon", "-w", log],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"btmon capturing {args.secs}s — press the key now")
    time.sleep(args.secs)
    mon.terminate()
    try:
        mon.wait(timeout=3)
    except subprocess.TimeoutExpired:
        mon.kill()
    time.sleep(0.5)
    out = sh(f"btmon -r {log} 2>/dev/null | grep -c 'Handle Value Notification'")
    os.unlink(log)
    n = int(out.strip() or 0)
    print(f"ATT notifications seen: {n}")
    print("board SENT reports (host-side delivery issue)" if n
          else "board did NOT send (kbd firmware/report path issue)")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan"); s.add_argument("secs", nargs="?", type=int, default=8)
    s = sub.add_parser("pair"); s.add_argument("--mac")
    sub.add_parser("status")
    s = sub.add_parser("listen")
    s.add_argument("secs", nargs="?", type=int, default=40)
    s.add_argument("--key", type=int, default=DEFAULT_KEY)
    s = sub.add_parser("sniff"); s.add_argument("secs", nargs="?", type=int, default=15)
    args = p.parse_args()
    return {"scan": cmd_scan, "pair": cmd_pair, "status": cmd_status,
            "listen": cmd_listen, "sniff": cmd_sniff}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
