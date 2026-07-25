#!/usr/bin/env python3
"""test_dev_switch_accept.py — DEV switch acceptance test (N rounds).

Round trip: BLE2 (dongle -> Linux terminal) <-> BLE1 (Windows terminal).
Dongle side types `echo Dn >> /tmp/devsw.log` which EXECUTES in the
focused Linux terminal — the script then reads the file to self-verify.
Windows side is watched by the user.

Prereqs: BLE1=Windows paired, BLE2=dongle board paired, a terminal
focused on the Linux box (dongle forwards keys there).

    uv run python tools/test_dev_switch_accept.py [--rounds 5]
"""
import argparse
import os
import re
import serial
import sys
import time

LOG = "/tmp/devsw.log"


def open_port(tag):
    import serial.tools.list_ports
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x1A86 and p.pid != 0x8010:
            try:
                s = serial.Serial(p.device, 115200, timeout=0.3)
                time.sleep(0.15)
                s.write(b"\r\n"); time.sleep(0.2); s.reset_input_buffer()
                s.write(b"AT+VER\r\n"); time.sleep(0.4)
                if f"[{tag}]" in s.read(256).decode(errors="replace"):
                    return s
                s.close()
            except (OSError, serial.SerialException):
                pass
    raise SystemExit(f"no [{tag}] board")


def wait_urc(ser, pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        data = ser.read(ser.in_waiting or 1).decode(errors="replace")
        if pattern in data:
            return time.time()
        time.sleep(0.1)
    return None


def type_sync(kbd, text):
    kbd.write(f"AT+KEY_STR={text}\r\n".encode())
    if not wait_urc(kbd, "+KEY_DONE", len(text) * 0.1 + 5):
        return False
    kbd.write(b"AT+TAP=80,0,40\r\n")   # Enter
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5)
    args = ap.parse_args()

    kbd = open_port("kbd")
    print(f"kbd={kbd.port}  rounds={args.rounds}")
    try:
        os.unlink(LOG)
    except FileNotFoundError:
        pass

    results = []
    for n in range(1, args.rounds + 1):
        # --- to dongle (BLE2) ---
        kbd.reset_input_buffer()
        kbd.write(b"AT+DEV=BLE2\r\n")
        t0 = time.time()
        ok = wait_urc(kbd, "+BT_CONNECTED:2", 25) or n == 1 and time.time()
        if not ok and n > 1:
            results.append((n, "BLE2 reconnect FAIL", None)); continue
        recon = (ok - t0) if ok and n > 1 else 0
        time.sleep(2.5)   # let the dongle finish GATT/arm
        typed = type_sync(kbd, f"echo D{n} >> {LOG}")
        # --- to Windows (BLE1) ---
        kbd.reset_input_buffer()
        kbd.write(b"AT+DEV=BLE1\r\n")
        t1 = time.time()
        ok1 = wait_urc(kbd, "+BT_CONNECTED:1", 25)
        recon1 = (ok1 - t1) if ok1 else None
        if ok1:
            time.sleep(1.5)
            type_sync(kbd, f"win round {n}")
        results.append((n, recon, recon1))
        print(f"round {n}: BLE2_recon={recon:.1f}s typed={'Y' if typed else 'N'} "
              f"BLE1_recon={recon1:.1f}s" if recon1 is not None else
              f"round {n}: BLE1 reconnect FAIL")

    time.sleep(2)
    got = ""
    try:
        got = open(LOG).read()
    except FileNotFoundError:
        pass
    hits = sorted(set(re.findall(r"^D(\d)$", got, re.M)), key=int)
    print(f"self-verify {LOG}: lines D* = {hits}")
    ok = len(hits) >= args.rounds - 1
    print("ACCEPT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
