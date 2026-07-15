<!-- DESIGN PHILOSOPHY: at-node is an AI agent peripheral — the hands and feet of LLMs. -->
# at-node Design Notes

Quick reference for future refactoring and debugging.

## Platform Roadmap

| Platform | MCU | Stack | Status | Key Feature |
|----------|-----|-------|--------|-------------|
| **at-node** | CH582F | Bare-metal + WCH SDK | ✅ Active | BLE HID + USB CDC + AT Parser |
| **at-node-nrf** | nRF52840 | Zephyr RTOS | 📋 Planned | All BLE/USB via Kconfig, no register hacking |
| **at-node-esp** | ESP32-S3 | ESP-IDF / Zephyr | 📋 Planned | WiFi + TLS + MQTT, remote agent control |

### Technology Experiment: Rust Core Logic

Plan to implement portable business logic in Rust (`no_std`, `extern "C"` FFI) as an optional alternative to C.

| Module | Language | Reason |
|--------|----------|--------|
| AT parser, command dispatch | **Rust** | `match` enum, compile-time exhaustiveness |
| IR protocol encoding (NEC/SIRC) | **Rust** | unit-testable without hardware |
| I²C sensor decoders | **Rust** | `Option`/`Result`, no null pointer bugs |
| USB ISR, GPIO, TIM, BLE stack | **C** | register access, WCH SDK, pre-compiled libs |

**Build**: `cargo build --target riscv32imac-unknown-none-elf` → `libat_rust.a` → link with C firmware. All Rust logic tested on host (`cargo test`) before deploying to CH582F.

**Rationale**: Rust's type system catches state machine bugs at compile time. `#[test]` enables full coverage without hardware in the loop. Compilation to RISC-V is natively supported by `rustc`.

### Why CH582F is the primary target

Despite the painful SDK, CH582F wins on cost and accessibility:

- **Chip cost**: ~$0.50 (vs nRF52840 ~$2.50, ESP32-S3 ~$3)
- **Family compatibility**: Code is portable to CH592 (lower power, ~$0.30) and CH572 (USB-only) with minimal changes
- **PCB simplicity**: QFN48, single 3.3V rail, few passives — 2-layer board is trivial
- **Replication cost**: Bare-minimum BOM under $3 including IR LED + transistor
- **Yield**: One reflow pass, no fine-pitch BGA, hand-solderable in small batches
- **vs RP2040**: Pico costs the same or more, has no BLE, no IR decoder, fewer GPIOs — worse value

**Verdict**: Develop on CH582F (cheap enough to fry), port to CH592 for production volume. Keep nRF/ESP as premium variants for users who need Zephyr/WiFi.

### Platform Design Philosophy

All variants share the same AT command interface. Agent code (Python) works identically regardless of transport:

```
CH582F:  AT commands via USB CDC / BLE NUS
nRF52:   AT commands via USB CDC / BLE NUS (Zephyr config)
ESP32:   AT commands via USB CDC / TCP+TLS / MQTT
```

The transport layer is abstracted; the AT parser and command handlers are the common core.

## 1. BLE Bonding (SNV)

- **SDK-managed**, not user code.
- `libCH58xBLE.a` handles serialize/deserialize of bonding data
- User provides: Flash R/W callbacks (`Lib_Read_Flash`, `Lib_Write_Flash`) + config macros
- Config in `HAL/include/config.h`:
  - `BLE_SNV_ADDR = 0x77E00` (data Flash last 512 B)
  - `BLE_SNV_BLOCK = 256`, `BLE_SNV_NUM = 1`
- Only 1 bonded device stored; new pairing overwrites old

## 2. USB + Low-Power Mutual Exclusion

- CH582F USB clock stops in Sleep / Shutdown → USB enumeration lost (code 43)
- **HAL_SLEEP=TRUE → USB must be disabled, UART only**
- `main.c` enforces this at compile time: `#if HAL_SLEEP==TRUE` / `#else` USB branch
- Future: AT command `AT+SLEEP` should kill USB first, re-enumerate on wake

## 3. Key Scanning

- `HalKeyConfig(key_press)` must be called in `main()` after `HAL_Init()`, NOT in BLE callback
- Reason: PB22 should work on USB even without BLE connection
- Originally was in `hidEmuRptCB(HID_DEV_OPER_ENABLE)` — BLE-only

## 4. USB Implementation

- **`usb_dev.c` is a direct copy of WCH EVT `HID_CompliantDev/src/Main.c`** with modified descriptors only
- Do NOT rewrite `USB_DevTransProcess()` — the EVT version is the correct one
- The 500+ line custom rewrite attempt was a dead end (all deleted in commit history)
- USB IRQ must be fast — handler is in `.highcode` (RAM-resident)

## 5. USB Descriptor Key Points

| Field | Value |
|-------|-------|
| EP0 size | 64 B |
| EP1 IN (HID report) | 8 B, Interrupt, 10ms polling |
| EP1 OUT (LED output) | 8 B, Interrupt |
| EP1 DMA buffer | `EP1_Databuf[64+64]` (128 B, IN at offset 64) |
| HID Report Descriptor | 68 B standard keyboard (modifiers + 6 keys + LEDs) |
| `wTotalLength` | 0x0029 (41 B config descriptor) |

## 6. GPIO / Sleep Side Effects

- `GPIOB_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU)` **does NOT interfere with USB D+/D-** (PB10/11) on this chip — confirmed by BleInputStick reference
- Original suspicion was wrong; GPIO exclusion not needed

## 7. BLE init order (from BleInputStick)

```
CH58X_BLEInit → HAL_Init → GAPRole_PeripheralInit → HidDev_Init → HidEmu_Init
→ USB_Device_Setup → PFIC_EnableIRQ(USB_IRQn) → TMOS_SystemProcess()
```

## 8. EP1 USB Report Sending

```c
void USB_HID_SendReport(uint8_t *buf, uint8_t len) {
    for (i = 0; i < len && i < 8; i++)
        pEP1_IN_DataBuf[i] = buf[i];
    DevEP1_IN_Deal(len);
}
```

- `pEP1_IN_DataBuf` = `(pEP1_RAM_Addr + 64)` — EP1 IN buffer from library
- `DevEP1_IN_Deal(len)` — sets EP1_T_LEN + ACK, triggers DMA transfer

## 9. Callback Registration in CH58X_BLEInit (MCU.c lines 99-150)

```c
cfg.MEMAddr       = MEM_BUF;
cfg.MEMLen        = BLE_MEMHEAP_SIZE;  // >= 6144
cfg.BufMaxLen     = BLE_BUFF_MAX_LEN;  // 27-516
cfg.BufNumber     = BLE_BUFF_NUM;      // 5
cfg.TxNumEvent    = BLE_TX_NUM_EVENT;  // 1
cfg.TxPower       = BLE_TX_POWER;      // 0 dBm
cfg.SNVAddr       = BLE_SNV_ADDR;
cfg.sleepCB       = CH58X_LowPower;    // *** KILLS USB ***
cfg.tsCB          = HAL_GetInterTempValue;
cfg.readFlashCB   = Lib_Read_Flash;
cfg.writeFlashCB  = Lib_Write_Flash;
```

## 10. Memory Layout (CH582F)

```
0x00000000 - 0x0006FFFF  Flash (448K)
0x00077000 - 0x00077FFF  Data Flash (4K, SNV at 0x77E00)
0x20000000 - 0x20007FFF  RAM (32K)
  - MEM_BUF[BLE_MEMHEAP_SIZE/4]  BLE heap (≥6K)
  - USB EP0/1/2/3 buffers        ~0.7K
  - TMOS stacks + BSS            ~8K
  - Stack                        512B (top of RAM)
```

## 11. Encoding Standards

- All `.c`/`.h`/`.S` are **UTF-8 without BOM**, pure ASCII comments
- Original WCH SDK was GB2312; converted via `tools/batch_utf8.py`
- Verify: `uv run python tools/batch_utf8.py software`
- CI check: `uv run python tools/check_encoding.py`

## 12. IR Transmitter Design (planned)

Single-pin IR LED driver — Timer interrupt gates PWM output.

```
PWM4 → 38kHz carrier (50% duty, continuous)
        └─ RB_PWM_OUT_EN gate bit (register, no external hardware)
TMR1 → 562 µs ISR → IR state machine (NEC lead-in / data / stop)
        └─ < 5 µs per ISR, does not impact BLE/TMOS/USB
```

**Output pin**: PAx (PWM4 channel) → resistor → IR LED → GND (direct)

**For higher range** (recommended power circuit):

```
PAx (PWM4) → 1kΩ → 2N2222 Base
5V rail → IR LED → 100Ω → 2N2222 Collector → GND
2N2222 Emitter → GND
```

The transistor decouples GPIO current from LED drive. Running LED from 5V (not 3.3V GPIO) allows 50-100mA LED current.

**AT command**: `AT+IR=NEC,0x12345678` → TMOS queues → TMR1 state machine → done → `OK`

**Debug**: UART1 (PA9) prints state transitions, no extra pins needed.

### IR AT Commands (planned)

Three protocol modes, no raw timing arrays needed:

| Command | Format | Example |
|---------|--------|---------|
| `AT+IR=NEC,<hex>` | NEC 32-bit code, 38kHz carrier | `AT+IR=NEC,0x807F00FF` |
| `AT+IR=SIRC,<hex>,<bits>` | Sony SIRC, 12/15/20 bits | `AT+IR=SIRC,0xA90,12` |
| `AT+IR=RAW,<t1>,<t2>,...` | Raw timing in µs (fallback) | `AT+IR=RAW,9000,4500,560,...` |

**AT_LINE_MAX must be raised from 256 to 1024** for RAW mode (AC codes are ~400-600 characters).

### AI Agent Integration

```
AI Agent (Python) knows protocol encoding
  └─ AT+IR=NEC,0x12345678\n → CDC serial → at-node
     └─ TMR1 ISR → PWM4 gate → IR LED
     └─ AT_Response("OK")
```

Agent handles code database; firmware only replays. No IR codes stored on device.

### Notes

- NMOS gate drive is unnecessary for IR — NPN 2N2222 is simpler and fast enough
- 38kHz PWM never stops; OUT_EN bit gates the output (no glitch)

## 13. BLE UART Service (NUS) (planned)

Nordic UART Service for wireless AT command channel. Classic bluetooth SPP not available on CH582F.

```
UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
  RX Char (Write)    — Agent commands (Write, WriteNoResp)
  TX Char (Notify)   — Device responses (Notify)
```

Features:
- Shared AT parser with USB CDC
- Response only to originating channel
- Works with nRF UART app / Web Bluetooth API on host
- WiFi-independent, low-power wireless agent communication
