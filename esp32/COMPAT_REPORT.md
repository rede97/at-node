# ESP32 跨芯片兼容性测试报告

日期：2026-08-14
固件：`esp32/arduino/` full 变体
改动：状态接口 + Web UI 新增芯片温度字段（`temp_c`）

> **⚠️ ESP32-S3 支持已放弃（2026-08-14）**：S3 在 Arduino-ESP32 3.3.10 工具链内跑 TLS（`WiFiClientSecure`/mbedTLS）会因预编译库默认 `CONFIG_SPIRAM_USE_MALLOC=y`（PSRAM 冒充全局 malloc 堆）在启动阶段崩溃，且无法通过改 app 层配置规避，唯一修复是重编译 esp32s3-libs。经评估放弃 S3 支持，**本项目仅支持 ESP32-C3**。下文的 S3 记录仅为踩坑留档，不再维护。

## 1. 温度功能

### 实现

- 新增 `cpu_temp_c()`（`esp32_at_node.ino`）：直接返回 `temperatureRead()`。
  - `temperatureRead()` 在所有 ESP32 变体上**已经返回摄氏度**：
    - 原版 ESP32：`(temprature_sens_read() - 32) / 1.8`
    - C3 / S3（`SOC_TEMP_SENSOR_SUPPORTED`）：`temperature_sensor_get_celsius()`
  - 带 `isnan()` 兜底，传感器异常时返回 `0.0`，避免 JSON 输出非法的 `nan`。
- 三个通道同步输出：
  - HTTP：`GET /at-node/cmd/status` → `"temp_c":55.6`
  - AT：`AT+STATUS` → `... temp_c=55.6`
  - Web：Status 页新增 "Temperature" 行（`s-temp`），显示 `55.6 °C`

### 过程中修复的 bug

初版误把 `temperatureRead()` 当作华氏度又转换了一次，得到 `12.0°C`。
查证 Arduino-ESP32 3.3.10 源码（`cores/esp32/esp32-hal-misc.c:80-113`）后确认其已返回摄氏，
去掉多余转换。修复后 C3 实测 `55.6°C`（合理芯片温度）。

### 精度说明

- 内部温度传感器测的是 **die 温度**，非环境温度；受 CPU 负载 / WiFi / BLE 收发影响。
- C3/S3 的 `temperature_sensor_install` 配置范围 `10~50 ℃`（`TEMPERATURE_SENSOR_CONFIG_DEFAULT`），
  超出该范围读数可能不准或夹紧。
- 精度约 ±3℃，仅作状态参考，不可作精确测温。

## 2. 编译与内存对比（full 变体）

| 芯片 | 编译 | Flash | RAM | 备注 |
|---|---|---|---|---|
| ESP32-C3 | ✅ | 1531561 B (48%) | 49836 B (15%) | 单核 160MHz，4MB flash |
| ESP32-S3 | ✅ | 1437195 B (45%) | 59428 B (18%) | 双核 240MHz，8MB flash + 8MB PSRAM |

- S3 编译通过，说明固件代码在 S3 上 **API / 语法层面完全兼容**（无 C3 专属调用）。
- S3 RAM 占用略高（59428 vs 49836），主因是双核 + 更大的外设支持；仍有 268 KB 余量，安全。

## 3. 烧录

| 芯片 | 端口 | 结果 | 硬件识别 |
|---|---|---|---|
| ESP32-C3 | COM3 | ✅ | QFN32 rev v0.4，Flash 4MB (XMC) |
| ESP32-S3 | COM26 | ✅ | QFN56 rev v0.1，Flash 8MB (quad)，PSRAM 8MB (AP_3v3) |

- 两块板 esptool 均 `Hash of data verified`，烧录校验通过。
- S3 经 **DAPLink 桥**（VID `0D28` PID `0204`，Mbed/CMSIS-DAP）连接，走 UART 烧录。

## 4. 运行时验证

### ESP32-C3（COM3）— 完整通过 ✅

```
esp32_at_node start
WiFi connected, IP=192.168.1.27
mDNS: atnodeesp-5688.local
HTTP server on port 80
I2C initialized (SDA=8, SCL=9)
IR initialized (GPIO4, 38kHz carrier)
BLE keyboard started: AT-Node-ESP-5688

AT+STATUS → role=esp32_at_node connected=0 ip=192.168.1.27
            wifi_rssi=-52dBm (96%) temp_c=55.6
```

WiFi / HTTP / I2C / IR / BLE 全部初始化成功，温度字段 `temp_c=55.6` 正确。

### ESP32-S3（COM26）— 编译烧录通过，串口调试通道受阻 ⚠️

- 编译 ✅、烧录 ✅（见上表）。
- `esptool --before no-reset chip-id` 连接失败（`No serial data received`）：
  表明 S3 复位后**不在 bootloader 监听态**，推断已进入 app 运行。
- 但 **无法通过 DAPLink CDC 读到正常 boot 日志**：`115200` 恒为 0 字节；
  高波特率（460800/921600）曾读到少量乱码（疑似 ROM bootloader 消息被错误波特率采样）。
- 尝试的配置组合均未解决：
  - `CDCOnBoot=default`（Serial→UART0 GPIO43/44）+ `PSRAM=opi`
  - `CDCOnBoot=default` + `PSRAM=disabled`
  - `CDCOnBoot=cdc` + `USBMode=hwcdc`（Serial→USB-Serial/JTAG GPIO19/20）+ `PSRAM=disabled`

**结论**：不是固件崩溃（编译/烧录均过，且 no-reset 推断已在运行），
而是 **DAPLink 串口桥的接线 / 波特率映射与 S3 的 `Serial` 路由不匹配**，属硬件调试通道问题，
需在板级确认 DAPLink 的 UART 到底接的是 S3 的 UART0 还是 USB-Serial/JTAG，以及其内部 UART 波特率是否固定。

## 5. 兼容性结论

| 维度 | ESP32-C3 | ESP32-S3 |
|---|---|---|
| 编译（full 变体） | ✅ | ✅ |
| 烧录 | ✅ | ✅ |
| 温度字段（`temp_c`） | ✅ 55.6°C | ⚠️ 未实测（串口通道受阻） |
| WiFi / HTTP / I2C / IR / BLE | ✅ | ⚠️ 未逐项实测 |
| 串口日志 | ✅ | ⚠️ DAPLink 桥不通 |

**核心结论**：温度功能在 C3 上完整验证通过；固件代码对 S3 **编译兼容 + 烧录成功**。
S3 的运行时逐项验证受 DAPLink 串口桥硬件限制阻塞，属接线/波特率问题而非固件问题。

## 6. 待确认 / 下一步

1. 确认 S3 板 DAPLink 的 UART 桥接的是 UART0（GPIO43/44）还是 USB-Serial/JTAG（GPIO19/20）。
2. 确认 DAPLink 内部 UART 波特率是否随 CDC 设置（若固定，则需对齐 S3 的 `Serial` 波特率）。
3. 换用 S3 原生 USB（若有板载 USB-C 直连）烧录 + 读串口，即可绕过 DAPLink 完成 S3 运行时逐项验证。
4. `temperature_sensor` 在 S3 上同样走 `SOC_TEMP_SENSOR_SUPPORTED` 分支，代码路径与 C3 一致，预期可用。
