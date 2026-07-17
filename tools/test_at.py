#!/usr/bin/env python3
"""AT command tester — auto-detects device, runs tests. Exit 0=pass, 1=fail."""
import serial, serial.tools.list_ports, sys, time

def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x1A86 and p.pid != 0x8010:
            return p.device
    return None

def test(ser, cmd, expect=None):
    ser.write(cmd)
    parts = []
    for _ in range(4):  # read in 4 passes to capture multi-packet response
        time.sleep(0.2)
        b = ser.read(256)
        if b:
            parts.append(b.decode(errors="replace"))
        else:
            break
    text = "".join(parts)
    ok = "OK" in text
    match = expect is None or expect in text
    s = "PASS" if (ok and match) else "FAIL"
    print(f"  [{s}] {cmd.decode().strip()!r} -> {text.strip()[:80]}")
    return ok and match

port = sys.argv[1] if len(sys.argv) > 1 else find_port()
if not port:
    print("FAIL: no device"); sys.exit(1)
print(f"Device: {port}")
ser = serial.Serial(port, 115200, timeout=0.3)
ser.dtr = True
# Warmup: first OUT transfer after port open can be lost (CDC line-state
# change races endpoint setup). Send a blank line and drain before testing.
time.sleep(0.3)
ser.write(b"\r\n")
time.sleep(0.3)
ser.reset_input_buffer()

all_ok = True
all_ok &= test(ser, b"AT\r\n")
all_ok &= test(ser, b"AT+VER\r\n", "AT-Node")
all_ok &= test(ser, b"AT+KB\r\n")
all_ok &= test(ser, b"AT+ECHO=hello\r\n", "hello")
all_ok &= test(ser, b"AT+HELP\r\n", "AT+VER")
all_ok &= test(ser, b"AT+KEY=0\r\n")  # all-zero report: exercises handler, no keystroke
ser.close()
print("\nALL PASS" if all_ok else "\nSOME FAILED")
sys.exit(0 if all_ok else 1)
