# at-node — CH582 BLE HID Keyboard Firmware

RISC-V BLE HID keyboard firmware for the WCH CH582F microcontroller. Self-contained project (all SDK dependencies consolidated locally).

## Project

- **MCU**: CH582F (RISC-V, rv32imac)
- **Stack**: BLE 4.2/5.0 via pre-compiled `libCH58xBLE.a`
- **Scheduler**: TMOS (cooperative, single-loop)
- **Entry point**: `software/APP/hidkbd_main.c` → `main()`
- **Device name**: "AT-Node" (set in `hidkbd.c`)
- **Repo**: https://github.com/rede97/at-node

## Commands

Build (from `software/obj/`):
```bash
cd software/obj && make --no-print-directory main-build
```
Requires MounRiver Studio toolchain on PATH (`riscv-none-embed-gcc`, `make`).

Clean:
```bash
cd software/obj && make clean
```

Output: `software/obj/HID_Keyboard.elf` / `.hex` / `.lst` / `.map`.

## Architecture

| Layer | Path | Role |
|-------|------|------|
| APP | `software/APP/` | Keyboard app logic — `hidkbd_main.c` (main init), `hidkbd.c` (key report, connection mgmt) |
| Profile | `software/Profile/` | BLE GATT services — HID Device, HID Keyboard, Battery, Device Info, Scan Params |
| HAL | `software/HAL/` | Hardware abstraction — MCU init, KEY polling, LED, RTC, SLEEP. `CONFIG.h` is master config |
| BLE Stack | `software/LIB/libCH58xBLE.a` | Pre-compiled BLE protocol stack (LL/HCI/L2CAP/SM/GATT/GAP/TMOS) |
| StdPeriphDriver | `software/StdPeriphDriver/` | Chip peripheral drivers (GPIO/UART/SPI/I2C/ADC/PWM/Timer/USB/Flash/Clocks). `libISP583.a` |
| RVMSIS | `software/RVMSIS/` | CMSIS-style core layer (NVIC/PFIC interrupt control) |
| Startup | `software/Startup/` | RISC-V reset vector + interrupt table (`startup_CH583.S`) |
| Ld | `software/Ld/` | Linker script (FLASH 448K, RAM 32K) |

**Data flow**: KEY polling → `hal_key_process_callback()` → `hidEmuSendKbdReport()` → `GATT_Notification()` → BLE radio.

## Conventions

- **C only** (no C++), standard gnu99.
- **TMOS tasks**: each module registers as a TMOS task; communicate via events (bit flags) and messages (queues). 3 tasks: HAL, HID Dev, HID Keyboard App.
- **Static GATT tables**: services define compile-time `gattAttribute_t[]` arrays.
- **Callback layers**: APP registers callbacks with HID Dev → HID Dev registers with BLE lib.
- **Config macros**: all tunables in `HAL/include/config.h` (BLE_MEMHEAP_SIZE, TX power, sleep, SNV, etc.) overridable via `-D` flags.
- **Already consolidated**: zero external path dependencies — everything is under `software/`.

## Notes

### EVT/ — Reference code
`EVT/` at the project root contains the original WCH CH583 EVT demo/examples for reference only. It is **not** compiled as part of the firmware build. The directory is gitignored to prevent accidental inclusion.

