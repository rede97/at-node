#!/usr/bin/env python3
r"""Send AT commands to a board by role or port.

  usage:
    uv run python tools/at_cli.py --role kbd "AT+VER"
    uv run python tools/at_cli.py --role dongle "AT+BT_AUTO=0" "AT+BT_PAIR"
    uv run python tools/at_cli.py --port /dev/ttyACM0 "AT+KEY=0,4" "AT+KEY=0"
    uv run python tools/at_cli.py --role dongle --listen 6 "AT+BT_SCAN=3"

  --role TAG   find the board whose AT+VER reports [TAG] (kbd|dongle)
  --port DEV   use a fixed serial port instead
  --listen N   after the last command, keep printing output for N seconds
  Exit 1 if the board is not found or any command answers ERROR.
"""
import argparse
import sys
import time

import serial
import serial.tools.list_ports

VID = 0x1A86
PID_WCH_LINK = 0x8010


def find_by_role(tag, retries=10):
    for _ in range(retries):
        for p in serial.tools.list_ports.comports():
            if p.vid == VID and p.pid != PID_WCH_LINK:
                try:
                    s = serial.Serial(p.device, 115200, timeout=0.3)
                    time.sleep(0.2)
                    s.write(b"\r\n")
                    time.sleep(0.3)
                    s.reset_input_buffer()
                    s.write(b"AT+VER\r\n")
                    time.sleep(0.6)
                    resp = s.read(256).decode(errors="replace")
                    if f"[{tag}]" in resp:
                        return s
                    s.close()
                except (OSError, serial.SerialException):
                    pass
        time.sleep(1)
    return None


def cmd(ser, c, wait=0.8):
    ser.reset_input_buffer()
    ser.write(c.encode() + b"\r\n")
    time.sleep(wait)
    return ser.read(4096).decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="+")
    ap.add_argument("--role", choices=["kbd", "dongle"])
    ap.add_argument("--port")
    ap.add_argument("--listen", type=float, default=0)
    args = ap.parse_args()

    if args.port:
        ser = serial.Serial(args.port, 115200, timeout=0.3)
        time.sleep(0.3)
    elif args.role:
        ser = find_by_role(args.role)
        if not ser:
            print(f"FAIL: no [{args.role}] board found", file=sys.stderr)
            return 1
        print(f"# {args.role}: {ser.port}")
    else:
        ap.error("--role or --port required")

    rc = 0
    for c in args.commands:
        resp = cmd(ser, c)
        print(resp, end="" if resp.endswith("\n") else "\n")
        if "ERROR" in resp:
            rc = 1

    if args.listen:
        end = time.time() + args.listen
        while time.time() < end:
            b = ser.read(1024)
            if b:
                print(b.decode(errors="replace"), end="")
            else:
                time.sleep(0.05)
    ser.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
