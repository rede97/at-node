# esp32/PLAN.md — ESP32-C3 AT Node 网络版实现计划

> 版本：v0.2 · 2026-07-24
> 目标：在 ESP32-C3 上实现与 CH582 AT Node 语义一致的网络版外设，
> 以 WiFi HTTP 为主控制面，MQTT (TLS) 为远程控制面，BLE HID 键盘为输出通道。

---

## 1. 定位与边界

| 项 | 说明 |
|---|---|
| **产品定位** | CH582 AT Node 的网络版并行实现，不是替代 |
| **核心能力** | BLE HID 键盘（boot protocol） + WiFi HTTP 命令接口 |
| **控制面** | HTTP（主） + USB 串口（调试/后备） + MQTT（远程 TLS） |
| **MQTT** | 已实现：PubSubClient + FreeRTOS task，TLS 指纹验证 |
| **BLE 角色** | 仅 Peripheral（键盘），不做 Central/接收器 |

## 2. 目录结构

```
esp32/
├── README.md                  # ESP32 系列说明（变体矩阵、S3 决策）
├── COMPAT_REPORT.md           # 跨芯片兼容性实测（C3/S3）
├── arduino/                   # Arduino 变体（C3 / 原版 ESP32）— 本计划对应目录
│   ├── README.md              # Arduino 变体主文档
│   ├── PLAN.md                # 本文件
│   ├── arduino.ino            # 主 sketch（与目录同名，Arduino 约定）
│   ├── wifi_config.h          # WiFi 凭据（gitignore）
│   ├── wifi_config.h.example  # 模板
│   ├── web/                   # Web 控制面前端（build.py → web_page.h）
│   ├── web_page.h             # 生成的 gzip 单页应用
│   └── build.ps1 + build-c3.ps1 / build-esp32.ps1
│                              # 编译/上传：共享引擎 + 板卡专用封装
│                              # （C3: CDCOnBoot=cdc, PartitionScheme=huge_app；agent 默认用封装）
└── zephyr/                    # Zephyr 变体（ESP32-S3 等 PSRAM 机型，TODO）
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

- 基于现有 `tools/demo/esp32c3_kbd/esp32c3_kbd.ino` 的 NimBLE boot keyboard 实现。
- 设备名默认 `AT-Node-ESP`，可通过 `/at-node/cmd/config/name` 修改（预留）。
- 暴露 Boot Keyboard Input Report (0x2A22, READ|NOTIFY)、
  Boot Keyboard Output Report (0x2A32)、Protocol Mode (0x2A4E, 默认 0)。
- 电池服务保留。

## 5. 串口功能

- USB 串口（USB-Serial-JTAG）与 HTTP **同等优先级**，均实现完整 AT 命令集。
- 命令解析、参数处理、响应生成与 HTTP 的 `/at-node/at` 完全一致。
- 串口同时保留调试日志（可开关）。

## 6. 外设命令（GPIO / ADC / I2C / IR）

ESP32-C3 与 CH582 外设能力差异较大，**分阶段实现**：

| 子系统 | 阶段 | 说明 |
|--------|------|------|
| GPIO | E3 | 数字输入输出，可配置引脚 |
| ADC | E3 | 模拟采样，返回 mV |
| I2C | E4 | 主机模式，扫描/读写 |
| IR | E5 | **RMT 外设实现**，NEC/SIRC/RAW 发送 |

**IR 实现方案**：
- 使用 ESP32-C3 的 RMT 外设（专为红外收发设计）。
- 参考开源方案（如 IRremoteESP8266 的 RMT 驱动思路），但不引入完整库（体积大），
  在 `ir_sender.cpp/h` 中实现最小 RMT 载波发送状态机。
- 支持协议：NEC、SIRC、RAW 时序数组。

## 7. 设备配置持久化

- 配置存储：**NVS (Preferences)**。
- 配置项：设备名（BLE advertising）、WiFi SSID/密码、MQTT 服务器/指纹/auto。
- 修改方式：
  - **Web**：`POST /at-node/cmd/config/set`（JSON 键值对）。
  - **串口 AT**：`AT+CONF=<key>=<value>`（与 HTTP 同等功能）。
- 修改后生效策略：
  - WiFi 参数：保存后提示重启生效，或提供 `AT+RST` 命令。
  - BLE 设备名：保存后重新初始化 BLE 广播。

## 8. MQTT（已实现）

- **必须支持 TLS**（远程连接安全）。
- 实现方案：
  - `PubSubClient` + `WiFiClientSecure`，运行在独立 FreeRTOS task。
  - TLS 验证：`setInsecure()` + post-connect SHA256 指纹校验（NVS `mqtt_ca_fp`）。
  - Topic：`atnode/<hostname>/cmd`（订阅）、`atnode/<hostname>/resp`（响应）、`atnode/<hostname>/info`（retained manifest）、`atnode/<hostname>/state`（LWT）。
  - 连接成功后自动发布 `sys/info`（含完整 API 目录，~2KB）。
  - `mqtt_auto` NVS 开关控制重启后自动连接。

## 9. 实现阶段

| # | 阶段 | 内容 | 判据 |
|---|------|------|------|
| E1 | HTTP 基础 | `/at-node/status`, `/at-node/cmd/keyboard/{tap,text,key}` | ✅ 已完成（骨架实现，测试通过） |
| E2 | BLE 键盘接入 | NimBLE boot keyboard，设备名 `AT-Node-ESP` | ✅ 已完成（CH582 dongle 扫描/连接/转发成功） |
| E3 | GPIO + ADC | `/at-node/cmd/gpio/{write,read}`, `/at-node/cmd/adc/read` | ✅ 已完成（HTTP/串口/AT 全通，测试通过） |
| E4 | I2C | `/at-node/cmd/i2c/scan` + 读写 | ✅ 已完成（HTTP/串口/AT 全通，测试通过） |
| E5 | IR (RMT) | `/at-node/cmd/ir/send` | ✅ 已完成（RMT 38kHz 载波，NEC/SIRC/RAW 全通） |
| E6 | 串口全功能 | 串口实现与 HTTP 等价的完整 AT 命令集 | ✅ 已完成（AT/TAP/TEXT/CONF/GPIO/ADC/I2C/IR/MQTT 全通） |
| E7 | 测试脚本 | `tools/test/test_esp32_at_node.py` | ✅ 已完成（HTTP 端点全 PASS） |
| E8 | MQTT TLS | `mqtt_client` 实现 + broker 对接 | ✅ 已完成（本地 TLS broker 连接/发布成功，自签名 CA） |
| E9 | rathole 隧道 | 单隧道客户端（plain TCP），AT/HTTP/MQTT 三通道配置 | ✅ 已完成（本地 server 端到端验证，长连接稳定） |
| E10 | 统一配置层 | `config_set/get/list` 注册表，AT+SET/GET/KEYS + `/at-node/cmd/config` + MQTT `config/*` | ✅ 已完成（存量命令为别名，三通道实测） |

## 10. 与 CH582 版的命令语义对齐

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
| `AT+IR=...` | `POST /at-node/cmd/ir/send` | RMT 外设实现 |
| `AT+CONF` | `POST /at-node/cmd/config/set` | NVS 持久化，串口/HTTP 双通道 |
| `AT+BT_*` | 不实现 | ESP32 版不做 BLE 主机/接收器 |

## 10. 已决策事项

- **IR 发送**：纳入 E5，使用 RMT 外设，参考开源 RMT 驱动方案。
- **设备配置**：NVS 持久化，HTTP + 串口双通道修改。
- **WiFi 凭据**：NVS 持久化，HTTP + 串口可配置（`AT+WIFI=ssid|pass`）。
- **MQTT**：已实现，必须 TLS，broker 配置由运行时决定（NVS）。
- **CA 证书/指纹**：仅支持 SHA256 指纹验证（NVS 持久化），不嵌入完整 CA/PEM；适合嵌入式资源约束。
- **TLS**：MQTT 必须 TLS；HTTP 仅本地网络，无 TLS。
- **串口与 HTTP**：同等优先级，均实现完整 AT 命令集。

## 11. 后续工作

- [x] MQTT 远程 broker 实测（TLS + 指纹验证）—— 云服务器 8883 连接成功，RPC 调用验证通过。
- [x] MQTT connect 阻塞优化 —— MQTT 操作移至独立 FreeRTOS task，主循环不再阻塞。
- [x] MQTT auto-connect —— NVS `mqtt_auto` 开关，首次成功连接后自动启用，`AT+MQTT=auto,<0|1>` 可控制。
- [x] API 目录结构化升级 —— 17 个服务的参数描述通过 MQTT `sys/info`、HTTP `/at-node/help.json`、AT+HELP 三通道暴露；HTML help 增加 Description 列。
- [x] TLS 简化 —— 删除 PEM CA 证书支持，仅保留 SHA256 指纹（`setInsecure()` + post-connect 校验）。
- [x] PubSubClient buffer 扩容 —— 1024→4096，适配 ~3KB 的 sys/info API 目录 payload。
- [ ] 更多外设（PWM、SPI、UART 透传等）。
- [ ] Agent 工作流集成示例。
