# esp32/PLAN.md — ESP32-C3 AT Node 网络版实现计划

> 版本：v0.1 · 2026-07-23
> 目标：在 ESP32-C3 上实现与 CH582 AT Node 语义一致的网络版外设，
> 以 WiFi HTTP 为主控制面，BLE HID 键盘为输出通道。
> MQTT 作为预留扩展，当前不实现。

---

## 1. 定位与边界

| 项 | 说明 |
|---|---|
| **产品定位** | CH582 AT Node 的网络版并行实现，不是替代 |
| **核心能力** | BLE HID 键盘（boot protocol） + WiFi HTTP 命令接口 |
| **控制面** | HTTP（主） + USB 串口（调试/后备） |
| **MQTT** | 预留架构接口，当前不实现 broker 对接 |
| **BLE 角色** | 仅 Peripheral（键盘），不做 Central/接收器 |

## 2. 目录结构

```
esp32/
├── README.md                  # ESP32 版整体说明
├── PLAN.md                    # 本文件
├── esp32_at_node/             # 主 sketch
│   ├── esp32_at_node.ino      # 主程序
│   ├── wifi_config.h          # WiFi 凭据（gitignore）
│   ├── wifi_config.h.example  # 模板
│   ├── http_server.cpp/h      # HTTP 路由与 JSON 解析
│   ├── at_parser.cpp/h        # 原生 AT 命令解析与执行
│   ├── ble_keyboard.cpp/h     # NimBLE HID keyboard 封装
│   └── build.ps1              # 编译/上传脚本
```

## 3. HTTP 路由设计

Base path：`/at-node`

### 3.1 状态查询

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/at-node/status` | 返回 JSON：BLE 连接、IP、MAC、设备名 |

### 3.2 结构化 JSON 命令

路径模式：`POST /at-node/cmd/<resource>/<action>`

| 路径 | 参数（JSON body） | 对应 CH582 AT 命令 |
|------|------------------|------------------|
| `/at-node/cmd/keyboard/tap` | `{"mods":0,"k":4,"ms":100}` | `AT+TAP=100,0,4` |
| `/at-node/cmd/keyboard/text` | `{"s":"Hello","ms":40,"gap":30}` | `AT+KEY_STR=Hello` |
| `/at-node/cmd/keyboard/key` | `{"mods":0,"keys":[4,0,0,0,0,0]}` | `AT+KEY=0,4` |
| `/at-node/cmd/gpio/write` | `{"pin":12,"level":1}` | `AT+GPIO_W=12,1` |
| `/at-node/cmd/gpio/read` | `{"pin":13}` | `AT+GPIO_R=13` |
| `/at-node/cmd/adc/read` | `{"ch":0}` | `AT+ADC=0` |
| `/at-node/cmd/i2c/scan` | `{}` | `AT+I2C_SCAN` |
| `/at-node/cmd/ir/send` | `{"protocol":"NEC","data":"0x807F00FF"}` | `AT+IR=NEC,0x807F00FF` |

响应统一：
```json
{"ok":true,"cmd":"keyboard/tap","ms":100}
```

错误响应：
```json
{"ok":false,"error":"invalid param: k must be 0-255"}
```

### 3.3 原生 AT 命令路由

| 方法 | 路径 | Content-Type | Body | 响应 |
|------|------|--------------|------|------|
| POST | `/at-node/at` | `text/plain` | `AT+TAP=100,0,4` | `{"ok":true,"response":"OK"}` |

**设计原则**：JSON 命令端点和 `/at-node/at` 最终调用同一个
`handle_at_command(const char* line)` 函数，行为完全一致。

### 3.4 404

未知路径返回 `{"ok":false,"error":"not found"}`。

## 4. BLE HID 键盘

- 基于现有 `tools/esp32c3_kbd/esp32c3_kbd.ino` 的 NimBLE boot keyboard 实现。
- 设备名默认 `AT-Node-ESP`，可通过 `/at-node/cmd/config/name` 修改（预留）。
- 暴露 Boot Keyboard Input Report (0x2A22, READ|NOTIFY)、
  Boot Keyboard Output Report (0x2A32)、Protocol Mode (0x2A4E, 默认 0)。
- 电池服务保留。

## 5. 串口功能

- USB 串口（USB-Serial-JTAG）保留，默认输出调试日志。
- 支持简单文本命令：`TAP <key>`、`TEXT <str>`、`STATUS`。
- 不实现完整 AT 命令集（HTTP 为主控制面）。

## 6. 外设命令（GPIO / ADC / I2C / IR）

ESP32-C3 与 CH582 外设能力差异较大，**分阶段实现**：

| 子系统 | 阶段 | 说明 |
|--------|------|------|
| GPIO | E3 | 数字输入输出，可配置引脚 |
| ADC | E3 | 模拟采样，返回 mV |
| I2C | E4 | 主机模式，扫描/读写 |
| IR | E5 | NEC/SIRC/RAW 发送 |

**待确认**：IR 发送在 ESP32-C3 上需要 RMT 外设，比 CH582 的 PWM+Timer 复杂，是否纳入第一阶段？

## 7. MQTT 预留

当前不实现，但代码结构预留：

- `mqtt_client.cpp/h` 接口定义（connect/publish/subscribe）。
- 命令处理器与 HTTP 共用同一入口 `handle_at_command()`。
- Topic 草案：`atnode/<device_id>/cmd/<cmd>` / `atnode/<device_id>/resp/<cmd>`。

## 8. 实现阶段

| # | 阶段 | 内容 | 判据 |
|---|------|------|------|
| E1 | HTTP 基础 | `/at-node/status`, `/at-node/cmd/keyboard/{tap,text,key}` | 通过 `tools/test_esp32_at_node.py` 验证 |
| E2 | BLE 键盘接入 | NimBLE boot keyboard，设备名 `AT-Node-ESP` | CH582 dongle 扫描/连接/转发成功 |
| E3 | GPIO + ADC | `/at-node/cmd/gpio/{write,read}`, `/at-node/cmd/adc/read` | 万用表/杜邦线验证 |
| E4 | I2C | `/at-node/cmd/i2c/scan` + 读写 | 挂 EEPROM/传感器验证 |
| E5 | IR | `/at-node/cmd/ir/send` | 示波器/设备验证 |
| E6 | 串口后备 | 简单 TAP/TEXT/STATUS 命令 | 可用 |
| E7 | 测试脚本 | `tools/test_esp32_at_node.py` | 全项 PASS |

## 9. 与 CH582 版的命令语义对齐

| CH582 AT 命令 | ESP32 HTTP 端点 | 备注 |
|-------------|----------------|------|
| `AT+VER` | `GET /at-node/status` | 版本信息合并到 status |
| `AT+HELP` | `GET /at-node/help` | 命令列表（JSON） |
| `AT+STATUS` | `GET /at-node/status` | 状态合并 |
| `AT+TAP` | `POST /at-node/cmd/keyboard/tap` | 参数映射 |
| `AT+KEY` | `POST /at-node/cmd/keyboard/key` | 参数映射 |
| `AT+KEY_STR` | `POST /at-node/cmd/keyboard/text` | 参数映射 |
| `AT+GPIO_W` | `POST /at-node/cmd/gpio/write` | 参数映射 |
| `AT+GPIO_R` | `POST /at-node/cmd/gpio/read` | 参数映射 |
| `AT+ADC` | `POST /at-node/cmd/adc/read` | 参数映射 |
| `AT+I2C_SCAN` | `POST /at-node/cmd/i2c/scan` | 参数映射 |
| `AT+I2C_R/W` | `POST /at-node/cmd/i2c/{read,write}` | 预留 |
| `AT+IR=...` | `POST /at-node/cmd/ir/send` | 参数映射 |
| `AT+BT_*` | 不实现 | ESP32 版不做 BLE 主机/接收器 |

## 10. 开放问题

- [ ] IR 发送是否纳入 E5？（RMT 实现复杂度）
- [ ] 设备名/网络配置是否支持运行时修改？（需要持久化到 NVS）
- [ ] MQTT 的 broker 地址/端口/认证方式？
- [ ] 是否需要 TLS（HTTPS）？当前建议仅本地网络使用 HTTP。
