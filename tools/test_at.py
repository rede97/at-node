#!/usr/bin/env python3
"""AT command tester — sends commands over CDC serial, reads responses."""
import serial, sys, time

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM14"

ser = serial.Serial(PORT, 115200, timeout=1)
ser.dtr = True
print(f"Connected to {PORT}")

tests = [
    b"AT\r\n",
    b"AT+VER\r\n",
    b"AT+ECHO=hello\r\n",
    b"AT+HELP\r\n",
]

for cmd in tests:
    cmd_str = cmd.decode().strip()
    ser.write(cmd)
    time.sleep(0.2)
    # read all available response
    ser.timeout = 0.1
    lines = []
    while True:
        try:
            b = ser.read(1)
            if not b: break
            if b == b'\n':
                line = b"".join(lines).decode(errors="replace").strip()
                if line: print(f"  <<< {line}")
                lines = []
            else:
                lines.append(b)
        except: break
    print()
ser.close()
print("Done.")
