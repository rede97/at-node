#!/usr/bin/env python3
r"""List all at-node boards on USB and report their firmware role.

  One line per board:  <port>  <role>   e.g.  /dev/ttyACM0  kbd
  Role comes from the AT+VER tag ([kbd]/[dongle]); "?" = no answer.

  Usage:  board_roles.py [--wait SEC] [--require ROLE]
    --wait SEC     retry until at least one board answers (default 0)
    --require ROLE exit 1 unless some board reports ROLE
"""
import argparse
import sys
import time

import serial
import serial.tools.list_ports

VID = 0x1A86
PID_ISP = 0x8010  # ISP bootloader / WCH-Link — never an at-node app


def find_boards():
    return [p for p in serial.tools.list_ports.comports()
            if p.vid == VID and p.pid != PID_ISP]


def query_role(port):
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
        time.sleep(0.2)
        s.write(b"\r\n")
        time.sleep(0.3)
        s.reset_input_buffer()
        s.write(b"AT+VER\r\n")
        time.sleep(0.5)
        resp = s.read(256).decode(errors="replace")
        s.close()
        for role in ("kbd", "dongle"):
            if f"[{role}]" in resp:
                return role
    except (OSError, serial.SerialException):
        pass
    return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wait", type=float, default=0)
    ap.add_argument("--require", default=None)
    args = ap.parse_args()

    deadline = time.time() + args.wait
    roles = {}
    while True:
        roles = {}
        for p in find_boards():
            roles[p.device] = query_role(p.device)
        if any(r != "?" for r in roles.values()) or time.time() >= deadline:
            break
        time.sleep(0.5)

    if not roles:
        print("no at-node boards found", file=sys.stderr)
        return 1
    for port, role in sorted(roles.items()):
        print(f"{port}  {role}")
    if args.require and args.require not in roles.values():
        print(f"required role [{args.require}] not present", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
