#!/usr/bin/env python3
r"""Type text via the ESP32-C3 BLE keyboard bench.

  The C3 runs tools/esp32c3_kbd/esp32c3_kbd.ino (BLE HID keyboard + HTTP).
  This script sends the text to the C3, which transmits key reports over BLE.
  If a CH582 dongle is paired and listening, the text appears as USB HID
  keystrokes on the host PC.

  Example:
      .venv\Scripts\python tools/c3_type.py --host 192.168.1.27 "Hello World"
"""
import argparse
import sys

import requests


def main():
    parser = argparse.ArgumentParser(description="Type text via C3 BLE keyboard")
    parser.add_argument("text", help="text to type")
    parser.add_argument("--host", default="esp32kbd.local", help="C3 HTTP host or IP")
    parser.add_argument("--ip", default=None, help="C3 IP (overrides --host)")
    parser.add_argument("--ms", type=int, default=0, dest="ms",
                        help="per-key press duration in ms (default: use C3 default)")
    parser.add_argument("--gap", type=int, default=0, dest="gap",
                        help="gap between characters in ms (default: use C3 default)")
    args = parser.parse_args()

    host = args.ip if args.ip else args.host
    base = f"http://{host}"

    try:
        r = requests.get(f"{base}/status", timeout=5)
        r.raise_for_status()
        st = r.json()
        print(f"C3 status: {st}")
        if not st.get("connected"):
            print("WARN: C3 reports BLE not connected — typing will not reach a host")
    except Exception as e:
        print(f"FAIL: cannot reach C3 at {base} ({e})")
        return 1

    params = {"s": args.text}
    if args.ms > 0:
        params["ms"] = args.ms
    if args.gap > 0:
        params["gap"] = args.gap

    try:
        r = requests.post(f"{base}/text", params=params, timeout=5)
        r.raise_for_status()
        print(f"OK: {r.text} '{args.text}'")
    except Exception as e:
        print(f"FAIL: /text request failed ({e})")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
