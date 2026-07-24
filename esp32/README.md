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
- ✅ WiFi 凭据：NVS 持久化，HTTP/串口可配置
- ✅ MQTT：本地 TLS broker 连接/发布成功，自签名 CA 验证；plain TCP (1883) 和 TLS (8883) 双模式；CA 证书/SHA256 指纹可配置
- 下一步：远程 MQTT broker 实测、connect 阻塞优化、更多外设

## 快速开始

1. 拷贝 WiFi 配置：
   ```powershell
   Copy-Item wifi_config.h.example wifi_config.h
   # 编辑 wifi_config.h 填入 SSID 和密码
   ```

2. 编译/上传：
   ```powershell
   cd esp32/esp32_at_node
   .\build.ps1 -Port COM3
   ```

3. 与 CH582 dongle 验证 BLE 键盘：
   ```powershell
   cd tools
   .venv\Scripts\python test_dongle_c3.py --dongle-port COM4 --c3-ip 192.168.1.27
   ```

4. HTTP/AT 命令测试：
   ```powershell
   .venv\Scripts\python test_esp32_at_node.py --ip 192.168.1.27
   ```

5. 本地 MQTT broker 测试：
   ```powershell
   .venv\Scripts\python mqtt_broker.py   # 启动 broker
   # 另一个终端：
   .venv\Scripts\python test_esp32_at_node.py --ip 192.168.1.27
   ```
