#!/usr/bin/env python3
r"""Send HID key via AT-Node. Usage:

  send_key.py <keycode> [--mode USB|BLE|BOTH]
  send_key.py --seq "Hello, World!" [--mode BLE] [--delay 5]

Single key mode: script builds raw HID report, sends down then up.
  send_key.py 0x39                     # CapsLock press+release
  send_key.py 0x04 --mode BLE          # 'a' via BLE
  send_key.py 0xE1 0x04                # Shift+a (capital A)

Sequence mode (--seq): batch HID playback via AT+KEY_SEQ.
  Script translates text to HID reports, firmware plays back at delay_ms
  via TMOS timer (async). Text is chunked to 8 reports per command —
  firmware limit SEQ_MAX_REPORTS=8.

HID key codes (hex):
  Letters:    a=0x04 b=0x05 .. z=0x1D
  Numbers:    1=0x1E 2=0x1F .. 0=0x27
  F-keys:     F1=0x3A F2=0x3B .. F12=0x45
  Navigation: Left=0x50 Right=0x4F Up=0x52 Down=0x51
  Modifiers (byte 0 of report, handled automatically):
    Ctrl=0xE0 LeftShift=0xE1 Alt=0xE2 Win=0xE3
  Special:    Enter=0x28 Esc=0x29 Backspace=0x2A Tab=0x2B Space=0x2C
              CapsLock=0x39
"""

import serial, serial.tools.list_ports, sys, time, argparse

# Keys that go in the modifier byte (bits 0-3 of byte 0),
# not in the key array (bytes 2-7).
MODIFIER_KEYS = {
    0xE0: 0x01,  # Left Ctrl
    0xE1: 0x02,  # Left Shift
    0xE2: 0x04,  # Left Alt
    0xE3: 0x08,  # Left GUI (Win)
    0xE4: 0x01,  # Right Ctrl  -> same bit as left
    0xE5: 0x02,  # Right Shift -> same bit as left
    0xE6: 0x04,  # Right Alt   -> same bit as left
    0xE7: 0x08,  # Right GUI   -> same bit as left
}

# ASCII -> (shift_mod, HID_keycode) mapping
# USB HID Usage Tables: Keyboard/Keypad Page (0x07)
ASCII_TO_HID = {
    'a':(0,0x04),'b':(0,0x05),'c':(0,0x06),'d':(0,0x07),'e':(0,0x08),
    'f':(0,0x09),'g':(0,0x0A),'h':(0,0x0B),'i':(0,0x0C),'j':(0,0x0D),
    'k':(0,0x0E),'l':(0,0x0F),'m':(0,0x10),'n':(0,0x11),'o':(0,0x12),
    'p':(0,0x13),'q':(0,0x14),'r':(0,0x15),'s':(0,0x16),'t':(0,0x17),
    'u':(0,0x18),'v':(0,0x19),'w':(0,0x1A),'x':(0,0x1B),'y':(0,0x1C),
    'z':(0,0x1D),
    'A':(2,0x04),'B':(2,0x05),'C':(2,0x06),'D':(2,0x07),'E':(2,0x08),
    'F':(2,0x09),'G':(2,0x0A),'H':(2,0x0B),'I':(2,0x0C),'J':(2,0x0D),
    'K':(2,0x0E),'L':(2,0x0F),'M':(2,0x10),'N':(2,0x11),'O':(2,0x12),
    'P':(2,0x13),'Q':(2,0x14),'R':(2,0x15),'S':(2,0x16),'T':(2,0x17),
    'U':(2,0x18),'V':(2,0x19),'W':(2,0x1A),'X':(2,0x1B),'Y':(2,0x1C),
    'Z':(2,0x1D),
    '1':(0,0x1E),'!':(2,0x1E),  '2':(0,0x1F),'@':(2,0x1F),
    '3':(0,0x20),'#':(2,0x20),  '4':(0,0x21),'$':(2,0x21),
    '5':(0,0x22),'%':(2,0x22),  '6':(0,0x23),'^':(2,0x23),
    '7':(0,0x24),'&':(2,0x24),  '8':(0,0x25),'*':(2,0x25),
    '9':(0,0x26),'(':(2,0x26),  '0':(0,0x27),')':(2,0x27),
    ' ':(0,0x2C), '\n':(0,0x28), '\t':(0,0x2B),
    '-':(0,0x2D),'_':(2,0x2D),  '=':(0,0x2E),'+':(2,0x2E),
    '[':(0,0x2F),'{':(2,0x2F),  ']':(0,0x30),'}':(2,0x30),
    '\\':(0,0x31),'|':(2,0x31),
    ';':(0,0x33),':':(2,0x33),  "'":(0,0x34),'"':(2,0x34),
    '`':(0,0x35),'~':(2,0x35),  ',':(0,0x36),'<':(2,0x36),
    '.':(0,0x37),'>':(2,0x37),  '/':(0,0x38),'?':(2,0x38),
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
    deadline = time.time() + 3.0  # longer timeout for KEY_SEQ
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
    """Send single raw HID report: AT+KEY=<mods>,<k1>,..,<k6>"""
    ks = [str(k) for k in keys] + ["0"] * (6 - len(keys))
    at = f"AT+KEY={mods},{','.join(ks)}\n".encode()
    return cmd(ser, at)


# Firmware plays reports back asynchronously via TMOS timer and accepts
# at most SEQ_MAX_REPORTS=8 per command (AT_LINE_MAX=256 bound). Longer
# text is chunked here; between chunks we must wait for playback to
# finish — a new AT+KEY_SEQ replaces the running queue.
MAX_REPORTS_PER_CMD = 8


def send_sequence(ser, text, delay_ms=5):
    """Send text via AT+KEY_SEQ — batch HID report playback.

    Converts each character to a press+release HID report pair,
    packs into chunked AT+KEY_SEQ commands (<=8 reports each) and
    paces chunks to the firmware's async playback.
    """
    reports = []
    for ch in text:
        if ch not in ASCII_TO_HID:
            print(f"  [SKIP] unmapped char: {repr(ch)}")
            continue
        shift, kc = ASCII_TO_HID[ch]
        # Key down
        reports.append((shift, [kc, 0, 0, 0, 0, 0]))
        # Key up (release all)
        reports.append((0, [0, 0, 0, 0, 0, 0]))

    if not reports:
        return "ERR: no mappable characters"

    print(f"  SEQ: {len(reports)} reports, "
          f"{(len(reports) + MAX_REPORTS_PER_CMD - 1) // MAX_REPORTS_PER_CMD} chunk(s)")
    for i in range(0, len(reports), MAX_REPORTS_PER_CMD):
        chunk = reports[i:i + MAX_REPORTS_PER_CMD]
        parts = [f"AT+KEY_SEQ={delay_ms}"]
        for mods, keys in chunk:
            parts.append(str(mods))
            for k in keys:
                parts.append(str(k))
        at = ",".join(parts).encode() + b"\n"

        resp = cmd(ser, at)
        if "OK" not in resp:
            return f"ERR chunk {i // MAX_REPORTS_PER_CMD}: {resp}"
        # Wait for async playback of this chunk before sending the next,
        # otherwise the new command replaces the running queue.
        time.sleep(len(chunk) * delay_ms / 1000 + 0.05)
    return f"OK {len(reports)} reports"


def main():
    ap = argparse.ArgumentParser(description="Send HID keys via AT-Node")
    ap.add_argument("keycodes", nargs="*",
                    help="HID key codes (hex), e.g. 0x04 0x39")
    ap.add_argument("--seq", help="Text string to send via AT+KEY_SEQ")
    ap.add_argument("--delay", type=int, default=5,
                    help="Delay ms between reports for --seq (default: 5)")
    ap.add_argument("--mode", choices=["USB", "BLE", "BOTH"], default="BOTH")
    ap.add_argument("--port", help="COM port (auto-detect)")
    ap.add_argument("--status", action="store_true", help="show status only")
    args = ap.parse_args()

    if not args.keycodes and not args.seq and not args.status:
        ap.error("need keycodes, --seq, or --status")

    # Find device
    port = args.port or find_port()
    if not port:
        print("FAIL: no device found")
        sys.exit(1)

    ser = serial.Serial(port, 115200, timeout=0.3)
    print(f"Device: {port}")

    # Show status
    resp = cmd(ser, b"AT\n")
    time.sleep(0.1)
    resp = cmd(ser, b"AT+KB\n")
    print(f"Status: {resp}")

    if args.status:
        ser.close()
        return

    # Set mode (skip for KEY_SEQ — it uses the same kb_flush path)
    time.sleep(0.1)
    resp = cmd(ser, f"AT+KB={args.mode}\n".encode())
    if "OK" not in resp:
        print(f"FAIL: mode switch — {resp}")
        ser.close()
        sys.exit(1)
    print(f"Mode: {args.mode}")

    # --- Sequence mode: batch HID playback ---
    if args.seq:
        resp = send_sequence(ser, args.seq, args.delay)
        print(f"  [{('OK' if 'OK' in resp else 'ERR')}] {resp}")
        ser.close()
        print("Done.")
        return

    # --- Single key mode ---
    mods = 0
    regulars = []
    for kc_str in args.keycodes:
        kc = int(kc_str, 0)
        if kc in MODIFIER_KEYS:
            mods |= MODIFIER_KEYS[kc]
        else:
            regulars.append(kc)

    # Key down — all keys held
    resp = send_hid(ser, mods, regulars)
    tag = "OK" if "OK" in resp else "ERR"
    print(f"  [{tag}] DOWN mods={mods:02X} keys={[hex(k) for k in regulars]}")

    # Key up — release all
    resp = send_hid(ser, 0, [])
    tag = "OK" if "OK" in resp else "ERR"
    print(f"  [{tag}] UP   (release all)")

    ser.close()
    print("Done.")


if __name__ == "__main__":
    main()
