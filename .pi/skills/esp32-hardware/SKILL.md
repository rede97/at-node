---
name: esp32-hardware
description: ESP32 系列硬件兼容性速查表（C3/S3 实测）。记录已测板子的烧录器、VID、fqbn 关键参数（FlashMode/PSRAM）、串口复位时序，以及各自踩过的坑。烧录/调试前先查这张表，避免在相同硬件问题上重复耗时。
---

# ESP32 硬件兼容性速查表

> 实测记录，按板子汇总。换板子 / 换芯片先查这里，别重蹈覆辙。

## 速查表

| 板子 | 芯片 | 烧录器 (VID) | fqbn 关键参数 | Flash | PSRAM |
|---|---|---|---|---|---|
| SuperMini | ESP32-C3 | 原生 USB-JTAG (`303A:1001`) | `CDCOnBoot=cdc` | 4MB quad | 无 |
| nanoESP32-S3 | ESP32-S3 N8R8 | ESPLink/DAPLink (`0D28:0204`) | `CDCOnBoot=default, FlashMode=dio, PSRAM=opi` | 8MB | 8MB OPI |

## nanoESP32-S3（S3）踩坑

- **FlashMode 必须 dio**：官方示例 `--flash_mode dio`，Arduino 默认 `qio` 会让二级 bootloader 读 app 失败 → 无输出。
- **PSRAM 必须 opi**：N8R8 是 8MB OPI（octal 8 线）。`PSRAM=disabled` 实际被编译成 quad（sdkconfig 可见 `CONFIG_SPIRAM_MODE_QUAD`），quad 初始化 OPI PSRAM 失败 → 早期 panic 无输出。
- **烧录器是 ESPLink**（APM32F103，基于 DAPLink），其 UART1 ↔ S3 UART0（GPIO43/44）。`Serial` 用 UART0（`CDCOnBoot=default`），经 ESPLink 的 USB-to-Serial 输出。
- **判 boot 用 RGB LED**（WS2812B，GPIO48）：官方 demo 上电闪烁 = 硬件正常。别只看串口。
- 完整 fqbn：`esp32:esp32:esp32s3:CDCOnBoot=default,FlashSize=8M,FlashMode=dio,PSRAM=opi,PartitionScheme=huge_app`

## SuperMini（C3）踩坑

- 原生 USB-Serial/JTAG，`CDCOnBoot=cdc` 缺了串口完全无输出。
- 完整 fqbn：`esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app`

## 通用坑（换任何板子都可能踩）

- **DTR/RTS 极性**：自动下载电路 DTR→EN、RTS→GPIO0。正常运行态 = `dtr=False, rts=True`（EN=1、GPIO0=1）；`dtr=True` 拉低 EN 触发复位，`rts=False` 拉低 GPIO0 进 bootloader/下载模式（`boot:0x0 DOWNLOAD`）。pyserial 读串口前设 `dtr=False, rts=True`；复位用 **DTR 脉冲**（`dtr=True` 0.15s → `dtr=False`），且**不要 `reset_input_buffer()`**（会清掉一次性 boot 输出）。
- **`temperatureRead()` 已是摄氏**：Arduino-ESP32 3.x 内部已转（C3/S3 走 `temperature_sensor_get_celsius`，原版 ESP32 走 `(F-32)/1.8`）。别再转华氏→摄氏（会得到 12°C 假值）。
- **PSRAM 参数三态**：`disabled` / `enabled`(QSPI quad) / `opi`(octal)。模组尾缀 R 后数字 = PSRAM MB（N8=无，N8R8=8MB）。用错 → 初始化失败 → panic 无输出。
- **FlashMode 以板子文档为准**：Arduino 默认 `qio`，但部分板（nanoESP32-S3）要 `dio`。官方示例的 `--flash_mode` 是权威依据。
- **串口无输出的排查顺序**：① DTR/RTS 时序（应 `dtr=False, rts=True`，复位用 DTR 脉冲）→ ② FlashMode（dio vs qio）→ ③ PSRAM（opi vs quad vs disabled）→ ④ 用物理现象（LED 闪烁）判 boot，别只信串口。
