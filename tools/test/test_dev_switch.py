#!/usr/bin/env python3
r"""DEV-switch + auto-reconnect test.

  Verifies that after AT+DEV=USB then AT+DEV=BLE, the kbd resumes
  advertising and the dongle (AT+BT_AUTO=1) reconnects automatically.

  Auto-reconnect definition (PLAN.md §2, 2026-07-25):
    Triggered by kbd reset or DEV switch (USB→BLE), NOT by AT+BT_DISC.
    AT+BT_DISC is a user-initiated permanent disconnect (hold); it
    suppresses auto-reconnect until AT+BT_AUTO=1 is issued again.

  Works with single-mode (kbd) and multi-mode (kbd_multi) keyboards.

  Flow:
    0. Factory reset both boards → fresh pair → armed → AT+BT_AUTO=1
    For each round:
      1. AT+DEV=USB    — kbd stops BLE (dongle will lose link)
      2. AT+DEV=BLE    — kbd resumes BLE advertising
      3. Wait for dongle auto-reconnect → armed
      4. AT+KEY inject → verify forwarding

  Exit 0 = all rounds pass, 1 = failure.
"""
import re
import sys
import time

import serial
import serial.tools.list_ports


# ── board discovery ──────────────────────────────────────────────────

def open_role(tag):
    """Find the board whose AT+VER contains the role tag [tag]."""
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x1A86 and p.pid != 0x8010:  # skip WCH-Link / ISP
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
                    print(f"  {tag}: {p.device}")
                    return s
                s.close()
            except (OSError, serial.SerialException):
                pass
    return None


def cmd(ser, c, wait=0.6):
    ser.write(c.encode() if isinstance(c, str) else c)
    ser.write(b"\r\n")
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


def drain(ser):
    ser.read(ser.in_waiting or 4096)


# ── helpers ──────────────────────────────────────────────────────────

def wait_armed(dgl, timeout=20.0):
    """Wait until dongle reaches armed (state 4)."""
    end = time.time() + timeout
    buf = ""
    while time.time() < end:
        buf += listen(dgl, 0.5)
        if "armed" in buf:
            return True
        drain(dgl)
        dgl.write(b"AT+BT_STATE\r\n")
        time.sleep(0.3)
        resp = dgl.read(dgl.in_waiting or 256).decode(errors="replace")
        if "state=4" in resp:
            return True
    return False


def fresh_pair(kbd, dgl):
    """Factory reset both boards, then scan + connect. Returns (kbd, dgl)."""
    print("  factory reset both boards...")
    drain(kbd); drain(dgl)
    kbd.write(b"AT+FACTORY\r\n")
    dgl.write(b"AT+FACTORY\r\n")
    time.sleep(5)

    # Re-discover (ports may change after reset)
    kbd2 = open_role("kbd")
    dgl2 = open_role("dongle")
    if not kbd2 or not dgl2:
        return None, None

    # Disable auto, clear bonds
    cmd(dgl2, "AT+BT_AUTO=0", 0.5)
    drain(dgl2)

    # Multi-mode needs explicit pairing window; single-mode advertises by default
    dev_out = cmd(kbd2, "AT+DEV", 0.3)
    is_multi = dev_out.count("BLE") >= 3
    if is_multi:
        drain(kbd2)
        cmd(kbd2, "AT+BT_PAIR=BLE1", 0.5)

    drain(kbd2)

    # Scan for kbd
    drain(dgl2)
    dgl2.write(b"AT+BT_SCAN=3\r\n")
    time.sleep(4)
    scan_out = dgl2.read(dgl2.in_waiting or 4096).decode(errors="replace")

    # Find AT-Node index
    idx = None
    for m in re.finditer(r"\+BT_SCAN:(\d+),[0-9A-F]+,.*AT-Node", scan_out):
        idx = m.group(1)
        break
    if idx is None:
        print("  ERROR: AT-Node not found in scan")
        return None, None

    # Connect
    drain(dgl2)
    dgl2.write(f"AT+BT_CONN={idx}\r\n".encode())
    out = listen(dgl2, 8)
    if "armed" not in out:
        print("  ERROR: pairing did not reach armed")
        return None, None

    print("  paired + armed")
    return kbd2, dgl2, is_multi


def key_test(kbd, dgl):
    """Inject 'a' on kbd, verify dongle forwards it. Returns ok."""
    drain(dgl); drain(kbd)
    cmd(kbd, "AT+KEY=0,4", 0.3)    # press 'a' (HID code 0x04)
    cmd(kbd, "AT+KEY=0", 0.3)       # release
    out = listen(dgl, 2)
    return " 04 " in out or "0400" in out.replace(" ", "")


# ── main ─────────────────────────────────────────────────────────────

def main():
    results = []

    print("== find boards ==")
    kbd = open_role("kbd")
    dgl = open_role("dongle")
    if not kbd or not dgl:
        print("FAIL: need both [kbd] and [dongle] boards")
        return 1

    # Clean start
    print("\n== fresh pair ==")
    ret = fresh_pair(kbd, dgl)
    if not ret:
        return 1
    kbd, dgl, is_multi = ret

    # Verify initial key forwarding
    ok = key_test(kbd, dgl)
    print(f"  initial key: {'PASS' if ok else 'FAIL'}")
    results.append(("initial key", ok))

    # Enable auto-reconnect on dongle
    cmd(dgl, "AT+BT_AUTO=1", 0.5)
    drain(dgl)

    # ── DEV switch rounds ──────────────────────────────────────────
    print("\n== DEV switch rounds ==")
    dev_ble = "AT+DEV=BLE1" if is_multi else "AT+DEV=BLE"
    rounds = 3

    for r in range(rounds):
        print(f"\n-- round {r + 1} --")

        # 1. Switch to USB
        drain(kbd)
        cmd(kbd, "AT+DEV=USB", 0.8)
        print("  1. DEV=USB")

        # 2. Switch back to BLE
        cmd(kbd, dev_ble, 1.5)
        print(f"  2. {dev_ble}")

        # 3. Wait for auto-reconnect
        print("  3. waiting for dongle auto-reconnect...")
        recon = wait_armed(dgl, 20)
        print(f"     reconnect: {'PASS' if recon else 'FAIL (not armed)'}")
        results.append((f"R{r+1} reconnect", recon))

        # 4. Key test
        if recon:
            ok = key_test(kbd, dgl)
            print(f"  4. key forward: {'PASS' if ok else 'FAIL'}")
            results.append((f"R{r+1} key", ok))
        else:
            results.append((f"R{r+1} key", False))

    # ── summary ─────────────────────────────────────────────────────
    print("\n" + "=" * 50)
    print("SUMMARY")
    print("=" * 50)
    all_ok = True
    for name, ok in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
        all_ok &= ok

    print("\n" + ("ALL PASS" if all_ok else "SOME FAILED"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
