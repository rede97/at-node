# at-node — BLE + USB HID Keyboard & CDC Firmware

CH582F RISC-V firmware — BLE HID keyboard + USB CDC+HID composite device. Self-contained SDK.

## Project

- **MCU**: CH582F (RISC-V rv32imac, 60 MHz, 448K Flash / 32K RAM)
- **BLE**: 4.2/5.0 via pre-compiled `libCH58xBLE.a`, TMOS scheduler
- **USB**: CDC ACM (PID=0x2107) + HID Keyboard composite (IAD)
- **Entry point**: `software/APP/main.c` → `main()`
- **Device name**: "AT-Node" (BLE advertising, set in `hidkbd_ble.c`)
- **Design notes**: see `DESIGN.md` for memory layout, BLE bonding, USB constraints

## Commands

Build:
```bash
cd software/obj && make --no-print-directory main-build          # kbd (default)
cd software/obj && make --no-print-directory main-build DONGLE=1 # dongle (after make clean)
cd software/obj && make --no-print-directory main-build MODE=DUAL # dual: AT+ROLE runtime switch
tools/ci/build_all.sh                                            # all three -> tools/ci/out/
```
Requires MounRiver Studio toolchain on PATH (`riscv-none-embed-gcc`, `make`) — `source env.sh`. Variant switch needs `make clean` first.

Encoding check:
```bash
uv run python tools/check_encoding.py
uv run python tools/batch_utf8.py software   # GB2312 → UTF-8
```

## Architecture

| Layer | Path | Role |
|-------|------|------|
| APP | `software/APP/` | main, BLE keyboard (`hidkbd_ble.c`), USB keyboard (`hidkbd_usb.c`), USB CDC+HID (`usb_dev.c`), AT parser+cmds |
| APP/HWS | `software/APP/HWS/` | Hardware services — core, LED, KEY, RTC, SLEEP. All `hws_` prefix. |
| APP/BLE | `software/APP/BLE/` | BLE stack init (`ble_stack.c`) + GATT services — HID Dev, HID Keyboard, Battery, Device Info |
| BLE Stack | `software/LIB/libCH58xBLE.a` | Pre-compiled LL/HCI/L2CAP/SM/GATT/GAP/TMOS |
| StdPeriphDriver | `software/StdPeriphDriver/` | GPIO/UART/I2C/ADC/USB/Flash drivers + `libISP583.a` |
| RVMSIS | `software/RVMSIS/` | RISC-V core access (NVIC/PFIC) |
| Startup | `software/Startup/` | Reset vector + interrupt table |

**USB endpoint allocation**:

| EP | Interface | Type | Size | Note |
|----|-----------|------|------|------|
| EP0 | — | Control | 64B | Enumeration |
| EP1 | CDC Data | BULK IN/OUT | 64B | AT command pipe |
| EP2 | HID Keyboard | Interrupt IN/OUT | 8B | Keys + LED |
| EP3 | CDC Comm | Interrupt IN | 8B | Serial state notify |

**Data flow**: KEY polling (`hws_key_config(key_press)` in main) → `kb_ble_send_report()` / `kb_usb_send_report()` → BLE GATT notification + `USB_HID_SendReport()` → EP2 IN.

## Conventions

- **C only**, gnu99. All `.c/.h/.S` are UTF-8 without BOM, ASCII comments only.
- **USB code is WCH EVT copy**: `usb_dev.c` USB_DevTransProcess is based on official `HID_CompliantDev/src/Main.c`. Don't rewrite it.
- **Key scanning in `main()`**, not in BLE callback — works on USB without BLE paired.
- **HWS_SLEEP=TRUE disables USB** — enforced at compile time in main.c. USB clock stops in sleep.
- **GPIO_Pin_All init does NOT interfere with USB D+/D-** — confirmed by BleInputStick.
- **Tools** under `tools/` use Python + `uv` venv.

## Notes

- `DESIGN.md` — memory layout, BLE callback registration, USB/low-power exclusion details.
- `EVT/` — WCH CH583 SDK reference code (gitignored, not compiled).
- `software/REQUIREMENTS.md` — feature requirements (Chinese).
