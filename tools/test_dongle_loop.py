#!/usr/bin/env python3
r"""Dongle closed-loop test — two CH582 boards, one script drives both.

  Board B (kbd firmware, main branch): fixed test keyboard.
    Keys are injected with AT+KEY over its CDC.
  Board A (dongle firmware, dongle-wip branch): BLE HID receiver.
    Reports arriving from B must appear as +BT_NTF lines on its CDC.

  Boards are auto-detected by the AT+VER role tag: "AT-Node v1.0 [kbd]"
  vs "AT-Node v1.0 [dongle]" (both are WCH VID 0x1A86).

  Flow: scan -> connect -> armed -> inject 'a','b' -> verify bytes.
  Exit 0 = pass, 1 = fail.
"""
import re
import sys
import time

import serial
import serial.tools.list_ports


def open_role(tag):
    """Find the board whose AT+VER contains the role tag [tag]."""
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x1A86 and p.pid != 0x8010:  # skip ISP bootloader
            try:
                s = serial.Serial(p.device, 115200, timeout=0.3)
                time.sleep(0.2)
                s.write(b"\r\n")
                time.sleep(0.3)
                s.reset_input_buffer()
                s.write(b"AT+VER\r\n")
                time.sleep(0.5)
                resp = s.read(256).decode(errors="replace")
                if f"[{tag}]" in resp:
                    print(f"{tag}: {p.device}")
                    return s
                s.close()
            except (OSError, serial.SerialException):
                pass
    return None


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


def main():
    kbd = open_role("kbd")
    dgl = open_role("dongle")
    if not kbd or not dgl:
        print("FAIL: need both boards ([kbd] and [dongle] AT+VER tags)")
        return 1

    # kbd: BLE only, so injected keys don't also type on this PC over USB
    print(cmd(kbd, b"AT+KB=BLE"))

    # dongle: scan for the kbd board advertising as "AT-Node"
    cmd(dgl, b"AT+BT_SCAN=5", 0.2)
    text = listen(dgl, 8)
    idx = None
    for line in text.splitlines():
        m = re.search(r"\+BT_SCAN:(\d+),.*AT-Node", line)
        if m:
            idx = m.group(1)
            print(line)
    if idx is None:
        print("FAIL: AT-Node keyboard not found in scan")
        return 1

    print(cmd(dgl, f"AT+BT_CONN={idx}".encode(), 0.2))
    text = listen(dgl, 10)
    print(text)
    if "armed" not in text:
        print("FAIL: dongle did not arm")
        return 1

    # inject keys on kbd, expect matching +BT_NTF bytes on dongle
    ok = True
    seen = ""
    for kc, name in [(4, "a"), (5, "b")]:
        cmd(kbd, f"AT+KEY=0,{kc}".encode())
        time.sleep(0.3)
        cmd(kbd, b"AT+KEY=0")
        time.sleep(0.3)
    seen = listen(dgl, 3)
    print("dongle saw:", seen or "(silence)")
    for kc, name in [(4, "a"), (5, "b")]:
        if f" {kc:02X} " in seen or seen.endswith(f" {kc:02X}"):
            print(f"  [PASS] key {name} ({kc:#04x}) forwarded")
        else:
            print(f"  [FAIL] key {name} ({kc:#04x}) not seen on dongle")
            ok = False

    print("\nALL PASS" if ok else "\nSOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
