# esp32/ — ESP32-C3 AT Node (network-enabled variant)

> 基于 ESP32-C3 的 at-node 网络版实现。
> 与 CH582 版本共享同一套 AT 命令语义，但用 WiFi HTTP / MQTT 代替 USB CDC / BLE NUS。

## 目录规划

| 路径 | 用途 |
|------|------|
| `esp32/README.md` | 本文件，ESP32 版整体说明 |
| `esp32/PLAN.md` | ESP32 版实现计划（阶段、接口、验证） |
| `.pi/skills/esp32-windows/` | Windows/ESP32-C3 开发踩坑要点（pi skill） |
| `esp32/esp32_at_node/` | ESP32-C3 AT Node 主 sketch（待实现） |
| `tools/esp32c3_kbd/` | 现有 C3 BLE 键盘测试台（保留，作为 dongle 陪练） |

## 与 CH582 版的关系

| 维度 | CH582 版 | ESP32-C3 版 |
|------|----------|-------------|
| 主控 | CH582F | ESP32-C3 |
| 传输 | USB CDC + BLE HID/NUS | WiFi HTTP + MQTT（规划） |
| 键盘 | BLE HID Peripheral | BLE HID Peripheral（同 C3 测试台） |
| 接收器 | BLE HID Central | 无（可选后续） |
| AT 命令 | `AT+...` 文本协议 | `POST /at/<cmd>` JSON 协议 |
| 供电 | USB / 电池 | USB / 电池 |

**目标**：同一份 Agent 脚本（Python/JS），CH582 走串口，ESP32 走 HTTP，
命令语义完全一致。

## 当前状态

- ✅ HTTP 基础：`/at-node/status`, `/at-node/at`, `/at-node/cmd/keyboard/{tap,text,key}`
- ✅ BLE 键盘：NimBLE boot keyboard，设备名 `AT-Node-ESP`，CH582 dongle 已验证
- ✅ GPIO / ADC：HTTP + 串口 + 原生 AT 全通
- ✅ I2C：扫描/读写，SDA=GPIO8, SCL=GPIO9
- ✅ IR：RMT 38kHz 载波，NEC/SIRC/RAW，GPIO4
- ✅ 串口全功能：与 HTTP 等价的完整 AT 命令集
- ✅ 测试脚本：`tools/test_esp32_at_node.py` 全 PASS
- ✅ MQTT：WiFiClientSecure + PubSubClient，TLS 预留，NVS 配置持久化（待真实 broker 实测）
- 下一步：MQTT broker 实测、更多外设、稳定性优化
