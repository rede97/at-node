# ATNode — Multi-platform AI Agent I/O Node

**ATNode** is a family of AT-command firmware variants — the physical hands & feet of AI agents.
One command semantics across platforms; agents reuse scripts, only the transport changes.

## Platform matrix

| Series | Variant dir | Chip | Stack | Status |
|---|---|---|---|---|
| WCH BLE | `wchble/mr2/` | CH582F | MounRiver Studio 2 project, bare-metal + TMOS + precompiled BLE lib | ✅ Active |
| ESP32 | `esp32/arduino/` | ESP32-C3, classic ESP32 (NOT S3) | Arduino-ESP32 | ✅ Active |
| ESP32 | `esp32/zephyr/` | ESP32-S3 (nanoESP32-S3 N8R8) & PSRAM chips | Zephyr (native BLE host) | ✅ Active |
| Nordic | `nordic/zephyr/` | nRF52840 | Zephyr (nRF Connect SDK) | 📋 TODO placeholder |

- Cross-hardware requirement differences: **final decisions only** in `REQUIREMENTS.md` §4, pointing to platform docs.
- Per-chip hardware info & issue records live in the platform dirs (`wchble/mr2/HARDWARE.md`, `wchble/mr2/FIELD-NOTES.md`, `esp32/COMPAT_REPORT.md`).

## WCH BLE — CH582 (`wchble/mr2/`, "MR2" = MounRiver Studio 2 project)

- **MCU**: CH582F (RISC-V rv32imac, 60 MHz, 448K Flash / 32K RAM) — specs/pinout/USB endpoints: `wchble/mr2/HARDWARE.md`
- **BLE**: 4.2/5.0 via pre-compiled `LIB/libCH58xBLE.a`, TMOS scheduler
- **USB**: CDC ACM (PID=0x2107) + HID Keyboard composite (IAD)
- **Roles**: kbd (Peripheral keyboard) / dongle (Central receiver, forwards a BLE keyboard to USB) / dual (`AT+ROLE` runtime switch)
- **Entry point**: `wchble/mr2/APP/main.c` → `main()`
- **Device name**: "AT-Node" (BLE advertising, set in `hidkbd_ble.c`)

### Firmware variants

|Variant|Build|Notes|
|---|---|---|
|kbd|`make main-build` (default)|Production keyboard (Peripheral, single-mode)|
|kbd_multi|`make main-build MODE=KBD_MULTI`|Multi-mode keyboard (Peripheral, 3 hosts, seamless `AT+DEV=<target>` switch: target=USB\|BLE1\|BLE2\|BLE3\|ALL)|
|dongle|`make main-build DONGLE=1`|BLE HID receiver (Central) — verified on two-board rig|
|dual|`make main-build MODE=DUAL`|Single-mode keyboard + dongle (debug only, `AT+ROLE=KBD\|DONGLE` runtime switch)|

`BLE_MODE` tri-state in `config.h`; `BLE_DONGLE` kept as normalized alias.

- **`AT+VER` role tag**: reports `AT-Node v1.0 [kbd|dongle]` (runtime role in DUAL) — distinguishes identical boards.
- **RAM budget**: kbd 19076 B (58%) / dongle 19740 B (60%) / dual ~21000 B (64%). `.highcode` (~8KB) is WCH RAM-resident code — untouchable.
- **Two-board dev rig**: kbd board (test keyboard, inject keys via `AT+KEY`) + dongle board (receiver). `tools/test/test_dongle_loop.py` + `tools/test/test_dongle_hardening.py` drive both; `tools/ci/loop_test.sh` one-click build+flash+test.

### Commands (CH582)

Build:
```bash
cd wchble/mr2/obj && make --no-print-directory main-build          # kbd (default)
cd wchble/mr2/obj && make --no-print-directory main-build MODE=KBD_MULTI # kbd_multi: 3-host keyboard
cd wchble/mr2/obj && make --no-print-directory main-build DONGLE=1 # dongle (after make clean)
cd wchble/mr2/obj && make --no-print-directory main-build MODE=DUAL # dual (debug)
tools/ci/build_all.sh                                            # all variants -> tools/ci/out/
```
Requires MounRiver Studio toolchain on PATH (`riscv-none-embed-gcc`, `make`) — `source env.sh`. Variant switch needs `make clean` first. xPack/upstream GCC builds broken firmware (interrupt attr) — see `tools/ci/TOOLCHAIN.md`.

Encoding check:
```bash
uv run python tools/utils/batch_utf8.py wchble/mr2 --check
uv run python tools/utils/batch_utf8.py wchble/mr2   # GB2312 → UTF-8
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

### Architecture (CH582)

#### Layer stack

|Layer|Path|Role|
|---|---|---|
|APP|`wchble/mr2/APP/`|main, BLE keyboard (`hidkbd_ble.c`), USB keyboard (`hidkbd_usb.c`), USB CDC+HID (`usb_dev.c`), AT parser+cmds, runtime role (`role.c`), role init dispatch (`ble_init.c`)|
|APP/HWS|`wchble/mr2/APP/HWS/`|Hardware services — core, LED, KEY, RTC, SLEEP. All `hws_` prefix. Peripheral drivers (GPIO/ADC/I2C) land here, macro-gated.|
|APP/BLE|`wchble/mr2/APP/BLE/`|BLE stack init (`ble_stack.c`) + GATT services (HID Dev, HID Keyboard, Battery, Device Info) + dongle receiver (`ble_dongle.c`, Central/HID host)|
|BLE Stack|`wchble/mr2/LIB/libCH58xBLE.a`|Pre-compiled LL/HCI/L2CAP/SM/GATT/GAP/TMOS|
|StdPeriphDriver|`wchble/mr2/StdPeriphDriver/`|GPIO/UART/I2C/ADC/USB/Flash drivers + `libISP583.a`|
|RVMSIS|`wchble/mr2/RVMSIS/`|RISC-V core access (NVIC/PFIC)|
|Startup|`wchble/mr2/Startup/`|Reset vector + interrupt table|

#### Keyboard routing layer (`hidkbd_common.h` + `at_cmds.c`)

```
AT command → at_cmds handler (at_cmd_KEY etc.)
  → kb_*() function (at_cmds.c)
    → kb_flush() checks kb_target bitmask
      → kb_ble_send_report_slot() [hidkbd_ble.c] → ble_hid_dev_report() [BLE/ble_hid_dev.c]
      → kb_usb_send_report() [hidkbd_usb.c] → USB_HID_SendReport() [usb_dev.c]
```

Target bitmask: `KB_TGT_USB=0x01`, `KB_TGT_BLE1=0x02`, `KB_TGT_BLE2=0x04`, `KB_TGT_BLE3=0x08`. Set via `AT+DEV=USB|BLE|BLE1|BLE2|BLE3`.

#### Init sequence (7 linear stages)

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

#### TMOS task registry

|Task|Registered in|File|
|---|---|---|
|HWS task|`hws_init()`|`APP/HWS/hws_core.c`|
|AT task|`at_init()` → `AT_Init()`|`APP/at_parser.c`|
|HID Dev task|`ble_hid_dev_init()`|`APP/BLE/ble_hid_dev.c`|
|HID Emu task|`ble_hid_emu_init()`|`APP/hidkbd_ble.c`|
|Dongle task|`ble_dongle_init()` (when `BLE_DONGLE=TRUE`, replaces Peripheral)|`APP/BLE/ble_dongle.c`|

HWS periodic tasks (KEY poll, BLE calibration) are table-driven via
`hws_tasks[]` in `hws_core.c` — adding one is a one-line table entry;
LED self-schedules blink timing outside the table.

## ESP32 — Arduino variant (`esp32/arduino/`)

Network-enabled AT Node with WiFi HTTP (`/at-node/*`) / MQTT control plane — see
`esp32/arduino/PLAN.md` and `esp32/arduino/README.md`. Full AT command parity with CH582.

- **Chips**: ESP32-C3 (verified), classic ESP32 (verified 2026-08-15; GPIO6-11 are flash lines — every pin #define must be chip-conditional, enforced by a compile-time target gate in `features.h`). **ESP32-S3 NOT supported on Arduino** — final decision + root cause: `esp32/COMPAT_REPORT.md`; S3 goes to `esp32/zephyr/` (TODO).
- **Sketch**: `arduino.ino` (must match dir name). Web UI is a gzipped single-page app built from `esp32/arduino/web/` (Bun project: `cd esp32/arduino/web && bun run build` → `web_page.h`), served from flash in one response; all dynamic content via JSON `/at-node/cmd/*`.
- **Config**: all persistent config goes through one registry (`config_set/get/list`): `AT+SET=<key>=<val>` / `AT+GET=<key>` / `AT+KEYS` on serial, `/at-node/cmd/config` over HTTP, `config/set|get|list` over MQTT; key space `device.*`, `wifi.*`, `mqtt.*`, `http.*`, `ble.*`, `rathole.enable`, `tunnel.1.*`. Legacy commands/endpoints are aliases.
- **Build/flash — agents default to the board wrappers**:
  - `esp32/arduino/build-c3.ps1 -Port <COM>` — ESP32-C3 SuperMini (fqbn pins `CDCOnBoot=cdc`)
  - `esp32/arduino/build-esp32.ps1 -Port <COM>` — classic ESP32
  - Both wrap `build.ps1 -Board c3|esp32`. Never flash with bare `arduino-cli`/IDE defaults: on C3 a missing `CDCOnBoot=cdc` silently routes `Serial` to UART0 pads — native-USB COM shows only the ROM boot log, AT dead, while WiFi/HTTP keep working. Symptom → reflash with build-c3.ps1, do not debug the sketch.
  - Variants: `-Variant full|base|remoter|rathole` (feature macros in `features.h`); `base` = no tunnel/no LAN HTTP, `rathole` = tunnel-only test unit (I2C off → GPIO8 breathing liveness LED).
- **Ability**: reported via `AT+ABILITY` / `/at-node/cmd/ability`; the SPA hides tabs for disabled features.
- **WiFi watchdog**: boot connect has only a 30s window and the driver does not reliably re-associate — loop() retries `WiFi.begin` every 15s while down and `wifi_services_up()` brings up mDNS/HTTP whenever the link comes up. A board "off-network after flashing" is almost always the wrong-fqbn flash above, not WiFi config; reflash, then watch serial for `WiFi reconnecting...` → `WiFi connected` self-recovery.

C3 typing:
```bash
uv run python tools/test/c3_type.py --ip 192.168.1.27 "Hello World"
uv run python tools/test/c3_type.py --ip 192.168.1.27 --ms 60 --gap 100 "Hello World"
```

## ESP32 — Zephyr variant (`esp32/zephyr/`)

ESP32-S3 AT Node on Zephyr: WiFi HTTP + MQTT(TLS) control planes, BLE HID + USB HID
keyboard (AT+DEV routing), settings/NVS config registry, GPIO/ADC/I2C, WS2812 status
LED. Shares the Arduino variant's web SPA (`../arduino/web_page.h`, included
unmodified). Build: `esp32/zephyr/tools/build.sh [build|pristine|flash]`
(board `nano_esp32s3/esp32s3/procpu`, needs `~/zephyrproject` + zephyr-sdk on PATH).
Full docs (AT set, config keys, deltas vs Arduino, memory budget): `esp32/zephyr/README.md`.
Reference/pit list: `~/zephyrproject/apps/nano_esp32s3_demo/docs/DEBUGGING.md`.

## Remote broker (MQTT + HTTP proxy for remote device access)

Step-by-step runbook: `tools/broker/GET_START.md`; full reference: `tools/broker/README.md`.
```bash
uv run python tools/broker/atnode_broker.py serve          # MQTT broker only
uv run python tools/broker/atnode_broker.py serve --http   # + HTTP proxy :8080
uv run python tools/broker/atnode_broker.py client list    # list devices
```

> Broker is a workspace member of the root project. Run `uv sync --all-packages` once from the repo root; all broker commands run from the project root.
>
> **Server persistence pitfall**: the broker runs as a `systemd --user` service. After `deploy install`, verify `loginctl show-user $USER -p Linger` is `yes`; otherwise the service stops when the SSH session ends. Fix once with `sudo loginctl enable-linger $USER`, then `systemctl --user restart atnode-broker`.

## Conventions

- **CH582: C only**, gnu99. All `.c/.h/.S` are UTF-8 without BOM, ASCII comments only.
- **USB code is WCH EVT copy**: `usb_dev.c` `USB_DevTransProcess` is based on official `HID_CompliantDev/src/Main.c`. Don't rewrite it.
- **Key scanning in `main()`**, not in BLE callback — works on USB without BLE paired.
- **HWS_SLEEP=TRUE disables USB** — enforced at compile time in `main.c`. USB clock stops in sleep.
- **Feature conflicts are compile errors**: `config.h` uses first-class macros (`USB_ENABLE`, `HWS_SLEEP`, `BLE_DONGLE`); invalid combos (#error): USB+sleep, dongle without USB.
- **BLE SNV (CH582)**: Flash at `0x77E00` (last 512B of Data Flash), 1 bonded device, new pairing overwrites.
- **BLE heap (CH582)**: `MEM_BUF[BLE_MEMHEAP_SIZE/4]` at top of RAM, default 5KB (hard floor 4KB, checked in `ble_stack_init`).
- **Tools** under `tools/` use Python + `uv` venv. Layout: `broker/`, `test/`, `demo/`, `utils/`, `ci/` — see `tools/README.md`.

## Notes

- `REQUIREMENTS.md` — consolidated requirements by platform; cross-hardware decision registry §4.
- `wchble/mr2/USER-MANUAL.md` — AT 命令使用手册(CH582 命令/模式/参数/注意细节)。
- `wchble/mr2/HARDWARE.md` — CH582F 硬件规格/引脚/USB 端点/硬件约束。
- `wchble/mr2/DESIGN.md` — CH582 设计哲学、内存布局、BLE 回调注册、USB/低功耗互斥。
- `wchble/mr2/FIELD-NOTES.md` — CH582 实战坑录(F1–F19)。
- `wchble/mr2/POWER.md` — CH582 低功耗设计指南。
- `esp32/README.md` — ESP32 系列说明（变体矩阵、S3 决定）。
- `esp32/arduino/PLAN.md` — ESP32 Arduino 变体实现计划 (E1–E8)。
- `esp32/arduino/API.md` — ESP32 HTTP API 参考(agent 集成用)。
- `esp32/COMPAT_REPORT.md` — ESP32 跨芯片兼容性实测 + S3 放弃根因。
- `.pi/skills/esp32-windows/` — Windows/ESP32-C3 development pit list (pi skill).
- `.pi/skills/esp32-hardware/` — ESP32 hardware pit list (pi skill).
- `.pi/skills/ch582-linux/` — Linux build/flash/test ops manual (pi skill).
- `EVT/` — WCH CH583 SDK reference code (gitignored, not compiled).
