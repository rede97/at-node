#!/usr/bin/env python3
r"""Dongle closed-loop test using ESP32-C3 as BLE keyboard peer.

  The C3 runs tools/esp32c3_kbd/esp32c3_kbd.ino (BLE HID keyboard + HTTP).
  The CH582 dongle is driven over its CDC port (AT commands).

  Flow:
    1. AT+BT_DISC on dongle for clean slate.
    2. AT+BT_SCAN to find C3-Kbd.
    3. AT+BT_CONN=<idx> to pair/connect.
    4. Wait for "armed".
    5. POST /tap to C3 for keys 'a' (0x04) and 'b' (0x05).
    6. Verify matching +BT_NTF bytes on dongle CDC.

  Exit 0 = pass, 1 = fail.
"""
import argparse
import re
import sys
import time

import requests
import serial

DEFAULT_DONGLE_PORT = "COM4"
DEFAULT_C3_HOST = "esp32kbd.local"


def cmd(ser, c, wait=0.6):
    ser.write(c + b"\r\n")
    time.sleep(wait)
    return ser.read(512).decode(errors="replace")


def listen(ser, dur):
    deadline = time.time() + dur
    buf = b""
    while time.time() < deadline:
        b = ser.read(1024)
        if b:
            buf += b
        else:
            time.sleep(0.05)
    return buf.decode(errors="replace")


def c3_status(host):
    r = requests.get(f"http://{host}/status", timeout=5)
    r.raise_for_status()
    return r.json()


def c3_tap(host, key, mods=0, ms=100):
    r = requests.post(
        f"http://{host}/tap",
        params={"k": key, "mods": mods, "ms": ms},
        timeout=10,
    )
    r.raise_for_status()


def main():
    parser = argparse.ArgumentParser(description="CH582 dongle + C3 keyboard loop test")
    parser.add_argument("--dongle-port", default=DEFAULT_DONGLE_PORT, help="dongle CDC port")
    parser.add_argument("--c3-host", default=DEFAULT_C3_HOST, help="C3 HTTP host or IP")
    parser.add_argument("--c3-ip", default=None, help="C3 IP (overrides mDNS)")
    args = parser.parse_args()

    host = args.c3_ip if args.c3_ip else args.c3_host

    # sanity-check C3 HTTP reachability and BLE state
    print(f"Checking C3 at http://{host} ...")
    try:
        st = c3_status(host)
        print(f"C3 status: {st}")
    except Exception as e:
        print(f"FAIL: cannot reach C3 HTTP ({e})")
        return 1

    c3_mac = st.get("ble_addr", "").replace(":", "").upper()
    print(f"C3 BLE address: {c3_mac or 'unknown'}")

    print(f"Opening dongle {args.dongle_port} ...")
    try:
        dgl = serial.Serial(args.dongle_port, 115200, timeout=0.5)
    except Exception as e:
        print(f"FAIL: cannot open dongle port {args.dongle_port}: {e}")
        print("Hint: close any serial monitor / other process using the port.")
        return 1

    time.sleep(0.2)
    dgl.reset_input_buffer()

    print("Dongle version:")
    print(cmd(dgl, b"AT+VER"))

    # clean slate
    print("Disconnect any stale link ...")
    cmd(dgl, b"AT+BT_DISC")
    time.sleep(2.5)

    # scan for C3-Kbd
    print("Scan for C3-Kbd ...")
    cmd(dgl, b"AT+BT_SCAN=5", 0.2)
    text = listen(dgl, 8)
    idx = None
    for line in text.splitlines():
        print(line)
        m = re.search(r"\+BT_SCAN:(\d+),.*C3-Kbd", line)
        if m:
            idx = m.group(1)
        if c3_mac and c3_mac in line.upper().replace(":", ""):
            m2 = re.search(r"\+BT_SCAN:(\d+),", line)
            if m2:
                idx = m2.group(1)
                print(f"(matched C3 MAC) idx={idx}")
    if idx is None:
        print("FAIL: C3-Kbd not found in scan")
        dgl.close()
        return 1
    print(f"Found C3-Kbd at scan index {idx}")

    # connect
    print("Connecting ...")
    cmd(dgl, f"AT+BT_CONN={idx}".encode(), 0.2)
    text = listen(dgl, 12)
    print(text)
    if "armed" not in text:
        print("FAIL: dongle did not arm")
        dgl.close()
        return 1

    # inject keys via C3 HTTP, expect +BT_NTF on dongle
    ok = True
    for kc, name in [(4, "a"), (5, "b")]:
        print(f"Sending key {name} (0x{kc:02X}) ...")
        c3_tap(host, kc, mods=0, ms=100)
        time.sleep(0.5)
        seen = listen(dgl, 2)
        print("dongle saw:", seen or "(silence)")
        needle = f" {kc:02X} "
        if needle in seen or seen.rstrip().endswith(f" {kc:02X}") or f"+BT_NTF:" in seen and f"{kc:02X}" in seen:
            print(f"  [PASS] key {name} forwarded")
        else:
            print(f"  [FAIL] key {name} not seen on dongle")
            ok = False

    dgl.close()
    print("\nALL PASS" if ok else "\nSOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
