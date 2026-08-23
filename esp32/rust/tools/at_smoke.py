"""AT-Node rust-s3 R0/R1 hardware smoke test.

Drives the UART0 AT console (ESPLink port) through the R1 command set:
AT/VER/HELP, SET/GET/KEYS registry semantics (defaults, bool/int
validation, write-only secrets, unknown keys), LED commands, error
paths, and flash persistence across AT+RST (plus NVS=clear restore).

Note: opening the ESPLink port resets the board (DTR/RTS) — the script
waits for the boot banner before sending anything.

Usage: uv run python esp32/rust/tools/at_smoke.py [--port /dev/ttyACM0]
"""

import argparse
import json
import sys
import time

import serial

BANNER = "AT-Node rust-s3 ready"

failures = []


class Console:
    def __init__(self, port: str):
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.buf = b""

    def hard_reset(self):
        """Pulse EN via the ESPLink auto-reset circuit (RTS -> EN)."""
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.2)
        self.ser.rts = False

    def read_line(self, timeout: float = 5.0) -> str | None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                return line.decode(errors="replace").strip()
            chunk = self.ser.read(256)
            if chunk:
                self.buf += chunk
        return None

    def wait_banner(self, timeout: float = 10.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.read_line(timeout=deadline - time.time())
            if line and BANNER in line:
                # ROM/boot logs may still be streaming; wait for 1 s of
                # silence so they don't pollute the next command's reply.
                quiet_deadline = time.time() + 5.0
                while time.time() < quiet_deadline:
                    if self.read_line(timeout=1.0) is None:
                        return True
                return True
        return False

    def cmd(self, line: str, timeout: float = 5.0) -> list[str]:
        """Send one AT line; return response lines up to OK/ERROR."""
        # Board echos input; skip the echo of our own line.
        self.ser.reset_input_buffer()
        self.buf = b""
        self.ser.write(line.encode() + b"\r\n")
        out = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            got = self.read_line(timeout=deadline - time.time())
            if got is None:
                break
            if got == line:  # echo
                continue
            if got.startswith("\x1b"):  # interleaved WARN/INFO log line
                continue
            out.append(got)
            if got == "OK" or got.startswith("ERROR"):
                return out
        return out


def check(name: str, cond: bool, detail: str = ""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f"  ({detail})" if detail and not cond else ""))
    if not cond:
        failures.append(name)


def expect(name: str, resp: list[str], lines: list[str]):
    check(name, resp == lines, f"got {resp!r}, want {lines!r}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--slow", action="store_true",
                    help="include the ~30 s AT+I2C_SCAN check (bare board)")
    args = ap.parse_args()

    c = Console(args.port)
    print(f"port {args.port} open; resetting board and waiting for banner...")
    c.hard_reset()
    if not c.wait_banner():
        print("[FAIL] no boot banner")
        return 1
    print("banner ok")

    expect("AT", c.cmd("AT"), ["OK"])
    expect("AT+VER", c.cmd("AT+VER"), ["AT-Node v1.0 [rust-s3]", "OK"])

    help_resp = c.cmd("AT+HELP")
    check("AT+HELP", len(help_resp) > 3 and help_resp[-1] == "OK", repr(help_resp))

    resp = c.cmd("AT+GET=device.name")
    check(
        "GET device.name default",
        len(resp) == 2
        and resp[0].startswith("+GET:device.name=AT-Node-ESP-")
        and resp[1] == "OK",
        repr(resp),
    )
    def_name = resp[0].split("=", 1)[1] if len(resp) == 2 else "AT-Node-S3-????"

    expect("GET mqtt.port default", c.cmd("AT+GET=mqtt.port"),
           ["+GET:mqtt.port=8883", "OK"])
    expect("GET write-only secret", c.cmd("AT+GET=wifi.pass"),
           ["ERROR write-only"])
    expect("GET unknown key", c.cmd("AT+GET=nope"), ["ERROR unknown key"])

    expect("SET device.name", c.cmd("AT+SET=device.name=SmokeTest"), ["OK"])
    expect("GET device.name updated", c.cmd("AT+GET=device.name"),
           ["+GET:device.name=SmokeTest", "OK"])

    expect("SET bool true", c.cmd("AT+SET=mqtt.auto=true"), ["OK"])
    expect("GET bool normalized", c.cmd("AT+GET=mqtt.auto"),
           ["+GET:mqtt.auto=1", "OK"])
    expect("SET bool invalid", c.cmd("AT+SET=ble.auto=maybe"),
           ["ERROR bad value"])
    expect("SET int valid", c.cmd("AT+SET=mqtt.port=1883"), ["OK"])
    expect("SET int low", c.cmd("AT+SET=mqtt.port=0"), ["ERROR bad value"])
    expect("SET int high", c.cmd("AT+SET=mqtt.port=70000"),
           ["ERROR bad value"])
    expect("SET unknown key", c.cmd("AT+SET=foo=1"), ["ERROR unknown key"])
    expect("SET missing value sep", c.cmd("AT+SET=device.name"),
           ["ERROR bad args"])

    resp = c.cmd("AT+KEYS")
    ok = False
    if len(resp) == 2 and resp[0].startswith("+KEYS:") and resp[1] == "OK":
        try:
            keys = json.loads(resp[0][6:])
            by_key = {e["key"]: e for e in keys}
            ok = (
                by_key["device.name"]["value"] == "SmokeTest"
                and by_key["wifi.pass"].get("secret") is True
                and "value" not in by_key["wifi.pass"]
                and by_key["mqtt.port"]["value"] == "1883"
            )
        except (json.JSONDecodeError, KeyError) as e:
            ok = False
            print("  json error:", e)
    check("AT+KEYS json", ok, repr(resp))

    expect("LED rgb", c.cmd("AT+LED=255,0,0"), ["OK"])
    expect("LED hex", c.cmd("AT+LED=0x00,0xff,0"), ["OK"])
    expect("LED css hex", c.cmd("AT+LED=#0000FF"), ["OK"])
    expect("LED query custom", c.cmd("AT+LED?"), ["+LED:0,0,255,custom", "OK"])
    expect("LED off", c.cmd("AT+LED=off"), ["OK"])
    expect("LED query off", c.cmd("AT+LED?"), ["+LED:0,0,0,off", "OK"])
    expect("LED auto", c.cmd("AT+LED=auto"), ["OK"])
    resp = c.cmd("AT+LED?")
    check(
        "LED query auto",
        len(resp) == 2 and resp[0].startswith("+LED:") and resp[0].endswith(",auto")
        and resp[1] == "OK",
        repr(resp),
    )
    expect("LED bad hex", c.cmd("AT+LED=#FFF"), ["ERROR bad args"])
    expect("LED too few", c.cmd("AT+LED=1,2"), ["ERROR bad args"])
    expect("LED too many", c.cmd("AT+LED=1,2,3,4"), ["ERROR bad args"])
    expect("LED range", c.cmd("AT+LED=300,0,0"), ["ERROR bad args"])

    expect("unknown cmd", c.cmd("AT+XYZ"), ["ERROR unknown cmd"])
    expect("NVS bad sub", c.cmd("AT+NVS=foo"), ["ERROR bad args"])

    # --- R2: wifi (fields present; link state depends on lab creds) ----
    resp = c.cmd("AT+STATUS")
    check(
        "STATUS fields",
        len(resp) == 10
        and resp[0].startswith("device=")
        and any(l.startswith("wifi=") for l in resp)
        and any(l.startswith("ip=") for l in resp)
        and any(l.startswith("mac=") for l in resp)
        and any(l.startswith("mqtt=") for l in resp)
        and resp[-1] == "OK",
        repr(resp),
    )
    resp = c.cmd("AT+WIFI=status")
    check(
        "WIFI status",
        len(resp) == 2 and resp[0].startswith("+WIFI:") and resp[1] == "OK",
        repr(resp),
    )
    expect("WIFI bad sub", c.cmd("AT+WIFI=foo,bar"), ["ERROR bad args"])

    # --- R3: mqtt status surface (broker loop tested separately) -------
    resp = c.cmd("AT+MQTT=status")
    check(
        "MQTT status",
        len(resp) == 2
        and resp[0].startswith("+MQTT:")
        and ",auto=" in resp[0]
        and ",enabled=" in resp[0]
        and resp[1] == "OK",
        repr(resp),
    )
    expect("MQTT bad sub", c.cmd("AT+MQTT=foo"), ["ERROR bad args"])

    # --- R7: gpio/adc/i2c ----------------------------------------------
    expect("GPIO_W ok", c.cmd("AT+GPIO_W=21,1"), ["OK"])
    expect("GPIO_W low", c.cmd("AT+GPIO_W=21,0"), ["OK"])
    expect("GPIO_W strap pin", c.cmd("AT+GPIO_W=0,1"), ["ERROR unsafe pin"])
    expect("GPIO_W flash pin", c.cmd("AT+GPIO_W=26,1"), ["ERROR unsafe pin"])
    expect("GPIO_W uart pin", c.cmd("AT+GPIO_W=43,1"), ["ERROR unsafe pin"])
    expect("GPIO_W ws2812 pin", c.cmd("AT+GPIO_W=48,1"), ["ERROR unsafe pin"])
    expect("GPIO_W nonexistent", c.cmd("AT+GPIO_W=49,1"), ["ERROR unsafe pin"])
    resp = c.cmd("AT+GPIO_R=21")
    check("GPIO_R pullup", resp in (["+GPIO_R:0", "OK"], ["+GPIO_R:1", "OK"]), repr(resp))
    expect("GPIO_R bad pin", c.cmd("AT+GPIO_R=0"), ["ERROR bad pin"])

    resp = c.cmd("AT+ADC=0")
    check(
        "ADC ch0 mV",
        len(resp) == 2
        and resp[0].startswith("+ADC:")
        and resp[0][5:].isdigit()
        and resp[1] == "OK",
        repr(resp),
    )
    expect("ADC bad channel", c.cmd("AT+ADC=10"), ["ERROR bad channel"])

    # I2C: scan first (bit-bang, fast); NACK checks use an absent address.
    resp = c.cmd("AT+I2C_SCAN", timeout=10.0)
    present = []
    if len(resp) == 2 and resp[0].startswith("+I2C:") and resp[1] == "OK":
        present = resp[0][5:].split()
    check("I2C_SCAN ok", len(resp) == 2 and resp[1] == "OK", repr(resp))
    absent = next((f"0x{a:02X}" for a in range(0x08, 0x78)
                   if f"0x{a:02X}" not in present), "0x51")
    expect("I2C_R absent device", c.cmd(f"AT+I2C_R={absent},0,4"), ["ERROR i2c"])
    expect("I2C_W absent device", c.cmd(f"AT+I2C_W={absent},0,1,2,3"), ["ERROR i2c"])
    expect("I2C_R len 0", c.cmd(f"AT+I2C_R={absent},0,0"), ["ERROR bad args"])
    expect("I2C_R len 33", c.cmd(f"AT+I2C_R={absent},0,33"), ["ERROR bad args"])
    expect("I2C_R missing arg", c.cmd(f"AT+I2C_R={absent},0"), ["ERROR bad args"])

    # --- persistence across reset -------------------------------------
    expect("AT+RST", c.cmd("AT+RST"), ["OK"])
    check("reboot banner", c.wait_banner())
    expect("name persisted", c.cmd("AT+GET=device.name"),
           ["+GET:device.name=SmokeTest", "OK"])
    expect("port persisted", c.cmd("AT+GET=mqtt.port"),
           ["+GET:mqtt.port=1883", "OK"])
    expect("bool persisted", c.cmd("AT+GET=mqtt.auto"),
           ["+GET:mqtt.auto=1", "OK"])

    # --- NVS=clear restores defaults ----------------------------------
    expect("NVS clear", c.cmd("AT+NVS=clear"),
           ["NVS erased, reboot with AT+RST", "OK"])
    expect("AT+RST 2", c.cmd("AT+RST"), ["OK"])
    check("reboot banner 2", c.wait_banner())
    expect("name back to default", c.cmd("AT+GET=device.name"),
           [f"+GET:device.name={def_name}", "OK"])
    expect("port back to default", c.cmd("AT+GET=mqtt.port"),
           ["+GET:mqtt.port=8883", "OK"])

    print()
    if failures:
        print(f"SMOKE FAIL: {len(failures)} failed: {failures}")
        return 1
    print("SMOKE PASS: all checks green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
