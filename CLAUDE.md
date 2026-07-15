# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

CH582F firmware — BLE HID keyboard + USB CDC+HID composite. AT-command driven AI agent peripheral. Self-contained WCH SDK.

- **MCU**: CH582F (RISC-V rv32imac, 60 MHz, 448K Flash / 32K RAM)
- **BLE**: 4.2/5.0 via pre-compiled `libCH58xBLE.a`, TMOS cooperative scheduler
- **USB**: CDC ACM (PID=0x8040) + HID Keyboard composite (IAD)
- **Entry**: `software/APP/main.c` → `main()`
- **Device name**: "AT-Node" (BLE advertising)
- **License**: MIT

## Commands

```bash
# Build (requires MounRiver Studio toolchain: riscv-none-embed-gcc + make on PATH)
cd software/obj && make --no-print-directory main-build
# Output: at-node.elf / .hex / .lst / .map

# Encoding (WCH SDK was GB2312; converted to UTF-8 without BOM)
uv run python tools/check_encoding.py
uv run python tools/batch_utf8.py software   # GB2312 → UTF-8

# AT command test (device auto-detected by VID 0x1A86)
uv run python tools/test_at.py
```

## Architecture

### Layer stack

| Layer | Path | Role |
|-------|------|------|
| APP | `software/APP/` | main, AT parser + cmds, BLE/USB keyboard, USB CDC+HID, `config.h` |
| APP/HWS | `software/APP/HWS/` | Hardware services — KEY, LED, RTC, SLEEP, MCU init |
| APP/BLE | `software/APP/BLE/` | BLE GATT services — HID Dev, HID Keyboard, Battery, Device Info |
| BLE Stack | `software/LIB/libCH58xBLE.a` | Pre-compiled LL/HCI/L2CAP/SM/GATT/GAP/TMOS |
| StdPeriphDriver | `software/StdPeriphDriver/` | GPIO/UART/I2C/ADC/USB/Flash drivers + `libISP583.a` |
| RVMSIS | `software/RVMSIS/` | RISC-V core access (NVIC/PFIC) |
| Startup | `software/Startup/` | Reset vector + interrupt table |

### Recent refactoring (the split)

Old monolith `hidkbd.c` + `AT.c` was split into:

| New file | Purpose |
|----------|---------|
| `APP/hidkbd_ble.c` | BLE HID keyboard — advertising, connection, GATT report send via `ble_hid_dev_report()` |
| `APP/hidkbd_usb.c` | USB HID keyboard — `USB_HID_SendReport()` wrapper |
| `APP/include/hidkbd_common.h` | Keyboard routing layer API — mode select (USB/BLE/BOTH) + key send functions |
| `APP/at_parser.c` | AT parser — UART1 ring buffer + CDC poll, `=`/`,` tokenizer, dual channel response routing |
| `APP/at_parser.h` | AT parser header — `at_cmd_t` struct, `AT_Init()`, `AT_Response()`, `AT_Poll()` |
| `APP/at_cmds.c` | Command table + keyboard routing state machine (`kb_flush()` dispatches to BLE/USB) |
| `APP/main.c` | Entry point — inline `key_press()` (test code), init sequence, TMOS main loop |

Deleted: `APP/hidkbd.c`, `APP/hidkbd_main.c`, `HAL/` directory (moved to `APP/HWS/`)

### Keyboard routing layer (`hidkbd_common.h` + `at_cmds.c`)

```
AT command → at_cmds handler (at_cmd_KEY etc.)
  → kb_*() function (at_cmds.c)
    → kb_flush() checks kb_mode
      → kb_ble_send_report() [hidkbd_ble.c] → ble_hid_dev_report() [BLE/hiddev.c]
      → kb_usb_send_report() [hidkbd_usb.c] → USB_HID_SendReport() [usb_dev.c]
```

Modes: `KB_USB=1`, `KB_BLE=2`, `KB_BOTH=3`. Set via `AT+KB=USB|BLE|BOTH`.

### AT command system

- **Parser**: TMOS task polling at 10ms (`AT_EVENT`) — registered in `AT_Init()`
- **Channels**: UART1 (ring buffer, 512B) + CDC (`USB_CDC_Read()`) — both feed same line parser
- **Response**: routed back to originating channel; CDC responses echo input line first
- **Format**: `AT+CMD=arg1,arg2\r\n` → `\r\nOK\r\n` or `\r\nERROR\r\n`
- **Implemented commands**: `AT`, `AT+VER`, `AT+HELP`, `AT+KB`, `AT+KEY`, `AT+KEY_DOWN`, `AT+KEY_UP`, `AT+MOD`, `AT+ECHO`
- **Max line**: 256 chars (`AT_LINE_MAX`); will need 1024 for RAW IR

### USB endpoint allocation

| EP | Interface | Type | Size | Note |
|----|-----------|------|------|------|
| EP0 | — | Control | 64B | Enumeration |
| EP1 | CDC Data | BULK IN/OUT | 64B | AT command pipe |
| EP2 | HID Keyboard | Interrupt IN/OUT | 8B | Keys + LED |
| EP3 | CDC Comm | Interrupt IN | 8B | Serial state notify |

### Init sequence

```
CH58X_BLEInit → HAL_Init → AT_Init → HalKeyConfig → GAPRole_PeripheralInit
→ ble_hid_dev_init → ble_hid_emu_init → USB_Device_Setup → PFIC_EnableIRQ(USB_IRQn)
→ Main_Circulation() [while(1) TMOS_SystemProcess()]
```

### TMOS task registry

| Task | Registered in | File |
|------|--------------|------|
| HAL task | `HAL_Init()` | `APP/HWS/MCU.c` |
| HID Dev task | `ble_hid_dev_init()` | `APP/BLE/hiddev.c` |
| HID Emu task | `ble_hid_emu_init()` | `APP/hidkbd_ble.c` |
| AT task | `AT_Init()` | `APP/at_parser.c` |

## Critical constraints

- **USB + sleep mutually exclusive**: `HAL_SLEEP=TRUE` → USB disabled at compile time (`#if` in main.c). USB clock stops in sleep → enumeration lost (code 43).
- **`usb_dev.c` is WCH EVT copy**: `USB_DevTransProcess` is based on official `HID_CompliantDev/src/Main.c`. Don't rewrite it.
- **Key scanning in `main()`**, not BLE callback — works on USB without BLE paired.
- **GPIO_Pin_All init does NOT interfere with USB D+/D-** (PB10/11) on CH582F — confirmed.
- **All `.c/.h/.S` are UTF-8 without BOM**, ASCII comments only.
- **C only**, gnu99. Toolchain: `riscv-none-embed-gcc` (MounRiver Studio).
- **BLE SNV**: Flash at `0x77E00` (last 512B of Data Flash), 1 bonded device, new pairing overwrites.
- **`MEM_BUF[BLE_MEMHEAP_SIZE/4]`**: BLE heap at top of RAM, ≥6KB.
- **BLE init callbacks** in `MCU.c` `CH58X_BLEInit()` — `sleepCB` only registered when `HAL_SLEEP == TRUE`.
- **EVT/ directory**: WCH CH583 SDK reference code (gitignored, not compiled).
- **References**: `DESIGN.md` (memory layout, BLE bonding, USB constraints), `README.md` (features, pinout, roadmap), `REQUIREMENTS.md` (full specs, Chinese).
