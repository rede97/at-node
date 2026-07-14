# at-node Design Notes

Quick reference for future refactoring and debugging.

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
