# at-node — BLE + USB HID Keyboard & CDC Firmware

CH582F RISC-V firmware — BLE HID keyboard + USB CDC+HID composite, plus a BLE HID Host (receiver) role. Self-contained SDK.

**ESP32-C3 variant**: network-enabled AT Node with WiFi HTTP / MQTT control plane — see `esp32/PLAN.md` and `esp32/README.md`.

## Project

- **MCU**: CH582F (RISC-V rv32imac, 60 MHz, 448K Flash / 32K RAM)
- **BLE**: 4.2/5.0 via pre-compiled `libCH58xBLE.a`, TMOS scheduler
- **USB**: CDC ACM (PID=0x2107) + HID Keyboard composite (IAD)
- **Roles**: kbd (Peripheral keyboard) / dongle (Central receiver, forwards a BLE keyboard to USB) / dual (`AT+ROLE` runtime switch)
- **Entry point**: `ch582/APP/main.c` → `main()`
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
- **Two-board dev rig**: kbd board (test keyboard, inject keys via `AT+KEY`) + dongle board (receiver). `tools/test/test_dongle_loop.py` + `tools/test/test_dongle_hardening.py` drive both; `tools/ci/loop_test.sh` one-click build+flash+test.
- **ESP32-C3 variant**: `esp32/esp32_at_node/` — WiFi HTTP (`/at-node/*`) + MQTT + BLE HID keyboard, full AT command parity with CH582. Build with `esp32/esp32_at_node/build.ps1`.

## Commands

Build:
```bash
cd ch582/obj && make --no-print-directory main-build          # kbd (default)
cd ch582/obj && make --no-print-directory main-build MODE=KBD_MULTI # kbd_multi: 3-host keyboard
cd ch582/obj && make --no-print-directory main-build DONGLE=1 # dongle (after make clean)
cd ch582/obj && make --no-print-directory main-build MODE=DUAL # dual: single-mode keyboard + dongle (debug)
tools/ci/build_all.sh                                            # all variants -> tools/ci/out/
```
Requires MounRiver Studio toolchain on PATH (`riscv-none-embed-gcc`, `make`) — `source env.sh`. Variant switch needs `make clean` first. xPack/upstream GCC builds broken firmware (interrupt attr) — see `tools/ci/TOOLCHAIN.md`.

Encoding check:
```bash
uv run python tools/utils/batch_utf8.py ch582 --check
uv run python tools/utils/batch_utf8.py ch582   # GB2312 → UTF-8
```

AT test:
```bash
uv run python tools/test/test_at.py
uv run python tools/test/send_key.py 0x39 --mode BLE          # CapsLock via BLE
uv run python tools/test/send_key.py 0x04 --mode USB --seq "Hi"  # 'a' / text via KEY_SEQ
```

**输入注入规则(FIELD-NOTES F18)**:常规注入一律用 `AT+TAP`(原子按下+释放)
或 `AT+KEY_STR`/`AT+KEY_SEQ`(序列引擎自动配对 press/release);裸 `AT+KEY`
仅限修饰键按住等特殊场景,且必须显式补 `AT+KEY=0,0` 释放。卡住时止血:
补发 `AT+KEY=0,0` ×2-3。

Dongle loop test:
```bash
uv run python tools/test/test_dongle_loop.py          # two CH582 boards
uv run python tools/test/test_dongle_c3.py --dongle-port COM4 --c3-ip 192.168.1.27  # C3 keyboard
```

C3 typing:
```bash
uv run python tools/test/c3_type.py --ip 192.168.1.27 "Hello World"
uv run python tools/test/c3_type.py --ip 192.168.1.27 --ms 60 --gap 100 "Hello World"
```

Remote broker (MQTT + HTTP proxy for remote device access, see `tools/broker/README.md`):
```bash
uv run python tools/broker/atnode_broker.py serve          # MQTT broker only
uv run python tools/broker/atnode_broker.py serve --http   # + HTTP proxy :8080
uv run python tools/broker/atnode_broker.py client list    # list devices
```

## Architecture

### Layer stack

| Layer | Path | Role |
|-------|------|------|
| APP | `ch582/APP/` | main, BLE keyboard (`hidkbd_ble.c`), USB keyboard (`hidkbd_usb.c`), USB CDC+HID (`usb_dev.c`), AT parser+cmds, runtime role (`role.c`), role init dispatch (`ble_init.c`) |
| APP/HWS | `ch582/APP/HWS/` | Hardware services — core, LED, KEY, RTC, SLEEP. All `hws_` prefix. Peripheral drivers (GPIO/ADC/I2C) land here, macro-gated. |
| APP/BLE | `ch582/APP/BLE/` | BLE stack init (`ble_stack.c`) + GATT services (HID Dev, HID Keyboard, Battery, Device Info) + dongle receiver (`ble_dongle.c`, Central/HID host) |
| BLE Stack | `ch582/LIB/libCH58xBLE.a` | Pre-compiled LL/HCI/L2CAP/SM/GATT/GAP/TMOS |
| StdPeriphDriver | `ch582/StdPeriphDriver/` | GPIO/UART/I2C/ADC/USB/Flash drivers + `libISP583.a` |
| RVMSIS | `ch582/RVMSIS/` | RISC-V core access (NVIC/PFIC) |
| Startup | `ch582/Startup/` | Reset vector + interrupt table |

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
    → kb_flush() checks kb_target bitmask
      → kb_ble_send_report_slot() [hidkbd_ble.c] → ble_hid_dev_report() [BLE/ble_hid_dev.c]
      → kb_usb_send_report() [hidkbd_usb.c] → USB_HID_SendReport() [usb_dev.c]
```

Target bitmask: `KB_TGT_USB=0x01`, `KB_TGT_BLE1=0x02`, `KB_TGT_BLE2=0x04`, `KB_TGT_BLE3=0x08`. Set via `AT+DEV=USB|BLE|BLE1|BLE2|BLE3`.

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
- **Tools** under `tools/` use Python + `uv` venv. Layout: `broker/` (application services: remote MQTT+HTTP broker), `test/` (test scripts), `demo/` (demo/recon sketches), `utils/`, `ci/` — see `tools/README.md`.

## Notes

- `DESIGN.md` — design philosophy, memory layout, BLE callback registration, USB/low-power exclusion details.
- `ch582/USER-MANUAL.md` — AT 命令使用手册(命令/模式/参数/注意细节)。
- `ch582/FIELD-NOTES.md` — 实战坑录(F1–F19)。
- `esp32/PLAN.md` — ESP32-C3 AT Node network variant plan (E1–E7).
- `.pi/skills/esp32-windows/` — Windows/ESP32-C3 development pit list (pi skill).
- `.pi/skills/ch582-linux/` — Linux build/flash/test ops manual (pi skill).
- `EVT/` — WCH CH583 SDK reference code (gitignored, not compiled).
- `REQUIREMENTS.md` — feature requirements (Chinese).
- `ch582/POWER.md` — low-power design guide.
