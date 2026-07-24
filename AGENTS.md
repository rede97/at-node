# at-node — BLE + USB HID Keyboard & CDC Firmware

CH582F RISC-V firmware — BLE HID keyboard + USB CDC+HID composite, plus a BLE HID Host (receiver) role. Self-contained SDK.

**ESP32-C3 variant**: network-enabled AT Node with WiFi HTTP / MQTT control plane — see `esp32/PLAN.md` and `esp32/README.md`.

## Project

- **MCU**: CH582F (RISC-V rv32imac, 60 MHz, 448K Flash / 32K RAM)
- **BLE**: 4.2/5.0 via pre-compiled `libCH58xBLE.a`, TMOS scheduler
- **USB**: CDC ACM (PID=0x2107) + HID Keyboard composite (IAD)
- **Roles**: kbd (Peripheral keyboard) / dongle (Central receiver, forwards a BLE keyboard to USB) / dual (`AT+ROLE` runtime switch)
- **Entry point**: `software/APP/main.c` → `main()`
- **Device name**: "AT-Node" (BLE advertising, set in `hidkbd_ble.c`)
- **Design notes**: see `DESIGN.md` for memory layout, BLE bonding, USB constraints

## Firmware variants

| Variant | Build | Notes |
|---------|------|-------|
| kbd | `make main-build` (default) | Production keyboard (Peripheral, single-mode) |
| kbd_multi | `make main-build MODE=KBD_MULTI` | Multi-mode keyboard (Peripheral, 3 hosts, seamless `AT+DEV=<target>` switch: target=USB\|BLE1\|BLE2\|BLE3\|ALL) |
| dongle | `make main-build DONGLE=1` | BLE HID receiver (Central) — verified on two-board rig |
| dual | `make main-build MODE=DUAL` | Single-mode keyboard + dongle (debug only, `AT+ROLE=KBD\|DONGLE` runtime switch) |

`BLE_MODE` tri-state in `config.h`; `BLE_DONGLE` kept as normalized alias.

- **`AT+VER` role tag**: reports `AT-Node v1.0 [kbd|dongle]` (runtime role in DUAL) — distinguishes identical boards.
- **RAM budget**: kbd 19076 B (58%) / dongle 19740 B (60%) / dual ~21000 B (64%). `.highcode` (~8KB) is WCH RAM-resident code — untouchable.
- **Two-board dev rig**: kbd board (test keyboard, inject keys via `AT+KEY`) + dongle board (receiver). `tools/test_dongle_loop.py` + `tools/test_dongle_hardening.py` drive both; `tools/ci/loop_test.sh` one-click build+flash+test.
- **ESP32-C3 variant**: `esp32/esp32_at_node/` — WiFi HTTP (`/at-node/*`) + MQTT + BLE HID keyboard, full AT command parity with CH582. Build with `esp32/esp32_at_node/build.ps1`.

## Commands

Build:
```bash
cd software/obj && make --no-print-directory main-build          # kbd (default)
cd software/obj && make --no-print-directory main-build MODE=KBD_MULTI # kbd_multi: 3-host keyboard
cd software/obj && make --no-print-directory main-build DONGLE=1 # dongle (after make clean)
cd software/obj && make --no-print-directory main-build MODE=DUAL # dual: single-mode keyboard + dongle (debug)
tools/ci/build_all.sh                                            # all variants -> tools/ci/out/
```
Requires MounRiver Studio toolchain on PATH (`riscv-none-embed-gcc`, `make`) — `source env.sh`. Variant switch needs `make clean` first. xPack/upstream GCC builds broken firmware (interrupt attr) — see `tools/ci/TOOLCHAIN.md`.

Encoding check:
```bash
uv run python tools/check_encoding.py
uv run python tools/batch_utf8.py software   # GB2312 → UTF-8
```

AT test:
```bash
uv run python tools/test_at.py
uv run python tools/send_key.py 0x39 --mode BLE          # CapsLock via BLE
uv run python tools/send_key.py 0x04 --mode USB --seq "Hi"  # 'a' / text via KEY_SEQ
```

Dongle loop test:
```bash
uv run python tools/test_dongle_loop.py          # two CH582 boards
uv run python tools/test_dongle_c3.py --dongle-port COM4 --c3-ip 192.168.1.27  # C3 keyboard
```

C3 typing:
```bash
uv run python tools/c3_type.py --ip 192.168.1.27 "Hello World"
uv run python tools/c3_type.py --ip 192.168.1.27 --ms 60 --gap 100 "Hello World"
```

## Architecture

### Layer stack

| Layer | Path | Role |
|-------|------|------|
| APP | `software/APP/` | main, BLE keyboard (`hidkbd_ble.c`), USB keyboard (`hidkbd_usb.c`), USB CDC+HID (`usb_dev.c`), AT parser+cmds, runtime role (`role.c`), role init dispatch (`ble_init.c`) |
| APP/HWS | `software/APP/HWS/` | Hardware services — core, LED, KEY, RTC, SLEEP. All `hws_` prefix. Peripheral drivers (GPIO/ADC/I2C/IR) land here, macro-gated. |
| APP/BLE | `software/APP/BLE/` | BLE stack init (`ble_stack.c`) + GATT services (HID Dev, HID Keyboard, Battery, Device Info) + dongle receiver (`ble_dongle.c`, Central/HID host) |
| BLE Stack | `software/LIB/libCH58xBLE.a` | Pre-compiled LL/HCI/L2CAP/SM/GATT/GAP/TMOS |
| StdPeriphDriver | `software/StdPeriphDriver/` | GPIO/UART/I2C/ADC/USB/Flash drivers + `libISP583.a` |
| RVMSIS | `software/RVMSIS/` | RISC-V core access (NVIC/PFIC) |
| Startup | `software/Startup/` | Reset vector + interrupt table |

### USB endpoint allocation

| EP | Interface | Type | Size | Note |
|----|-----------|------|------|------|
| EP0 | — | Control | 64B | Enumeration |
| EP1 | CDC Data | BULK IN/OUT | 64B | AT command pipe |
| EP2 | HID Keyboard | Interrupt IN/OUT | 8B | Keys + LED |
| EP3 | CDC Comm | Interrupt IN | 8B | Serial state notify |

**Data flow**: KEY polling (`hws_key_config(key_press)` in main) → `kb_ble_send_report()` / `kb_usb_send_report()` → BLE GATT notification + `USB_HID_SendReport()` → EP2 IN.

### Keyboard routing layer (`hidkbd_common.h` + `at_cmds.c`)

```
AT command → at_cmds handler (at_cmd_KEY etc.)
  → kb_*() function (at_cmds.c)
    → kb_flush() checks kb_mode
      → kb_ble_send_report() [hidkbd_ble.c] → ble_hid_dev_report() [BLE/hiddev.c]
      → kb_usb_send_report() [hidkbd_usb.c] → USB_HID_SendReport() [usb_dev.c]
```

Modes: `KB_USB=1`, `KB_BLE=2`, `KB_BOTH=3`. Set via `AT+KB=USB|BLE|BOTH`.

### Init sequence (7 linear stages)

```
1. hws_platform_init()    — power, clock, GPIO, debug UART
2. ble_stack_init()       — BLE protocol stack (+ initializes TMOS scheduler)
3. hws_init(key_press)    — RTC, sleep, LED, KEY + callback, HWS TMOS task
4. at_init()              — AT command parser (UART1 + CDC, 10ms TMOS poll)
5. ble_peripheral_init()  — GAP role + HID Device + HID Emu + advertising
6. usb_init()             — USB composite (CDC+HID) OR sleep mode
7. main_loop()            — TMOS_SystemProcess() forever
```

Stage 2 MUST come before stage 3: `BLE_LibInit()` initializes TMOS,
and `hws_init()` calls `TMOS_ProcessEventRegister()` which requires TMOS.
USB and sleep are mutually exclusive (compile-time via `HWS_SLEEP`).

### TMOS task registry

| Task | Registered in | File |
|------|--------------|------|
| HWS task | `hws_init()` | `APP/HWS/hws_core.c` |
| AT task | `at_init()` → `AT_Init()` | `APP/at_parser.c` |
| HID Dev task | `ble_hid_dev_init()` | `APP/BLE/ble_hid_dev.c` |
| HID Emu task | `ble_hid_emu_init()` | `APP/hidkbd_ble.c` |
| Dongle task | `ble_dongle_init()` (when `BLE_DONGLE=TRUE`, replaces Peripheral) | `APP/BLE/ble_dongle.c` |

HWS periodic tasks (KEY poll, BLE calibration) are table-driven via
`hws_tasks[]` in `hws_core.c` — adding one is a one-line table entry;
LED self-schedules blink timing outside the table.

## Conventions

- **C only**, gnu99. All `.c/.h/.S` are UTF-8 without BOM, ASCII comments only.
- **USB code is WCH EVT copy**: `usb_dev.c` `USB_DevTransProcess` is based on official `HID_CompliantDev/src/Main.c`. Don't rewrite it.
- **Key scanning in `main()`**, not in BLE callback — works on USB without BLE paired.
- **HWS_SLEEP=TRUE disables USB** — enforced at compile time in `main.c`. USB clock stops in sleep.
- **GPIO_Pin_All init does NOT interfere with USB D+/D-** (PB10/11) — confirmed by BleInputStick.
- **Feature conflicts are compile errors**: `config.h` uses first-class macros (`USB_ENABLE`, `HWS_SLEEP`, `BLE_DONGLE`); invalid combos (#error): USB+sleep, dongle without USB.
- **BLE SNV**: Flash at `0x77E00` (last 512B of Data Flash), 1 bonded device, new pairing overwrites.
- **BLE heap**: `MEM_BUF[BLE_MEMHEAP_SIZE/4]` at top of RAM, default 5KB (hard floor 4KB, checked in `ble_stack_init`).
- **Tools** under `tools/` use Python + `uv` venv.

## Notes

- `DESIGN.md` — design philosophy, memory layout, BLE callback registration, USB/low-power exclusion details.
- `software/PLAN.md` — roadmap + milestone log (M1–M4 done; M5 = peripheral drivers; M6 = C3 keyboard bench).
- `esp32/PLAN.md` — ESP32-C3 AT Node network variant plan (E1–E7).
- `.pi/skills/esp32-windows/` — Windows/ESP32-C3 development pit list (pi skill).
- `.pi/skills/ch582-linux/` — Linux build/flash/test ops manual (pi skill).
- `EVT/` — WCH CH583 SDK reference code (gitignored, not compiled).
- `REQUIREMENTS.md` — feature requirements (Chinese).
- `software/POWER.md` — low-power design guide.
