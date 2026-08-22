"""AT-Node rust-s3 — LCD1602 "Hello World" demo over PCF8574 I2C backpack.

Drives the LCD1602 in 4-bit mode through AT+I2C_W on the AT console.
PCF8574 has no register file: every written byte is the output port, so
each port state is sent as a doubled byte (I2C_W always appends a data
byte; writing <v>,<v> leaves the port at v).

Backpack wiring (standard): P0=RS P1=RW P2=EN P3=backlight P4..7=D4..7.

Usage: uv run python esp32/rust/tools/lcd_hello.py [--port /dev/ttyACM0]
                                                [--addr 0x27]
"""

import argparse
import time

import serial

RS = 0x01
EN = 0x04
BL = 0x08  # backlight (active high on this backpack)


class Lcd:
    def __init__(self, ser, addr):
        self.ser = ser
        self.addr = addr

    def _write_port(self, v):
        # [v, v]: I2C_W = <addr>,<first>,<second...>; double write pins
        # the final port state to v.
        self.ser.write(f"AT+I2C_W={self.addr:#04x},{v:#04x},{v:#04x}\r\n".encode())
        deadline = time.time() + 2.0
        buf = b""
        while time.time() < deadline:
            buf += self.ser.read(256)
            if b"OK" in buf or b"ERROR" in buf:
                if b"ERROR" in buf:
                    raise RuntimeError(f"I2C_W failed: {buf!r}")
                return
        raise TimeoutError("I2C_W no response")

    def _pulse(self, v):
        self._write_port(v | EN)
        self._write_port(v & ~EN)

    def _nibble(self, nibble, rs):
        self._pulse(BL | rs | (nibble << 4))

    def cmd(self, byte):
        self._nibble(byte >> 4, 0)
        self._nibble(byte & 0x0F, 0)
        time.sleep(0.002)

    def data(self, byte):
        self._nibble(byte >> 4, RS)
        self._nibble(byte & 0x0F, RS)

    def init(self):
        time.sleep(0.05)
        # 8-bit mode thrice, then switch to 4-bit (HD44780 datasheet).
        for _ in range(3):
            self._pulse(BL | 0x30)
            time.sleep(0.005)
        self._pulse(BL | 0x20)
        time.sleep(0.005)
        self.cmd(0x28)  # 4-bit, 2 lines, 5x8
        self.cmd(0x0C)  # display on, cursor off, no blink
        self.cmd(0x01)  # clear (needs ~2 ms)
        time.sleep(0.005)
        self.cmd(0x06)  # entry: increment, no shift

    def text(self, s, addr=0x00):
        self.cmd(0x80 | addr)
        for ch in s[:16]:
            self.data(ord(ch))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--addr", default="0x27")
    ap.add_argument("--line2", default="AT-Node rust-s3")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.3)
    ser.reset_input_buffer()
    lcd = Lcd(ser, int(args.addr, 0))
    print("init...")
    lcd.init()
    lcd.text("Hello World", 0x00)
    if args.line2:
        lcd.text(args.line2, 0x40)
    print("done: LCD should show 'Hello World' /", args.line2)


if __name__ == "__main__":
    main()
