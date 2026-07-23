#!/usr/bin/env python3
r"""ESP32 AT Node test harness.

  Tests the network-enabled ESP32-C3 AT Node over HTTP.
  Verifies /at-node/status, /at-node/at (raw AT), and keyboard endpoints.

  Usage:
      uv run python tools/test_esp32_at_node.py --ip 192.168.1.27
"""
import argparse
import sys
import time

import requests

DEFAULT_HOST = "192.168.1.27"


def check(name, condition):
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {name}")
    return condition


def main():
    parser = argparse.ArgumentParser(description="ESP32 AT Node test harness")
    parser.add_argument("--ip", default=DEFAULT_HOST, help="ESP32 IP or mDNS hostname")
    args = parser.parse_args()

    base = f"http://{args.ip}/at-node"
    ok = True

    print(f"Testing ESP32 AT Node at {base} ...")

    # status
    try:
        r = requests.get(f"{base}/status", timeout=5)
        r.raise_for_status()
        st = r.json()
        ok &= check("/at-node/status", "device" in st and "connected" in st)
        print(f"    {st}")
    except Exception as e:
        ok &= check("/at-node/status", False)
        print(f"    error: {e}")

    # raw AT
    try:
        r = requests.post(f"{base}/at", data="AT", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/at AT", j.get("ok") and j.get("response") == "OK")
    except Exception as e:
        ok &= check("/at-node/at AT", False)
        print(f"    error: {e}")

    # AT+TAP via raw AT
    try:
        r = requests.post(f"{base}/at", data="AT+TAP=100,0,4", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/at AT+TAP", j.get("ok"))
    except Exception as e:
        ok &= check("/at-node/at AT+TAP", False)
        print(f"    error: {e}")

    # keyboard/tap
    try:
        r = requests.post(f"{base}/cmd/keyboard/tap",
                          params={"mods": 0, "k": 5, "ms": 100}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/keyboard/tap", j.get("ok"))
    except Exception as e:
        ok &= check("/at-node/cmd/keyboard/tap", False)
        print(f"    error: {e}")

    # keyboard/text
    try:
        r = requests.post(f"{base}/cmd/keyboard/text",
                          params={"s": "hello", "ms": 50, "gap": 50}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/keyboard/text", j.get("ok") and j.get("queued"))
    except Exception as e:
        ok &= check("/at-node/cmd/keyboard/text", False)
        print(f"    error: {e}")

    # config set/get
    try:
        r = requests.post(f"{base}/at", data="AT+CONF=testkey=testval", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+CONF set", j.get("ok"))
    except Exception as e:
        ok &= check("AT+CONF set", False)
        print(f"    error: {e}")

    # GPIO write/read
    try:
        r = requests.post(f"{base}/cmd/gpio/write",
                          params={"pin": 2, "level": 1}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/gpio/write", j.get("ok"))

        r = requests.post(f"{base}/cmd/gpio/read",
                          params={"pin": 2}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/gpio/read", j.get("ok") and j.get("level") == 1)
    except Exception as e:
        ok &= check("/at-node/cmd/gpio", False)
        print(f"    error: {e}")

    # ADC
    try:
        r = requests.post(f"{base}/cmd/adc/read",
                          params={"ch": 0}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/adc/read", j.get("ok") and "mv" in j)
    except Exception as e:
        ok &= check("/at-node/cmd/adc/read", False)
        print(f"    error: {e}")

    # raw AT GPIO/ADC
    try:
        r = requests.post(f"{base}/at", data="AT+GPIO_W=2,0", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+GPIO_W", j.get("ok"))

        r = requests.post(f"{base}/at", data="AT+GPIO_R=2", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+GPIO_R", j.get("ok") and j.get("response", "").startswith("+GPIO_R:"))

        r = requests.post(f"{base}/at", data="AT+ADC=0", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+ADC", j.get("ok") and j.get("response", "").startswith("+ADC:"))
    except Exception as e:
        ok &= check("AT+GPIO/ADC", False)
        print(f"    error: {e}")

    # I2C scan
    try:
        r = requests.post(f"{base}/cmd/i2c/scan", timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/i2c/scan", j.get("ok") and "devices" in j)
    except Exception as e:
        ok &= check("/at-node/cmd/i2c/scan", False)
        print(f"    error: {e}")

    # raw AT I2C scan
    try:
        r = requests.post(f"{base}/at", data="AT+I2C_SCAN", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+I2C_SCAN", j.get("ok"))
    except Exception as e:
        ok &= check("AT+I2C_SCAN", False)
        print(f"    error: {e}")

    # IR send
    try:
        r = requests.post(f"{base}/cmd/ir/send",
                          params={"protocol": "NEC", "data": "0x807F00FF"}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/ir/send", j.get("ok"))
    except Exception as e:
        ok &= check("/at-node/cmd/ir/send", False)
        print(f"    error: {e}")

    # raw AT IR
    try:
        r = requests.post(f"{base}/at", data="AT+IR=NEC,0x807F00FF", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+IR=NEC", j.get("ok"))
    except Exception as e:
        ok &= check("AT+IR=NEC", False)
        print(f"    error: {e}")

    # MQTT status
    try:
        r = requests.get(f"{base}/cmd/mqtt/status", timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/mqtt/status", "connected" in j)
    except Exception as e:
        ok &= check("/at-node/cmd/mqtt/status", False)
        print(f"    error: {e}")

    # MQTT config
    try:
        r = requests.post(f"{base}/cmd/mqtt/config",
                          params={"broker": "192.168.1.7", "port": 1883}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/mqtt/config", j.get("ok"))
    except Exception as e:
        ok &= check("/at-node/cmd/mqtt/config", False)
        print(f"    error: {e}")

    # MQTT connect (queued, check status after delay)
    try:
        r = requests.post(f"{base}/cmd/mqtt/connect", timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/mqtt/connect", j.get("ok") and j.get("queued"))

        time.sleep(3)
        r = requests.get(f"{base}/cmd/mqtt/status", timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("MQTT connected", j.get("connected"))
    except Exception as e:
        ok &= check("MQTT connect", False)
        print(f"    error: {e}")

    # MQTT publish
    try:
        r = requests.post(f"{base}/cmd/mqtt/publish",
                          params={"topic": "test/topic", "msg": "hello"}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/mqtt/publish", j.get("ok"))
    except Exception as e:
        ok &= check("/at-node/cmd/mqtt/publish", False)
        print(f"    error: {e}")

    print("\nALL PASS" if ok else "\nSOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
