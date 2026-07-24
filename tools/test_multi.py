#!/usr/bin/env python3
"""test_multi.py — kbd_multi (KBD_MULTI) multi-host test suite (F1.10–F1.12).

Topology:
    kbd_multi board (Peripheral, 3 slots)  == under test, AT via USB CDC
    host1: Linux + CSR dongle (hci1)       == BLE host, asserts via evdev
    host2: dongle board (Central)          == BLE host, asserts via +BT_NTF

Prereqs: both boards flashed (kbd_multi + dongle), Linux paired once
(tools/bt_host.py pair), dongle bonded to AT-Node (auto on first connect).

    uv run python tools/test_multi.py [--taps N]

Tests:
    M1  AT+DEV query — 4 index-first lines, both hosts listed connected
    M2  single-target routing BLE1 / BLE2 — only the selected host receives
    M3  DEV=ALL broadcast — both hosts receive every tap
    M4  AT+STATUS dev= field
    M5  AT+BT_DISC=<slot> — URC +BT_DISCONNECTED:<slot> fires
"""
import argparse
import re
import serial
import sys
import time

sys.path.insert(0, "tools")
import bt_host  # noqa: E402

# AT command arguments are parsed by atoi() — DECIMAL. HID 0x14 ('t') is
# decimal 20 in the command, printed as "14" in dongle +BT_NTF hex dumps.
# (Field lesson 2026-07-24: writing 0x14 into the command string sends
# decimal 14 = 0x0E — tests must check 0x0E or send decimal.)
TAP_KEY_DEC = 20      # 't' = HID 0x14, sent as decimal
LINUX_KEY_T = 20      # evdev code for 't'
TAP_NTF_HEX = f"{TAP_KEY_DEC:02X} 00"   # dongle hex-dump pattern


def open_port_by_role(role, tries=10):
    """Find the at-node CDC port whose AT+VER reports the given role."""
    import serial.tools.list_ports
    for _ in range(tries):
        for p in serial.tools.list_ports.comports():
            if p.vid == 0x1A86 and p.pid != 0x8010:
                try:
                    s = serial.Serial(p.device, 115200, timeout=0.3)
                    time.sleep(0.15)
                    s.write(b"\r\n"); time.sleep(0.2); s.reset_input_buffer()
                    s.write(b"AT+VER\r\n"); time.sleep(0.4)
                    if f"[{role}]" in s.read(256).decode(errors="replace"):
                        return s
                    s.close()
                except (OSError, serial.SerialException):
                    pass
        time.sleep(1)
    raise SystemExit(f"no [{role}] board found")


def cmd(ser, c, wait=0.6):
    ser.reset_input_buffer()
    ser.write(c.encode() + b"\r\n")
    time.sleep(wait)
    return ser.read(ser.in_waiting or 1).decode(errors="replace")


def dev_lines(ser):
    out = cmd(ser, "AT+DEV", 1.0)
    return [l.strip() for l in out.splitlines() if re.match(r"^[1-4],", l.strip())]


def dgl_recv(dgl, pattern, sec=1.2):
    """Collect dongle +BT_NTF lines for sec; True if hex pattern seen."""
    end = time.time() + sec
    buf = ""
    while time.time() < end:
        buf += dgl.read(dgl.in_waiting or 1).decode(errors="replace")
        time.sleep(0.05)
    return pattern in buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--taps", type=int, default=5)
    args = ap.parse_args()

    kbd = open_port_by_role("kbd")
    dgl = open_port_by_role("dongle")
    print(f"kbd={kbd.port} dongle={dgl.port}")

    # --- make sure both BLE hosts are connected ---
    if not bt_host.ensure_connected(bt_host.find_mac(secs=3) or ""):
        print("WARN: Linux host not connected")
    out = cmd(dgl, "AT+BT_STATE")
    if "connect=0" in out or "connected" not in out:
        cmd(dgl, "AT+BT_SCAN=3,AT-Node", 4.5)
        cmd(dgl, "AT+BT_CONN=AT-Node", 6)
    time.sleep(1)

    results = []

    # M1 — AT+DEV structure
    lines = dev_lines(kbd)
    slots = {}
    for l in lines:
        parts = l.split(",")
        slots[parts[1]] = parts
    ok = (len(lines) == 4 and lines[0].startswith("1,USB")
          and any("connected" in l for l in lines[1:]))
    print(f"M1 AT+DEV structure: {'PASS' if ok else 'FAIL'}")
    for l in lines:
        print("   ", l)
    results.append(("M1", ok))

    ble_slots = [k for k in slots if k.startswith("BLE") and "connected" in ",".join(slots[k])]
    host2_slot = None
    for k in ble_slots:  # dongle board MAC has :32 suffix on this rig
        if slots[k][2].endswith(":32"):
            host2_slot = k
    linux_slot = next((k for k in ble_slots if k != host2_slot), None)
    print(f"   slots: linux={linux_slot} dongle-board={host2_slot}")

    if len(ble_slots) >= 2 and host2_slot and linux_slot:
        # M2 — single-target routing (dongle-board side is deterministic)
        cmd(kbd, f"AT+DEV={host2_slot}")
        hit = dgl_recv_after(kbd, dgl)
        print(f"M2 route {host2_slot} only: {'PASS' if hit else 'FAIL'}")
        results.append(("M2", hit))

        # M3 — broadcast
        cmd(kbd, "AT+DEV=ALL")
        ok_l = ok_d = 0
        for _ in range(args.taps):
            dgl.reset_input_buffer()
            kbd.write(f"AT+TAP=80,0,{TAP_KEY_DEC}\r\n".encode())
            got_l = any(c == LINUX_KEY_T and v == 1
                        for c, v in bt_host.listen_events(None, 0.9))
            ok_l += got_l
            ok_d += dgl_recv(dgl, TAP_NTF_HEX, 0.3)
            time.sleep(0.3)
        ok = ok_l >= args.taps - 1 and ok_d >= args.taps - 1
        print(f"M3 broadcast ALL: linux {ok_l}/{args.taps} dongle {ok_d}/{args.taps}"
              f" -> {'PASS' if ok else 'FAIL'}")
        results.append(("M3", ok))
    else:
        print("M2/M3 SKIP: need both BLE hosts connected")
        results += [("M2", None), ("M3", None)]

    # M4 — STATUS dev field
    out = cmd(kbd, "AT+STATUS")
    ok = "dev=" in out and "conn" in out
    print(f"M4 STATUS: {'PASS' if ok else 'FAIL'}  ({out.splitlines()[1] if chr(10) in out else out})")
    results.append(("M4", ok))

    # M5 — slot disconnect URC
    if host2_slot:
        out = cmd(kbd, f"AT+BT_DISC={host2_slot}", 2.0)
        slot_num = host2_slot[-1]
        ok = f"+BT_DISCONNECTED:{slot_num}" in out
        print(f"M5 BT_DISC={host2_slot} URC: {'PASS' if ok else 'FAIL'}")
        results.append(("M5", ok))
    else:
        results.append(("M5", None))

    bad = [n for n, r in results if r is False]
    print("==>", "ALL PASS" if not bad else f"FAIL: {bad}")
    return 1 if bad else 0


def dgl_recv_after(kbd, dgl):
    dgl.reset_input_buffer()
    kbd.write(f"AT+TAP=80,0,{TAP_KEY_DEC}\r\n".encode())
    return dgl_recv(dgl, TAP_NTF_HEX)


if __name__ == "__main__":
    sys.exit(main())
