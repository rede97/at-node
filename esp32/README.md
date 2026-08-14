# esp32/ — ESP32 series

> ESP32 系列 AT Node：网络版实现（WiFi HTTP / MQTT 控制面 + BLE HID 键盘输出）。
> 与 WCH BLE 系列共享同一套 AT 命令语义，但用 WiFi HTTP / MQTT 代替 USB CDC 作为主控制面。

## 变体矩阵

| 目录 | 框架 | 支持芯片 | 状态 |
|---|---|---|---|
| [arduino/](arduino/) | Arduino-ESP32 | **ESP32-C3**（已验证）、**原版 ESP32**（已验证 2026-08-15） | ✅ Active |
| [zephyr/](zephyr/) | Zephyr | **ESP32-S3** 及其他带 PSRAM 的高性能型号 | 📋 TODO |

**ESP32-S3 不在 Arduino 变体支持范围内（最终决定，2026-08-14）**：预编译
`esp32s3-libs` 默认 `CONFIG_SPIRAM_USE_MALLOC=y`（PSRAM 冒充全局 malloc 堆），
导致 mbedTLS/`WiFiClientSecure` 启动阶段崩溃，app 层无法规避，唯一修复是重编译
esp32s3-libs。S3 及 PSRAM 机型转由 Zephyr 变体承接。
根因与实测记录：[COMPAT_REPORT.md](COMPAT_REPORT.md)。

## 文档索引

| 文档 | 内容 |
|---|---|
| [arduino/README.md](arduino/README.md) | Arduino 变体主文档（功能、配置层、变体、快速开始） |
| [arduino/API.md](arduino/API.md) | HTTP API 参考（agent 集成用） |
| [arduino/PLAN.md](arduino/PLAN.md) | Arduino 变体实现计划（阶段、接口、验证） |
| [arduino/RATHOLE.md](arduino/RATHOLE.md) | rathole 隧道客户端架构/内存账目/坑录 |
| [COMPAT_REPORT.md](COMPAT_REPORT.md) | ESP32 跨芯片兼容性测试报告（C3/S3 实测 + S3 放弃根因） |
| [zephyr/README.md](zephyr/README.md) | Zephyr 变体规划（TODO） |

## 与其他系列的关系

| 维度 | WCH BLE（CH582） | ESP32 系列 |
|---|---|---|
| 控制面 | USB CDC + UART | WiFi HTTP（主） + MQTT TLS（远程） + 串口 |
| 键盘输出 | BLE HID + USB HID 双模 | BLE HID（NimBLE boot keyboard） |
| 接收器（dongle） | ✅ BLE HID Central | 无（可选后续） |
| 网络能力 | 无 | WiFi / mDNS / rathole 内网穿透 |

**目标**：同一份 Agent 脚本（Python/JS），CH582 走串口，ESP32 走 HTTP，命令语义完全一致。
