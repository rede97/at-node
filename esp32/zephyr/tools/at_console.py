#!/usr/bin/env python3
"""Persistent bidirectional AT console for the nanoESP32-S3 (ESPLink UART0).

Holds the serial port open so the board is never reset by repeated opens
(ESPLink DTR/RTS -> EN/IO0). stdin lines are forwarded to the board,
board output goes to stdout. Run under a supervisor (hub start).

Usage: at_console.py [port]   (default: ESPLink by-id path)
"""
import sys
import threading
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else \
    '/dev/serial/by-id/usb-MuseLab_DAPLink_CMSIS-DAP_0800000100570061330000034e503750a5a5a5a597969908-if01'

s = serial.Serial(PORT, 115200, timeout=0.2)
s.dtr = False
s.rts = False


def pump_stdin():
    for line in sys.stdin:
        s.write(line.rstrip('\n').encode() + b'\r\n')
        s.flush()


threading.Thread(target=pump_stdin, daemon=True).start()
print('=== at_console ready ===', flush=True)
while True:
    try:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode(errors='replace'))
            sys.stdout.flush()
    except Exception as e:
        print(f'=== serial error: {e} ===', flush=True)
        time.sleep(1)
