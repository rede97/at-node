<!-- DESIGN PHILOSOPHY: This device is an AI agent peripheral. All AT commands are designed for agent-to-hardware interaction, not human UI. -->
# ATNode 需求文档

> 版本：v0.2 · 最后更新：2026-08-15（多平台重构）
> 项目定位：**AI Agent 的物理 I/O 外设**——LLM 的手和脚。
> 本文档是**跨平台需求总账**：通用需求只写一遍；跨硬件差异只登记**最终决定**并指向对应平台文档条目。
> 各芯片的硬件信息、问题原因记录在各平台目录内，不在本文档重复。

---

## 1. 项目概述

### 1.1 项目背景

ATNode 是一组 **AI Agent 物理外设** 固件。所有功能通过 AT 命令（或其 HTTP/MQTT 等价物）
暴露给 Agent —— 键盘输入、传感器采集、GPIO 控制、红外发射 —— 没有 GUI，没有触屏，
只有文本协议。设计理念：LLM 的手和脚。

项目定位为**低成本、低功耗的物理层 AI Agent 控制节点**，多平台并行实现同一套
AT 命令语义：Agent 脚本跨平台复用，换硬件只换传输层。

CH582 平台同时承担 **WCH CH58x 系列 MCU 高质量开发模板** 的角色——三层架构
（HWS/BLE/APP）、一致命名规范、清晰扩展点。详见 [wchble/mr2/DESIGN.md](wchble/mr2/DESIGN.md)。

### 1.2 目标

| 目标 | 说明 |
|------|------|
| **低成本** | WCH BLE 芯片起步，BOM 成本控制在最低 |
| **低功耗** | 支持深度睡眠，电池供电可长期待机（CH582 平台） |
| **双模键盘** | 蓝牙（BLE HID）+ 有线（USB HID）（CH582 平台） |
| **网络控制面** | WiFi HTTP / MQTT TLS（ESP32 平台） |
| **AT 命令控制** | 统一的文本协议控制所有功能，跨平台语义一致 |
| **AI Agent 接入** | 串口 / USB CDC / HTTP / MQTT 与 LLM 管线对接 |
| **多芯片支持** | 平台变体矩阵化，各平台需求差异显式登记（见 §4） |

### 1.3 适用场景

- 远程唤醒电脑（WoL / USB 键盘唤醒）
- AI Agent 通过物理键盘输入执行自动化操作
- 无蓝牙台式机 / VM 的 BLE 键盘桥接（dongle）
- IoT 传感器数据采集（I²C、ADC）
- 远程/内网穿透控制（MQTT + rathole，ESP32 平台）
- 硬件调试与测试（GPIO 控制、串口交互）

---

## 2. 平台与变体矩阵

| 系列 | 变体目录 | 芯片 | 框架/栈 | 主控制面 | 状态 |
|------|---------|------|---------|---------|------|
| WCH BLE | [wchble/mr2/](wchble/mr2/) | CH582F（规划 CH592） | MounRiver Studio 2 工程，裸机 + TMOS + 预编译 BLE 栈 | USB CDC（+UART） | ✅ Active |
| ESP32 | [esp32/arduino/](esp32/arduino/) | ESP32-C3、原版 ESP32 | Arduino-ESP32 | WiFi HTTP + MQTT TLS | ✅ Active |
| ESP32 | [esp32/zephyr/](esp32/zephyr/) | ESP32-S3 等 PSRAM 机型 | Zephyr | WiFi HTTP + MQTT TLS | 🗄 已归档（e759a2a，Zephyr 放弃，转 esp-rs/Rust） |
| Nordic | [nordic/zephyr/](nordic/zephyr/) | nRF52840 | Zephyr（nRF Connect SDK） | USB CDC | 📋 TODO |

各平台文档索引见 [README.md](README.md)。

---

## 3. 跨平台通用需求

### 3.1 AT 命令协议（所有平台一致）

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| F3.1 | 基于文本行的请求/响应协议，`\n` 作为分隔符（`\r\n` 兼容）| P0 | ✅ |
| F3.2 | 命令格式：`AT+<CMD>[=<param1>,<param2>,...]` | P0 | ✅ |
| F3.3 | 响应格式：`\r\n<结果>\r\n`，成功返回 `OK`，失败返回 `ERROR:<原因>` | P0 | ✅ |
| F3.5 | 命令缓冲区 256 字节，支持退格编辑和回显 | P2 | ✅ |
| F3.6 | 蓝牙连接/断开时自动上报 `+BT_CONNECTED` / `+BT_DISCONNECTED`（URC） | P1 | ✅ |
| F3.9 | `AT+HELP` 输出格式化帮助文本，按功能分组，带用法/参数/示例 | P0 | ✅ |
| F3.10 | `AT+HELP=<CMD>` 仅显示指定命令的详细帮助 | P1 | ✅ |
| F3.11 | 帮助文本存 Flash 只读区，不占 RAM | P2 | ✅ |

基础命令集（各平台必须实现）：`AT` / `AT+RST` / `AT+VER` / `AT+HELP` / `AT+STATUS` /
`AT+KEY` / `AT+TAP` / `AT+MOD` / `AT+KEY_STR` / `AT+KEY_SEQ` / `AT+GPIO_W` / `AT+GPIO_R` /
`AT+ADC` / `AT+I2C_SCAN` / `AT+I2C_R` / `AT+I2C_W`。

键盘 HID 语义（各平台一致）：8 字节 boot 输入报告（修饰键掩码 + 6 键值，十进制或
0x 十六进制 HID Usage ID）；`AT+TAP` 原子按下+释放为常规注入首选；`AT+KEY_STR`
US 布局 ~15ms/字符；`AT+KEY_SEQ` 脚本预翻译序列回放。

平台特有命令在各平台文档中定义（CH582：[wchble/mr2/USER-MANUAL.md](wchble/mr2/USER-MANUAL.md)；
ESP32：[esp32/arduino/API.md](esp32/arduino/API.md)）。

### 3.2 AI Agent 对接

```
AI Agent (LLM / Python 脚本)
    │
    ├── 物理串口 ──→ UART TTL ──→ ATNode（全平台）
    ├── USB ──→ USB CDC (虚拟串口) ──→ ATNode（CH582 / nRF52840）
    ├── LAN ──→ WiFi HTTP (/at-node/cmd/*) ──→ ATNode（ESP32）
    └── 云 ──→ MQTT TLS / rathole 隧道 ──→ ATNode（ESP32）
```

AI Agent 只需发送 `AT+KEY_STR=hello`（或 HTTP 等价端点）即可完成物理键盘输入，
无需操作系统层面的输入法注入或驱动支持。

### 3.3 安全

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| Q4.1 | 键盘模拟功能必须显式声明其安全风险（README 顶部警告） | P0 | ✅ |
| Q4.2 | AT 命令接口应可配置为仅接受特定物理接口 | P1 | ✅（ESP32：HTTP 可关；CH582：通道固定） |
| Q4.3 | 配对绑定信息加密存储（CH582 SNV / ESP32 NVS） | P1 | ✅ |
| Q4.4 | 网络控制面默认最小暴露：ESP32 HTTP 无认证，仅限可信 NAT；远程走 MQTT TLS | P0 | ✅（见 §4 D6） |

### 3.4 AI 可读性

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| Q3.0 | 根目录 AGENTS.md 包含完整架构图、分层栈、初始化序列、约束清单 | P0 | ✅ |
| Q3.1 | 每个源文件头注释说明模块职责和数据流向 | P0 | ✅ |
| Q3.2 | 代码注释纯 ASCII 英文，无编码兼容问题 | P1 | ✅ |
| Q3.3 | 命名自解释——函数前缀表示层级（`hws_`/`ble_`/`AT_`） | P1 | ✅ |
| Q3.4 | 平台 DESIGN.md 提供设计哲学和扩展指南 | P1 | ✅ |
| Q3.5 | AGENTS.md + 各平台 DESIGN.md 双重引导，AI 读完即建立心智模型 | P2 | ✅ |

### 3.5 可靠性与可移植性

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| Q1.1 | 看门狗定时器，异常时自动复位 | P1 | ✅（CH582 `AT+WDG` 运行时开关） |
| Q1.2 | AT 命令异常输入不会导致设备崩溃 | P1 | ✅ |
| Q1.3 | BLE 断连后自动重连/广播 | P1 | ✅ |
| Q1.4 | Flash 参数区磨损均衡 | P2 | ⬜ |
| Q2.1 | 硬件服务层隔离芯片差异，上层不直接操作寄存器 | P1 | ✅（CH582 HWS 层） |
| Q2.2 | 芯片型号统一在 `config.h` 中通过宏切换 | P1 | ✅（CH582 `CHIP_ID`） |
| Q2.3 | AT 命令解析器与硬件无关，可独立测试 | P2 | ✅ |

---

## 4. 跨硬件差异 — 最终决定登记

> 本节只登记**最终决定**与理由摘要；硬件细节、问题原因、实测数据一律在指向的平台文档中维护。

| # | 差异点 | 最终决定（日期） | 详情 |
|---|--------|----------------|------|
| D1 | 红外发射（IR） | **CH582 不实现**（已从固件系统性删除）；**ESP32 Arduino 版实现**（RMT 38kHz，NEC/SIRC/RAW，GPIO4） | [esp32/arduino/README.md](esp32/arduino/README.md) |
| D2 | ESP32-S3 支持 | **Arduino 变体不支持 S3**（2026-08-14）：预编译 esp32s3-libs 默认 `CONFIG_SPIRAM_USE_MALLOC=y`，mbedTLS 启动崩溃，app 层无法规避；S3 及 PSRAM 机型由 **Zephyr 变体**承接 | [esp32/COMPAT_REPORT.md](esp32/COMPAT_REPORT.md)、[esp32/zephyr/README.md](esp32/zephyr/README.md) |
| D3 | 主控制面 | **CH582：USB CDC 为唯一 AT 通道**（UART1 仅调试，2026-07-25）；**ESP32：WiFi HTTP 为主 + MQTT TLS 远程 + 串口全功能后备** | [wchble/mr2/USER-MANUAL.md](wchble/mr2/USER-MANUAL.md)、[esp32/arduino/README.md](esp32/arduino/README.md) |
| D4 | 低功耗 ↔ USB | **CH582 硬件互斥**（休眠关 USB 时钟，枚举丢失），编译期 `HWS_SLEEP` 一刀切；运行时 VBUS 双模切换为产品化必备（T7.x）；CH592 若支持休眠保持 USB 可解除（T6.5） | [wchble/mr2/POWER.md](wchble/mr2/POWER.md)、§5.9 |
| D5 | dongle 第三方键盘兼容 | **第三方复杂键盘（RK 类多 Report ID/NKRO/Report Map 解析）正式废弃**；仅支持 Just Works 配对 + boot keyboard 8 字节报告路径；第三方简单 boot 键盘按兼容目标支持 | [wchble/mr2/DESIGN.md](wchble/mr2/DESIGN.md)、[wchble/mr2/FIELD-NOTES.md](wchble/mr2/FIELD-NOTES.md) |
| D6 | ESP32 HTTP 认证 | **HTTP 控制面不做认证**，仅部署于可信 NAT；不可信网络用 `AT+HTTP=0` 关闭，保留 MQTT TLS | [esp32/arduino/README.md](esp32/arduino/README.md) §安全策略 |
| D7 | ESP32 服务开关语义 | 统一 **enable（运行时临时，内存）+ auto（上电自启，NVS）两层**（HTTP/MQTT/BLE）；rathole 保持三级（master/enable/auto） | [esp32/arduino/README.md](esp32/arduino/README.md) §功能宏与固件变体 |
| D8 | ESP32 刷机通道 | **C3 必须经 `build-c3.ps1`**（fqbn 带 `CDCOnBoot=cdc`）；标准 ESP32 经 `build-esp32.ps1`；agent 默认使用板卡专用脚本，禁止裸 arduino-cli/IDE 默认刷机 | [esp32/arduino/README.md](esp32/arduino/README.md) §快速开始 |
| D9 | BLE 角色运行期切换 | **CH582 不支持热切**：角色在 `BLE_LibInit` 定死，DUAL 构建 `AT+ROLE` = 写标志 + 软复位 | [wchble/mr2/DESIGN.md](wchble/mr2/DESIGN.md) |
| D10 | ESP32 功能模型与 LED | **统一功能模型**（2026-08-23）：核心能力（BLE HID / LED / I2C）与通信接口（HTTP / MQTT / rathole）两合集，WiFi 为底座不裁剪；**LED 三态** `ATNODE_LED`=0无/1呼吸/2彩色 WS2812，breath 归入 LED 域。冲突编译期 `#error`：C3 呼吸灯（GPIO8）与 I2C 二选一；经典 ESP32 默认 breath @ GPIO2（板载蓝灯，零冲突）；S3 WS2812 @ GPIO48 独立（Rust `led-color` feature 默认开）；Arduino 的 WS2812 为预留钩子（驱动未实现，调色用 rust-s3）。板型选择宏 `ATNODE_BOARD` 一键切换默认配置集，默认跟随编译目标 | [esp32/arduino/features.h](esp32/arduino/features.h)、[esp32/rust/README.md](esp32/rust/README.md) |

---

## 5. CH582 平台需求（wchble/mr2）

> 硬件规格/引脚/USB 端点：[wchble/mr2/HARDWARE.md](wchble/mr2/HARDWARE.md)；
> 问题原因记录：[wchble/mr2/FIELD-NOTES.md](wchble/mr2/FIELD-NOTES.md)；
> AT 命令细节：[wchble/mr2/USER-MANUAL.md](wchble/mr2/USER-MANUAL.md)。
>
> **实现现状快照（2026-07-22）**：M1–M4 全部闭环。kbd 全链路生产态；dongle 接收器双板验证
> （扫描→配对→订阅→USB 转发、自动回连、退避保护、量产安静模式）；`BLE_MODE` 三态 +
> `AT+ROLE` 运行期切换实测；Linux CI 一键全绿（`loop_test.sh`）；ISP 无线升级打通。
> **C3 模拟键盘台架闭环**（HTTP 驱动 ESP32-C3 键盘 → dongle 转发，双端实测 PASS）。
> 状态图例：✅ 已验证 | 🚧 部分实现/未验证 | ⬜ 未实现

### 5.1 BLE 蓝牙键盘（✅ 已实现）

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| F1.1 | 设备以 BLE Peripheral 角色广播，设备名 "AT-Node" | P0 | ✅ |
| F1.2 | 广播数据包含 HID 服务 UUID（0x1812）和电池服务 UUID（0x180F） | P0 | ✅ |
| F1.3 | 连接后发送 8 字节 HID 键盘输入报告（修饰键 + 6 键值） | P0 | ✅ |
| F1.4 | 支持连接参数更新（间隔 10ms，延迟 0，超时 5s） | P0 | ✅ |
| F1.5 | 支持 PHY 更新到 LE 2M | P1 | ✅ |
| F1.6 | 支持绑定（Bonding）与配对 | P1 | ✅ |
| F1.7 | 支持 HID 空闲超时断开（默认 60s） | P2 | ✅ |
| F1.8 | 支持从机连接延时（Slave Latency）以降低功耗 | P2 | ✅ |
| F1.9 | 通过 AT 命令发送任意 HID 键值 | P0 | ✅ `AT+KEY`/`AT+MOD`/`AT+KEY_SEQ` |

#### 5.1.1 BLE 键盘多模（✅ 已实现，单活动链路模型）

> CH582 键盘支持多模连接：同时配对绑定 3 台主机（PC/笔记本/平板），
> 通过 `AT+DEV` 或快捷键无缝切换当前输出目标，**无需复位**。
> 构建变体 `MODE=KBD_MULTI` 启用，与单模键盘（`MODE=KBD`）和
> 接收器（`MODE=DONGLE`）互斥；`MODE=DUAL` 保留单模键盘+dongle 调试。

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| F1.10 | 支持 3 台主机同时配对并绑定（`PERIPHERAL_MAX_CONNECTION=3`, `BLE_SNV_NUM=3`) | P2 | ✅ |
| F1.11 | 每个连接独立维护 `conn_handle`，`kb_flush()` 按当前选中设备发送 | P2 | ✅ |
| F1.12 | `AT+DEV=<target>` 无缝切换当前输出设备，`AT+DEV` 查询当前设备列表（target=USB\|BLE1\|BLE2\|BLE3） | P2 | ✅（单活动链路 + 预留自动回连） |
| F1.15 | `BLE_MEMHEAP_SIZE` 按连接数动态调整（每连接 ~1.5 KB，3 连接约 12 KB）| P1 | ✅ |

#### 5.1.2 BLE HID Host 接收器模式（✅ 已实现）

> AT-Node 作为 Central 主动连接一台 BLE 键盘，接收其 HID 报告并经
> USB HID 转发给 PC —— 把任意 BLE 键盘变成"有线键盘"（KVM/远控场景）。
>
> **2026-07-21 双板验证**：AT-Node（kbd 固件）作为测试键盘，dongle 完成
> 扫描→Just Works 配对→绑定→GATT 发现→Boot 模式订阅→通知转发 USB，
> `test_dongle_loop.py` 连续 3 次全 PASS，按键字符出现在主机编辑器。

**模式配置（两级）：**

| 级别 | 机制 | 说明 |
|------|------|------|
| 编译期裁剪 | `BLE_MODE` 宏：`KBD`(1) / `DONGLE`(2) / `DUAL`(3) | KBD=只键盘；DONGLE=只接收器；DUAL=两者编入，运行期可切 |
| 运行期切换 | `AT+ROLE=KBD\|DONGLE`（仅 DUAL 构建）| 写模式标志到 DataFlash → `SYS_ResetExecute()` → 按标志启动（见 §4 D9） |

**内存代价（编译期裁剪的意义）：**

| 构建 | GATT server 表 | Central 上下文 | BLE 堆预留 | 说明 |
|------|---------------|----------------|-----------|------|
| KBD | ~1 KB（常驻） | 0 | 5 KB | 当前构建 |
| DONGLE | 0（裁掉）+ 无 SNV 绑定 | 1 | 6 KB | 省 ~1 KB .data |
| DUAL | ~1 KB（静态链接，常驻） | 按启动角色分配 | 6 KB | 见下 |

DUAL 构建因切换必经复位，复位后按 DataFlash 标志**单角色启动**：
`ble_stack_init` 只配置当前角色的 `ConnectNumber`，只注册当前角色
的 GATT 服务 —— 堆内上下文与运行状态均为单角色。真正甩不掉的
双份成本只有：静态链接的 GATT server 表（~1 KB RAM）与按最大值
的堆预留（6 KB vs 5 KB）。

运行期切换**必须经过一次软复位**（无法热切换，见 §4 D9）：路径为
"存标志 + 软复位"，切换时间 ≈ 一次重启（<1 s）。

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| F1.16 | `BLE_MODE` 三态宏接入 config.h（替代 `BLE_DONGLE` 布尔宏）| P3 | ✅（KBD/DONGLE/DUAL，派生 `BLE_HAS_KBD/DONGLE` 门控）|
| F1.17 | DONGLE 构建：Central 角色扫描/连接/绑定 BLE 键盘 | P3 | ✅ 双板验证（含 Just Works/自动配对码） |
| F1.18 | GATT client 解析 HID over GATT 报告（Report Map 解析或 Boot 模式）| P3 | ✅ Boot 模式已验证；全量解析随 D5 废弃 |
| F1.19 | 收到的键值经 `kb_usb_send_report()` 转发给 PC | P3 | ✅ 端到端验证（按键出现在主机） |
| F1.20 | DONGLE 构建裁掉 GATT server 服务与广播代码（`#if` 门控）| P3 | ✅ |
| F1.21 | DUAL 构建：`AT+ROLE` 运行期切换（DataFlash 标志 + 软复位）| P3 | ✅ 双板实测（标志存 0x7C00，标志随固件刷写保持）|

> **范围声明（2026-07-22，即 §4 D5）**：接收器**仅支持 Just Works 配对 + boot
> keyboard input report（8 字节标准布局）** 路径。第三方复杂键盘（RK 类多功能：
> 多 Report ID/NKRO/Report Map 解析）**正式废弃不实现**；第三方简单 boot 键盘按
> 兼容目标支持。

**AT 命令组（DONGLE 构建，✅=已实现）：**

```
AT+BT_SCAN[=<秒>[,<过滤>]]  ✅ 扫描,按信号排序(idx 0=最近);过滤=名称子串或 HID
AT+BT_CONN=<目标>    ✅ 连接:<idx|地址|名称>(自动配对/绑定/GATT 发现/订阅)
AT+BT_DISC            ✅ 断开当前 Central 连接
AT+BT_STATE           ✅ dongle 状态诊断(discovery 调试)
AT+BT_PASSKEY=<6位>   ✅ 应答/预设 SMP 配对码(默认 123456)
AT+BT_AUTO[=0|1]      ✅ 自动回连绑定键盘(直连 SNV 绑定地址,断链/开机即回连)
AT+BT_LIST            ✅ 已绑定设备列表(SNV)
```

> 延伸场景 — "USB 蓝牙扩展"：台式机无蓝牙、VM 透传不便、脚本
> 不想碰 WinRT 时，AT-Node 经 CDC 提供纯文本 BLE 扫描/连接能力，
> 任何语言零依赖驱动 BLE。狭义实现（键盘接收器）跑通 Central 链路
> 后，可再暴露通用 GATT 读写（`AT+GATT_RD/WR`），升级为通用 BLE
> 网卡 —— 边际成本低，单独评估。

### 5.2 USB 有线键盘（✅ 已实现）

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| F2.1 | USB Device 模式，枚举为 HID 键盘设备 | P1 | ✅ |
| F2.2 | 通过 USB HID Report 协议发送按键 | P1 | ✅ |
| F2.3 | 支持与 BLE 键盘同时工作（双模） | P2 | ✅ |
| F2.5 | USB CDC ACM 虚拟串口，AT 命令通道 | P1 | ✅ |

### 5.3 AT 命令接口（✅ 已实现，详见 [wchble/mr2/USER-MANUAL.md](wchble/mr2/USER-MANUAL.md)）

通用协议见 §3.1。CH582 特有决策：**USB CDC 为唯一 AT 通道**（UART1 仅作调试输出，
2026-07-25，即 §4 D3）。

CH582 平台命令集（在通用命令之上）：

| 命令 | 功能 | 状态 |
|------|------|------|
| `AT+DEV[=USB\|BLE\|BLE1\|BLE2\|BLE3]` | 键盘输出目标路由；无参查询状态 | ✅ |
| `AT+TAP=<ms>,<mods>,<k1>,..,<k6>` | 按下并自动释放（TMOS 定时器，非阻塞）；日常点按首选 | ✅ |
| `AT+ISP` | 进入 ROM ISP bootloader（擦 page0 软复位，wchisp 烧录） | ✅ |
| `AT+ROLE[=KBD\|DONGLE]` | 查询/切换 BLE 角色（DUAL 构建：写标志 + 软复位） | ✅ |
| `AT+SLEEP=<mode>[,<sec>]` | RTC 定时休眠并唤醒（USB 构建下禁用） | ✅ |
| `AT+BT_SCAN/CONN/DISC/PAIR/STATE/PASSKEY/AUTO/LIST` | dongle 命令组（见 §5.1.2） | ✅ |
| `AT+WDG[=0\|1]` | 看门狗运行时开关：默认关，武装后 100ms 周期喂狗，复位不记忆 | ✅ |
| ~~`AT+IR`~~ | ~~红外发射~~ | ❌ 已删除（§4 D1） |

**输入注入纪律**：常规注入一律 `AT+TAP`（原子按下+释放）或 `AT+KEY_STR`/`AT+KEY_SEQ`
（序列引擎自动配对 press/release）；裸 `AT+KEY` 仅限修饰键按住等特殊场景，且必须显式
补 `AT+KEY=0,0` 释放。卡住时止血：补发 `AT+KEY=0,0` ×2-3。
（[wchble/mr2/FIELD-NOTES.md](wchble/mr2/FIELD-NOTES.md) F18）

### 5.4 UART / USB 虚拟串口（✅ 已实现）

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| F4.1 | UART1 调试输出（TX），115200-8N1 | P0 | ✅ |
| F4.2 | UART1 收发（RX + TX），支持 AT 命令输入 | P0 | ✅ 与 CDC 共用同一线路解析器 |
| F5.1 | USB CDC ACM 设备，枚举为虚拟 COM 口 | P1 | ✅ CDC+HID 复合设备（VID 1a86 / PID 2107） |
| F5.2 | 虚拟串口与物理串口可同时工作，AT 命令共享 | P1 | ✅ 双通道独立响应路由，CDC 回显输入行 |
| F5.3 | 支持通过虚拟串口进行固件升级 | P2 | ✅ `AT+ISP` + `tools/ci/isp_flash.py`（ROM ISP 通道） |

### 5.5 GPIO / ADC / I²C（✅ 已实现）

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| F6.1 | 配置引脚为推挽输出 / 浮空输入 / 上拉/下拉输入 | P1 | ✅ `GPIO_W=<p>,<l>[,5|20]` `GPIO_R=<p>[,0|1|2]` |
| F6.2 | 读取引脚数字电平 | P1 | ✅ |
| F6.3 | 设置引脚输出高/低 | P1 | ✅ |
| F7.1 | 外部单通道 ADC 采样 | P1 | ✅（Vref 校准 + PGA） |
| F7.2 | AT 命令指定通道返回 mV | P1 | ✅ |
| F7.3 | 电池电压采样（`hws_batt` 经 `ADC_InterBATSampInit`） | P2 | ✅ |
| F8.1 | I²C 主机 100kHz/400kHz 可配 | P1 | ✅ |
| F8.2 | 扫描 7 位地址，返回应答设备列表 | P1 | ✅ |
| F8.3 | 读指定设备寄存器（1~N 字节） | P1 | ✅ |
| F8.4 | 写指定设备寄存器 | P1 | ✅ |

### 5.6 性能需求

| 编号 | 需求 | 指标 | 优先级 |
|------|------|------|--------|
| P1.1 | AT 命令响应时间 | < 10ms（本地命令）/ < 50ms（BLE 命令）| P1 |
| P1.2 | 键盘按键延迟（BLE） | < 20ms（连接态 10ms 间隔）| P1 |
| P1.3 | 键盘按键延迟（USB） | < 5ms（USB 1ms 轮询） | P1 |
| P1.4 | 待机功耗 | < 50μA（睡眠模式）| P2 |
| P1.5 | 广播功耗 | < 500μA（广播间隔 100ms）| P2 |
| P1.6 | 连续工作时长（500mAh 电池） | > 30 天（待机） / > 8 小时（持续按键）| P2 |
| P1.7 | RAM 占用 | < 8 KB（AT 任务 + 缓冲区）| P2 |
| P1.8 | Flash 占用 | < 256 KB（全部功能；当前 kbd 165 KB / dual 200 KB）| P1 |

### 5.7 低功耗测试需求（CH582 专项）

> 低功耗设计原理与定量分析：[wchble/mr2/POWER.md](wchble/mr2/POWER.md)。
> 本节保留测试需求与指标定义。

#### 5.7.1 测试目标

| 编号 | 目标 | 说明 |
|------|------|------|
| T1.1 | 测定 CH582F **芯片级极限最低功耗** | 最小系统（仅供电 + 去耦电容）下测量各休眠模式电流 |
| T1.2 | 测定 **系统级极限最低功耗** | 实际 PCB（含 LED、按键上拉、LDO 等外围）下测量 |
| T1.3 | 计算并验证 **电池待机时长** | 基于实测功耗推算 + 实际电池长时间待机验证 |

#### 5.7.2 测试模式与场景

| 编号 | 模式 | 芯片状态 | 外设状态 | 说明 |
|------|------|---------|---------|------|
| T2.1 | **深度睡眠（下电模式）** | 仅 RTC 运行，CPU 断电，SRAM 保持或断电可选 | 所有外设时钟关闭，GPIO 保持或高阻 | 寻找芯片手册标称极限值 |
| T2.2 | **睡眠模式（Sleep）** | CPU 暂停，SRAM 保持，RTC 运行 | 外设时钟关闭，唤醒后立即恢复 | 定时唤醒轮询场景 |
| T2.3 | **空闲模式（Idle）** | CPU 暂停，SRAM 保持 | 外设保持状态，任意中断唤醒 | 最低延迟唤醒场景 |
| T2.4 | **BLE 广播（待连接）** | CPU 间歇工作 | BLE 射频定时广播 | 未连接时典型功耗 |
| T2.5 | **BLE 连接（空闲）** | CPU 间歇工作 | BLE 射频按连接间隔监听 | 已连接无按键 |
| T2.6 | **BLE 连接（持续按键）** | CPU 持续工作 | BLE 射频发送 + GPIO 扫描 | 最耗电工作场景 |

#### 5.7.3 测试方法

| 编号 | 方法 | 说明 |
|------|------|------|
| T3.1 | **精密万用表 / 源表** | 串联高精度万用表（Keysight 34465A、Fluke 8846A）或 SMU，测平均电流 |
| T3.2 | **串联电阻 + 示波器** | 10Ω 精密电阻抓电流波形，观察瞬态尖峰和占空比 |
| T3.3 | **低功耗专用评估板** | CH582F 最小系统板（无 LED、无额外上拉），排除外围干扰 |
| T3.4 | **实际电池放电测试** | 500mAh 锂电池，记录满电到关机时间 |

#### 5.7.4 待测数据

| 编号 | 数据项 | 说明 |
|------|--------|------|
| T4.1 | 各模式 **平均电流**（μA） | 至少连续采样 1 分钟取均值 |
| T4.2 | 各模式 **峰值电流**（mA） | 重点 BLE 射频发射和 Flash 擦写瞬态 |
| T4.3 | **深度睡眠唤醒时间**（μs） | 唤醒事件到 CPU 第一条指令的延迟 |
| T4.4 | **广播间隔 vs 平均功耗** 曲线 | 20ms~1000ms 分别测量 |
| T4.5 | **连接间隔 vs 平均功耗** 曲线 | 7.5ms~100ms 分别测量 |
| T4.6 | **电池放电曲线** | 4.2V → 2.8V 全过程 |

#### 5.7.5 预期指标（参考芯片数据手册）

| 参数 | CH582F 手册标称 | 实测目标 |
|------|----------------|---------|
| 下电模式（Shutdown） | 0.1 μA | < 0.5 μA（含外围漏电） |
| 睡眠模式（Sleep） | 0.6 μA（32K RTC 运行） | < 2 μA（系统级） |
| 空闲模式（Idle） | — | < 100 μA |
| 广播平均（100ms 间隔） | — | < 200 μA |
| 连接平均（10ms 间隔） | — | < 500 μA |
| 发射峰值（0dBm） | 5.2 mA | < 6 mA |

> 注：实测目标为系统级值（含 LDO 静态功耗、外围漏电），高于芯片手册纯 MCU 值属正常。

#### 5.7.6 电池待机测算

| 场景 | 假设条件 | 待机时长（500mAh） |
|------|---------|------------------|
| 纯睡眠（下电模式） | 1 μA | **~57 年**（受电池自放电和寿命限制） |
| 睡眠 + 每日唤醒 1 次按键 | 平均 2 μA | **~28 年** |
| BLE 广播（1s 间隔，待连接） | 50 μA | **~417 天** |
| BLE 广播（100ms 间隔，待连接） | 200 μA | **~104 天** |
| BLE 连接 + 按键 1h/天 | 脉冲平均 300 μA | **~69 天** |
| BLE 持续按键 | 3 mA | **~7 天** |

> 以上为理论推算值，需通过实际测试修订。

#### 5.7.7 测试报告输出

| 编号 | 交付物 | 说明 |
|------|--------|------|
| T5.1 | **功耗测量数据表** | 各模式、各配置电流测量记录（Excel/CSV） |
| T5.2 | **电流波形截图** | 示波器抓取的典型工作脉冲波形 |
| T5.3 | **电池放电曲线图** | 500mAh 电池完整放电过程 |
| T5.4 | **待机时长结论** | 典型使用场景实测待机天数 |
| T5.5 | **功耗优化建议** | 针对非理想因素的改进方案 |

#### 5.7.8 低功耗与 USB 互斥（最终决定见 §4 D4）

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| T6.1 | `HWS_SLEEP = TRUE` 时**禁止启用 USB**，仅硬件 UART 与上位机通信 | P0 | ✅（编译期强制） |
| T6.2 | `HWS_SLEEP = FALSE` 时 USB 正常；需休眠由 `AT+SLEEP` 显式触发 | P0 | ✅ |
| T6.3 | `AT+SLEEP` 执行前先**关闭 USB**（清 `port_en`、断 DP 上拉），唤醒后由上位机重新枚举 | P1 | ✅ |
| T6.4 | 启动时检测 `HWS_SLEEP`：启用则 `USB_Device_Setup()` 直接返回 | P0 | ✅ |
| T6.5 | CH592 等后续芯片若支持休眠态保持 USB 连接，可解除此互斥 | P3 | 📋 规划 |

**典型配置组合**：

| 场景 | HWS_SLEEP | USB | 通信方式 |
|------|-----------|-----|---------|
| 桌面开发/调试 | FALSE | ✅ 启用 | USB CDC + USB HID |
| AI Agent 有线控制 | FALSE | ✅ 启用 | USB CDC AT 命令 |
| 电池供电低功耗 | TRUE | ❌ 禁用 | 硬件 UART AT 命令 |
| BLE 蓝牙键盘 | FALSE | ✅ 启用 | USB + BLE 双模 |

#### 5.7.9 USB 热插拔与低功耗双模（运行时切换）🔜

> 当前 `HWS_SLEEP` 是编译期开关。实际产品需要运行时检测 VBUS，无缝切换模式。

| 编号 | 需求 | 优先级 |
|------|------|--------|
| T7.1 | **VBUS 检测**：GPIO 输入引脚 + 分压电阻监测 USB 5V，上电判断初始模式 | P1 |
| T7.2 | **插入 USB**：VBUS 边沿中断 → 初始化 USB → 禁止深度睡眠 → BLE 保持连接 | P1 |
| T7.3 | **拔出 USB**：VBUS 边沿中断 → 反初始化 USB → 恢复睡眠能力 → BLE 保持连接 | P1 |
| T7.4 | **睡眠回调自感知**：`hws_sleep_enter()` 检查 `usb_active` 标志，USB 活跃直接返回 | P1 |
| T7.5 | **初始化 GPIO 策略自适应**：GPIO 全拉输入仅在 `!vbus_present()` 时执行 | P2 |
| T7.6 | **BLE 保持**：USB 拔插不影响 BLE 连接 | P1 |

**状态转换图**：

```
上电 ──→ 检测 VBUS
              ├─ VBUS=1 (USB供电) ──→ USB+BLE 双模 ──→ 拔掉 ──→ BLE+睡眠
              └─ VBUS=0 (电池)    ──→ BLE+睡眠      ──→ 插入 ──→ USB+BLE 双模
```

**硬件需求**：USB VBUS (5V) → 分压电阻 (2:1) → 3.3V → GPIO 输入引脚（VBUS 边沿中断）。

**边界条件**：
- USB 枚举需 ~100ms，枚举期间不能睡眠
- 拔出时 MCU 正在睡眠：VBUS 掉电不唤醒 MCU，需 RTC 定时唤醒检测（~1s 周期）或 PMOS 边沿唤醒电路
- 当前 BLE 参数插入/拔出时不变，后续可联动切换（拔掉后调大连接间隔省电）

---

## 6. ESP32 平台需求（esp32/arduino）

> 详细功能/配置/API/坑录：[esp32/arduino/README.md](esp32/arduino/README.md)、
> [esp32/arduino/API.md](esp32/arduino/API.md)、[esp32/arduino/RATHOLE.md](esp32/arduino/RATHOLE.md)。
> 跨芯片兼容实测与 S3 放弃根因：[esp32/COMPAT_REPORT.md](esp32/COMPAT_REPORT.md)。

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| E1.1 | 与 CH582 语义一致的网络版：同一 Agent 脚本换传输层即可复用 | P0 | ✅ |
| E1.2 | WiFi HTTP 主控制面（`/at-node/cmd/*` JSON） | P0 | ✅ |
| E1.3 | MQTT TLS 远程控制面（指纹验证，独立 FreeRTOS task） | P0 | ✅ |
| E1.4 | 串口全功能 AT 后备通道（与 HTTP 等价） | P0 | ✅ |
| E1.5 | BLE HID 键盘输出（NimBLE boot keyboard，仅 Peripheral） | P0 | ✅ |
| E1.6 | 统一配置注册表（AT/HTTP/MQTT 三通道等价，NVS 持久化） | P0 | ✅ |
| E1.7 | 服务开关统一 enable + auto 两层语义（§4 D7） | P1 | ✅ |
| E1.8 | 编译期功能变体：full / remoter / base / rathole（`features.h`） | P1 | ✅ |
| E1.9 | rathole 内网穿透客户端（单隧道，plain TCP，NVS 持久化自连） | P1 | ✅ |
| E1.10 | Web 控制面（gzip 单页应用，flash 一次发送，JSON 驱动） | P1 | ✅ |
| E1.11 | AP 配网（GPIO10 触发或 `AT+AP=1`，Captive Portal） | P1 | ✅ |
| E1.12 | BLE 配对安全（默认不广播，显式 60s 公共配对窗口） | P1 | ✅ |
| E1.13 | IR 红外发射（RMT 38kHz，NEC/SIRC/RAW）（§4 D1） | P2 | ✅ |
| E1.14 | WiFi 看门狗（断线 15s 周期重连，服务随链路恢复） | P1 | ✅ |
| E1.15 | 芯片温度上报（`temp_c`，HTTP/AT/Web 三通道） | P3 | ✅ |

---

## 7. 规划平台需求（TODO）

### 7.1 esp32/zephyr（ESP32-S3 等 PSRAM 机型）— 🗄 已归档（2026-08-20）

> **决定：放弃 Zephyr 路线，S3 转向 esp-rs（Rust, esp-hal + Embassy）实现。**
> 完整实现曾完成并硬件冒烟通过，归档于 commit **e759a2a**
> （`git show e759a2a` 可取回全部代码与文档；其 README 含完整坑录）。
> 放弃原因：Zephyr 对 ESP32 的支持不健全、不可持续维护——
> WiFi/BLE 为二进制 blob（崩溃无源码符号）、子系统组合从未被上游测试
> （WiFi+BT+USB+HTTP 同开连踩 8 个集成坑，详见该 commit README 的 bug 表）、
> Kconfig/API 在主线频繁变动、错误信息不指向根因。
> Z1.4/Z1.5 不再适用；Rust 版规划见 esp32/rust/。

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| Z1.1 | Zephyr 工程骨架（ESP32-S3 目标，PSRAM 启用） | P2 | 🗄 已完成→归档（e759a2a） |
| Z1.2 | WiFi HTTP + MQTT TLS 控制面（对齐 esp32/arduino 语义） | P2 | 🗄 已完成→归档（e759a2a，硬件冒烟通过） |
| Z1.3 | BLE HID 键盘（NimBLE host on Zephyr） | P2 | 🗄 已完成→归档（改用 Zephyr 原生 host + USB HID） |
| Z1.4 | PSRAM 大负载：完整 CA bundle、更大 Web 资产、多并发隧道 | P3 | ✗ 取消（随 Zephyr 路线放弃） |
| Z1.5 | IR 发送（RMT） | — | ✗ 取消（Zephyr 无 RMT 驱动；Rust esp-hal 有 RMT，可在 Rust 版实现） |

### 7.2 nordic/zephyr（nRF52840）

> 规划与准入条件：[nordic/zephyr/README.md](nordic/zephyr/README.md)。

| 编号 | 需求 | 优先级 | 状态 |
|------|------|--------|------|
| N1.1 | Zephyr 工程骨架（nRF52840，nRF Connect SDK） | P3 | ⬜ |
| N1.2 | BLE HID 键盘（Peripheral）+ USB CDC/HID 复合——对齐 CH582 行为 | P3 | ⬜ |
| N1.3 | AT 命令全语义对齐（无 WiFi，无网络控制面） | P3 | ⬜ |

---

## 8. 开发路线图

| 阶段 | 内容 | 状态 |
|------|------|------|
| **v0.1** | CH582 BLE HID 键盘基础功能 | ✅ 已完成 |
| **v0.2** | CH582 三层分离（HWS/BLE/APP）+ AT 命令 + USB CDC+HID 复合 | ✅ 已完成 |
| **v0.3** | 符号重命名统一 + 代码质量提升 + 模板化文档 | ✅ 已完成 |
| **v0.4** | CH582 GPIO/ADC/I²C 命令 + 多模键盘 + dongle 接收器 | ✅ 已完成 |
| **v0.5** | ESP32-C3 网络版（HTTP/MQTT/rathole/Web 控制面） | ✅ 已完成 |
| **v0.6** | 多平台目录重构 + 需求归类（本次） | ✅ 已完成 |
| **v0.7** | CH592 移植 + CH582 低功耗实测（§5.7） | 📋 规划 |
| **v0.8** | esp32/zephyr（S3）与 nordic/zephyr（nRF52840）启动 | 📋 规划 |

---

## 9. 附录

### 9.1 参考文档

- [CH582 数据手册](https://www.wch.cn/products/CH582.html)
- [CH583 EVT 参考代码](EVT/)
- [HID Usage Tables (USB-IF)](https://www.usb.org/hid)
- [BLE HID over GATT Profile Spec](https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile/)

### 9.2 术语

| 术语 | 说明 |
|------|------|
| TMOS | TI 风格的协作式任务调度器（CH582 BLE 栈内置） |
| HID | Human Interface Device，人机交互设备 |
| GATT | Generic Attribute Profile，BLE 属性协议 |
| CDC | Communication Device Class，USB 通信设备类 |
| HWS | Hardware Services，硬件服务层——纯寄存器操作，不含协议栈逻辑 |
| URC | Unsolicited Result Code，主动上报消息 |
| SNV | 简易非易失存储（Flash 模拟 EEPROM，CH582） |
| NVS | Non-Volatile Storage（ESP32 键值存储） |
| MR2 | MounRiver Studio 2，WCH RISC-V IDE/工具链 |
