#!/usr/bin/env python3
r"""ESP32 AT Node comprehensive test flow.

  Fixed test script covering all implemented features:
  - HTTP status/help pages
  - Raw AT commands
  - Keyboard (tap, text, key)
  - GPIO, ADC, I2C, IR
  - MQTT (config, connect, publish)
  - WiFi config, MQTT CA fingerprint

  Prerequisites:
  - ESP32 AT Node connected to WiFi (IP or mDNS hostname)
  - CH582 dongle on COM4 for BLE keyboard verification (optional)
  - Local MQTT broker on 192.168.1.7:1883/8883 (optional)

  Usage:
      uv run python tools/test_esp32_at_node_full.py --ip 192.168.1.27
      uv run python tools/test_esp32_at_node_full.py --ip atnodeesp-5688.local
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


def section(title):
    print(f"\n=== {title} ===")


def main():
    parser = argparse.ArgumentParser(description="ESP32 AT Node comprehensive test")
    parser.add_argument("--ip", default=DEFAULT_HOST, help="ESP32 IP or mDNS hostname")
    parser.add_argument("--dongle-port", default="COM4", help="CH582 dongle port (optional)")
    parser.add_argument("--skip-dongle", action="store_true", help="Skip dongle BLE tests")
    parser.add_argument("--skip-mqtt", action="store_true", help="Skip MQTT tests")
    args = parser.parse_args()

    base = f"http://{args.ip}/at-node"
    ok = True

    print(f"Testing ESP32 AT Node at {base}")
    print(f"Dongle port: {args.dongle_port} (skip: {args.skip_dongle})")
    print(f"MQTT tests: {not args.skip_mqtt}")

    # ============================================================
    section("Device Discovery & Status")
    # ============================================================

    # Root redirect
    try:
        r = requests.get(f"http://{args.ip}/", timeout=5, allow_redirects=False)
        ok &= check("root redirects to /at-node/status",
                    r.status_code == 302 and r.headers.get("Location") == "/at-node/status")
    except Exception as e:
        ok &= check("root redirect", False)
        print(f"    error: {e}")

    # Status HTML
    try:
        r = requests.get(f"{base}/status", timeout=5)
        r.raise_for_status()
        ok &= check("/at-node/status (HTML)", r.status_code == 200 and "AT-Node Status" in r.text)
    except Exception as e:
        ok &= check("/at-node/status (HTML)", False)
        print(f"    error: {e}")

    # Status JSON
    try:
        r = requests.get(f"{base}/cmd/status", timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("/at-node/cmd/status (JSON)",
                    "device" in j and "hostname" in j and "ip" in j)
        print(f"    {j}")
    except Exception as e:
        ok &= check("/at-node/cmd/status (JSON)", False)
        print(f"    error: {e}")

    # Help HTML
    try:
        r = requests.get(f"{base}/help", timeout=5)
        r.raise_for_status()
        ok &= check("/at-node/help (HTML)",
                    r.status_code == 200 and "AT-Node HTTP API" in r.text and "mDNS" in r.text)
    except Exception as e:
        ok &= check("/at-node/help (HTML)", False)
        print(f"    error: {e}")

    # ============================================================
    section("Raw AT Commands")
    # ============================================================

    try:
        r = requests.post(f"{base}/at", data="AT", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT", j.get("ok") and j.get("response") == "OK")
    except Exception as e:
        ok &= check("AT", False)
        print(f"    error: {e}")

    try:
        r = requests.post(f"{base}/at", data="AT+STATUS", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        # AT+STATUS returns a status line, not just OK
        ok &= check("AT+STATUS", j.get("ok") or j.get("response", "").startswith("+"))
    except Exception as e:
        ok &= check("AT+STATUS", False)
        print(f"    error: {e}")

    # ============================================================
    section("Keyboard")
    # ============================================================

    # Check BLE connection first
    st = requests.get(f"{base}/cmd/status", timeout=5).json()
    ble_connected = st.get("connected", False)

    if ble_connected:
        try:
            r = requests.post(f"{base}/cmd/keyboard/tap",
                              params={"mods": 0, "k": 4, "ms": 100}, timeout=5)
            r.raise_for_status()
            j = r.json()
            ok &= check("keyboard/tap", j.get("ok"))
        except Exception as e:
            ok &= check("keyboard/tap", False)
            print(f"    error: {e}")

        try:
            r = requests.post(f"{base}/cmd/keyboard/text",
                              params={"s": "hello", "ms": 50, "gap": 50}, timeout=5)
            r.raise_for_status()
            j = r.json()
            ok &= check("keyboard/text", j.get("ok") and j.get("queued"))
        except Exception as e:
            ok &= check("keyboard/text", False)
            print(f"    error: {e}")

        try:
            r = requests.post(f"{base}/at", data="AT+TAP=100,0,5", timeout=5,
                              headers={"Content-Type": "text/plain"})
            r.raise_for_status()
            j = r.json()
            ok &= check("AT+TAP", j.get("ok"))
        except Exception as e:
            ok &= check("AT+TAP", False)
            print(f"    error: {e}")
    else:
        print("  [SKIP] keyboard tests (BLE not connected)")

    # ============================================================
    section("GPIO / ADC")
    # ============================================================

    try:
        r = requests.post(f"{base}/cmd/gpio/write",
                          params={"pin": 2, "level": 1}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("gpio/write", j.get("ok"))

        r = requests.post(f"{base}/cmd/gpio/read",
                          params={"pin": 2}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("gpio/read", j.get("ok") and j.get("level") == 1)
    except Exception as e:
        ok &= check("gpio", False)
        print(f"    error: {e}")

    try:
        r = requests.post(f"{base}/cmd/adc/read",
                          params={"ch": 0}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("adc/read", j.get("ok") and "mv" in j)
    except Exception as e:
        ok &= check("adc/read", False)
        print(f"    error: {e}")

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

    # ============================================================
    section("I2C")
    # ============================================================

    try:
        r = requests.post(f"{base}/cmd/i2c/scan", timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("i2c/scan", j.get("ok") and "devices" in j)
    except Exception as e:
        ok &= check("i2c/scan", False)
        print(f"    error: {e}")

    try:
        r = requests.post(f"{base}/at", data="AT+I2C_SCAN", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+I2C_SCAN", j.get("ok"))
    except Exception as e:
        ok &= check("AT+I2C_SCAN", False)
        print(f"    error: {e}")

    # ============================================================
    section("IR (RMT)")
    # ============================================================

    try:
        r = requests.post(f"{base}/cmd/ir/send",
                          params={"protocol": "NEC", "data": "0x807F00FF"}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("ir/send (NEC)", j.get("ok"))
    except Exception as e:
        ok &= check("ir/send (NEC)", False)
        print(f"    error: {e}")

    try:
        r = requests.post(f"{base}/at", data="AT+IR=NEC,0x807F00FF", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+IR=NEC", j.get("ok"))
    except Exception as e:
        ok &= check("AT+IR=NEC", False)
        print(f"    error: {e}")

    # ============================================================
    section("WiFi Configuration")
    # ============================================================

    try:
        r = requests.post(f"{base}/cmd/wifi/config",
                          params={"ssid": "2-1909", "pass": "szyt1909"}, timeout=5)
        r.raise_for_status()
        j = r.json()
        ok &= check("wifi/config", j.get("ok") and j.get("ssid") == "2-1909")
    except Exception as e:
        ok &= check("wifi/config", False)
        print(f"    error: {e}")

    try:
        r = requests.post(f"{base}/at", data="AT+WIFI=ssid,2-1909", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+WIFI=ssid", j.get("ok"))
    except Exception as e:
        ok &= check("AT+WIFI=ssid", False)
        print(f"    error: {e}")

    # ============================================================
    section("MQTT")
    # ============================================================

    if not args.skip_mqtt:
        try:
            r = requests.get(f"{base}/cmd/mqtt/status", timeout=5)
            r.raise_for_status()
            j = r.json()
            ok &= check("mqtt/status", "connected" in j)
        except Exception as e:
            ok &= check("mqtt/status", False)
            print(f"    error: {e}")

        try:
            r = requests.post(f"{base}/cmd/mqtt/config",
                              params={"broker": "192.168.1.7", "port": 1883}, timeout=5)
            r.raise_for_status()
            j = r.json()
            ok &= check("mqtt/config", j.get("ok"))
        except Exception as e:
            ok &= check("mqtt/config", False)
            print(f"    error: {e}")

        try:
            r = requests.post(f"{base}/cmd/mqtt/ca",
                              params={"fp": "e1827db813ffdbb6dea1d3da3c726271179b227293d2090c72beb02ea74002a9"}, timeout=5)
            r.raise_for_status()
            j = r.json()
            ok &= check("mqtt/ca (fingerprint)", j.get("ok"))
        except Exception as e:
            ok &= check("mqtt/ca (fingerprint)", False)
            print(f"    error: {e}")

        try:
            r = requests.post(f"{base}/at", data="AT+MQTT=broker,192.168.1.7", timeout=5,
                              headers={"Content-Type": "text/plain"})
            r.raise_for_status()
            j = r.json()
            ok &= check("AT+MQTT=broker", j.get("ok"))
        except Exception as e:
            ok &= check("AT+MQTT=broker", False)
            print(f"    error: {e}")

        # MQTT connect (queued)
        try:
            r = requests.post(f"{base}/cmd/mqtt/connect", timeout=5)
            r.raise_for_status()
            j = r.json()
            ok &= check("mqtt/connect (queued)", j.get("ok") and j.get("queued"))

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
            ok &= check("mqtt/publish", j.get("ok"))
        except Exception as e:
            ok &= check("mqtt/publish", False)
            print(f"    error: {e}")
    else:
        print("  [SKIP] MQTT tests")

    # ============================================================
    section("Device Configuration")
    # ============================================================

    try:
        r = requests.post(f"{base}/at", data="AT+CONF=testkey=testval", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+CONF set", j.get("ok"))
    except Exception as e:
        ok &= check("AT+CONF set", False)
        print(f"    error: {e}")

    # ============================================================
    section("AP Portal")
    # ============================================================

    try:
        r = requests.post(f"{base}/at", data="AT+AP=1", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+AP=1", j.get("ok"))

        # Check AP status
        time.sleep(1)
        st = requests.get(f"{base}/cmd/status", timeout=5).json()
        ok &= check("AP active", st.get("ap", False))

        # Stop AP
        r = requests.post(f"{base}/at", data="AT+AP=0", timeout=5,
                          headers={"Content-Type": "text/plain"})
        r.raise_for_status()
        j = r.json()
        ok &= check("AT+AP=0", j.get("ok"))
    except Exception as e:
        ok &= check("AP portal", False)
        print(f"    error: {e}")

    # ============================================================
    section("Summary")
    # ============================================================

    print("\n" + "=" * 40)
    print("ALL PASS" if ok else "SOME FAILED")
    print("=" * 40)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
