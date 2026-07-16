#!/usr/bin/env python3
"""Send HID key via AT-Node. Usage:

  send_key.py <keycode> [--mode USB|BLE|BOTH]

Single keycode (0x04=0x65): script builds raw HID report, sends down then up.
  send_key.py 0x39                     # CapsLock press+release
  send_key.py 0x04 --mode BLE          # 'a' via BLE
  send_key.py 0xE1 --mode USB          # Left Shift (modifier) via USB

Multiple keys can be held together by passing more keycodes.
All keys held until final report releases them.
  send_key.py 0xE1 0x04                # Shift+a (capital A)
  send_key.py 0xE0 0xE2 0x2B          # Ctrl+Alt+Del (uses kb_release_all)

HID key codes (hex):
  Letters:    a=0x04 b=0x05 .. z=0x1D
  Numbers:    1=0x1E 2=0x1F .. 0=0x27
  F-keys:     F1=0x3A F2=0x3B .. F12=0x45
  Navigation: Left=0x50 Right=0x4F Up=0x52 Down=0x51
  Modifiers (byte 0 of report, handled automatically):
    Ctrl=0xE0 Shift=0xE1 Alt=0xE2 Win=0xE3
  Special:    Enter=0x28 Esc=0x29 Backspace=0x2A Tab=0x2B Space=0x2C
              CapsLock=0x39
"""

import serial, serial.tools.list_ports, sys, time, argparse

# Keys that go in the modifier byte (bits 0–3 of byte 0),
# not in the key array (bytes 2–7).
MODIFIER_KEYS = {
    0xE0: 0x01,  # Left Ctrl
    0xE1: 0x02,  # Left Shift
    0xE2: 0x04,  # Left Alt
    0xE3: 0x08,  # Left GUI (Win)
    0xE4: 0x01,  # Right Ctrl  → same bit as left
    0xE5: 0x02,  # Right Shift → same bit as left
    0xE6: 0x04,  # Right Alt   → same bit as left
    0xE7: 0x08,  # Right GUI   → same bit as left
}

def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x1A86 and p.pid != 0x8010:
            return p.device
    return None

def cmd(ser, c):
    ser.reset_input_buffer()
    ser.write(c)
    ser.flush()
    parts = []
    deadline = time.time() + 1.0
    while time.time() < deadline:
        b = ser.read(256)
        if b:
            parts.append(b.decode(errors="replace"))
            if b"OK" in b or b"ERROR" in b:
                break
        else:
            time.sleep(0.05)
    return "".join(parts).strip()

def send_hid(ser, mods, keys):
    """Send raw HID report: AT+KEY=<mods>,<k1>,..,<k6>"""
    ks = [str(k) for k in keys] + ["0"] * (6 - len(keys))
    at = f"AT+KEY={mods},{','.join(ks)}\n".encode()
    resp = cmd(ser, at)
    return resp

def main():
    ap = argparse.ArgumentParser(description="Send HID keys via AT-Node")
    ap.add_argument("keycodes", nargs="+", help="HID key codes (hex), e.g. 0x04 0x39")
    ap.add_argument("--mode", choices=["USB","BLE","BOTH"], default="BOTH")
    ap.add_argument("--port", help="COM port (auto-detect)")
    ap.add_argument("--status", action="store_true", help="show status only")
    args = ap.parse_args()

    # Parse keycodes, separate modifiers from regular keys
    mods = 0
    regulars = []
    for kc_str in args.keycodes:
        kc = int(kc_str, 0)
        if kc in MODIFIER_KEYS:
            mods |= MODIFIER_KEYS[kc]
        else:
            regulars.append(kc)

    # Find device
    port = args.port or find_port()
    if not port:
        print("FAIL: no device found")
        sys.exit(1)

    ser = serial.Serial(port, 115200, timeout=0.3)
    print(f"Device: {port}")

    # Show status
    resp = cmd(ser, b"AT\n"); time.sleep(0.1)
    resp = cmd(ser, b"AT+KB\n")
    print(f"Status: {resp}")

    if args.status:
        ser.close(); return

    # Set mode
    time.sleep(0.1)
    resp = cmd(ser, f"AT+KB={args.mode}\n".encode())
    if "OK" not in resp:
        print(f"FAIL: mode switch — {resp}")
        ser.close(); sys.exit(1)
    print(f"Mode: {args.mode}")

    # Send key down — full HID report with all keys held
    resp = send_hid(ser, mods, regulars)
    tag = "OK" if "OK" in resp else "ERR"
    print(f"  [{tag}] DOWN mods={mods:02X} keys={[hex(k) for k in regulars]}")

    # Send key up — release everything
    resp = send_hid(ser, 0, [])
    tag = "OK" if "OK" in resp else "ERR"
    print(f"  [{tag}] UP   (release all)")

    ser.close()
    print("Done.")

if __name__ == "__main__":
    main()
