# at-node

**AT-command driven agent I/O module — the hands & feet of AI**

> Designed for **AI Agents**, not humans. No GUI, no app — just AT commands over serial.
> Connect an LLM to the physical world: type keystrokes, read sensors, control GPIO.

📡 **Remote control / cloud broker quickstart**: [`tools/broker/GET_START.md`](tools/broker/GET_START.md)

[🇬🇧 English](#english) · [🇨🇳 中文](#chinese)

---

## <a name="english"></a>🇬🇧 English

**at-node** is an IoT firmware for the [WCH CH582F](https://www.wch.cn/products/CH582.html) (RISC-V BLE 5.0 MCU) that bridges AT commands to keyboard input, sensor reading, and I/O control. It can be seamlessly integrated into AI Agents to perform simple computer operations — wake up a PC, type text, press hotkeys, or read environmental data.

> ⚠️ **Security Warning**
>
> This device emulates a keyboard and can send arbitrary keystrokes to your computer. **This is equivalent to giving anyone with serial access unrestricted keyboard control.**
>
> - **Do not** connect this device to a computer you do not fully trust.
> - **Do not** expose the AT command interface (UART / USB CDC) to untrusted AI agents, public networks, or other people's devices.
> - **Always** consider the physical serial port as a privileged administrative console, not a toy interface.
>
> You are responsible for securing the physical and logical access to this device.

### Features

| Feature | Status | Description |
|---------|--------|-------------|
| **BLE HID Keyboard** | ✅ | Bluetooth keyboard via HID over GATT. Multi-host (3 slots) supported |
| **USB HID Keyboard** | ✅ | Wired keyboard via USB HID (composite with CDC) |
| **AT Command Interface** | ✅ | Text-based command/response protocol (40+ commands) |
| **USB CDC (Virtual Serial)** | ✅ | USB virtual COM port for AT commands (VID 1A86 / PID 2107) |
| **BLE HID Host (Dongle)** | ✅ | Central receiver: bridges any BLE keyboard to USB |
| **GPIO Control** | ✅ | Read/write digital pins via AT commands |
| **ADC Sampling** | ✅ | 14-channel external analog input, calibrated mV output |
| **I²C Sensor** | ✅ | Master mode scan/read/write (SCL=PB13, SDA=PB12) |
| **UART Serial Port** | ✅ | Hardware UART1 (TX=PA9, RX=PA8) for debug output |
| **Watchdog** | ✅ | Runtime-armable via AT+WDG |
| **Low-power Sleep** | ✅ | RTC-timed sleep (BLE-only build, USB mutually exclusive) |

### How It Works

```
┌──────────────────────────────────────────────────┐
│                   AI Agent / Host                  │
│  (Python script, automation tool, LLM pipeline)   │
└──────────┬───────────────────────────┬────────────┘
           │ AT Commands               │ AT Commands
           ▼                           ▼
┌──────────────────────┐   ┌──────────────────────┐
│   USB CDC / UART1    │   │   WiFi HTTP (ESP32)  │
│   (control channel)  │   │   (network variant)  │
└──────────┬───────────┘   └──────────────────────┘
           │
           ▼
┌──────────────────────────────────────────────────┐
│                   at-node (CH582F)                 │
│  ┌────────┐ ┌─────────┐ ┌──────┐ ┌────────┐     │
│  │AT Parser│ │BLE+USB  │ │I²C   │ │GPIO/ADC│     │
│  │        │ │Keyboard │ │Sensor│ │Control │     │
│  └────────┘ └─────────┘ └──────┘ └────────┘     │
└──────────────────────────────────────────────────┘
```

### Hardware

| Current | Planned |
|---------|---------|
| **CH582F** (RISC-V rv32imac, 60 MHz) | **CH592** (RISC-V, lower power, lower cost) |
| 448 KB Flash / 32 KB RAM | BLE 5.4 |
| BLE 4.2/5.0 | Pin-compatible peripheral set |
| Multi GPIO, I²C, SPI, ADC, PWM, USB | |

### Build

Requires MounRiver Studio toolchain (`riscv-none-embed-gcc`, `make`) on PATH.

```bash
cd ch582/obj && make --no-print-directory main-build          # kbd (default)
cd ch582/obj && make --no-print-directory main-build MODE=KBD_MULTI  # multi-host keyboard
cd ch582/obj && make --no-print-directory main-build DONGLE=1 # BLE receiver
```

Output: `ch582/obj/at-node.elf` / `.hex` / `.lst` / `.map`.

### Pinout (Default)

| Pin | Function | Notes |
|-----|----------|-------|
| PA0 | LED1 | Push-pull output, active low |
| PB22 | KEY1 | Pull-up input, active low |
| PB4 | KEY2 | Pull-up input, active low |
| PA9 (TXD1) | UART1 TX | Debug output (115200 baud) |
| PA8 (RXD1) | UART1 RX | Debug input |
| PB13 | I²C SCL | Pull-up, 100 kHz |
| PB12 | I²C SDA | Pull-up |
| PB10/PB11 | USB D+/D- | USB 2.0 FS (do not use as GPIO) |

### Project Structure

```
ch582/
├── APP/              # Application layer
│   ├── main.c           # Entry + 7-stage init
│   ├── at_cmds.c        # AT command table + keyboard routing
│   ├── at_parser.c      # Line parser (UART + CDC dual channel)
│   ├── hidkbd_ble.c     # BLE keyboard (advertising/connection/reports)
│   ├── hidkbd_usb.c     # USB keyboard report sending
│   ├── usb_dev.c        # USB composite (CDC + HID)
│   ├── HWS/             # Hardware services (KEY, LED, RTC, SLEEP, GPIO, ADC, I2C, WDG)
│   └── BLE/             # BLE GATT services (HID Dev, Battery, Device Info, Dongle)
├── LIB/              # BLE stack (libCH58xBLE.a)
├── StdPeriphDriver/  # Peripheral drivers (GPIO, UART, I²C, ADC, USB...)
├── RVMSIS/           # RISC-V core access layer (NVIC, PFIC)
├── Startup/          # Reset vector & interrupt table
├── Ld/               # Linker script
└── obj/              # Build output
```

---

## <a name="chinese"></a>🇨🇳 中文

**at-node** 是 **AI Agent 的物理 I/O 外设**——LLM 的手和脚。通过 USB 连接，Python Agent 发送 AT 命令即可: 输入键盘、读取传感器、控制 GPIO。它不是给人类用的 App，而是给 Agent 用的硬件接口。

> ⚠️ **安全警告**
>
> 本设备可模拟键盘，向电脑发送任意按键。**这意味着任何拥有串口访问权限的人都可以无限制地控制你的键盘。**
>
> - **切勿**将该设备连接到你不完全信任的电脑。
> - **切勿**将 AT 命令接口（UART / USB CDC）暴露给不信任的 AI Agent、公共网络或其他人的设备。
> - **始终**将物理串口视为特权管理控制台，而非玩具接口。

### 功能一览

| 功能 | 状态 | 说明 |
|------|------|------|
| **BLE 蓝牙键盘** | ✅ | HID over GATT，支持多主机（3 槽切换） |
| **USB 有线键盘** | ✅ | USB HID 设备（与 CDC 复合） |
| **AT 命令接口** | ✅ | 文本命令/响应协议（40+ 条命令） |
| **USB 虚拟串口** | ✅ | CDC ACM 虚拟 COM 口（VID 1A86 / PID 2107） |
| **BLE 接收器 (Dongle)** | ✅ | Central 角色：把任意 BLE 键盘桥接为 USB 键盘 |
| **GPIO 控制** | ✅ | 通过 AT 命令读写数字引脚 |
| **ADC 采样** | ✅ | 14 通道外部模拟输入，校准 mV 输出 |
| **I²C 传感器** | ✅ | 主机模式扫描/读/写（SCL=PB13, SDA=PB12） |
| **UART 串口** | ✅ | 硬件 UART1（TX=PA9, RX=PA8）调试输出 |
| **看门狗** | ✅ | AT+WDG 运行时开关 |
| **低功耗休眠** | ✅ | RTC 定时唤醒（BLE-only 构建，与 USB 互斥） |

### 构建方法

需要安装 MounRiver Studio 工具链（`riscv-none-embed-gcc`、`make`）并加入 PATH。

```bash
cd ch582/obj && make --no-print-directory main-build          # kbd（默认）
cd ch582/obj && make --no-print-directory main-build MODE=KBD_MULTI  # 多主机键盘
cd ch582/obj && make --no-print-directory main-build DONGLE=1 # BLE 接收器
```

输出文件：`ch582/obj/at-node.elf` / `.hex` / `.lst` / `.map`。

### 默认引脚

| 引脚 | 功能 | 说明 |
|------|------|------|
| PA0 | LED1 | 推挽输出，低电平亮 |
| PB22 | KEY1 | 上拉输入，低电平触发 |
| PB4 | KEY2 | 上拉输入，低电平触发 |
| PA9 (TXD1) | UART1 TX | 调试输出（115200 波特率） |
| PA8 (RXD1) | UART1 RX | 调试输入 |
| PB13 | I²C SCL | 上拉，100 kHz |
| PB12 | I²C SDA | 上拉 |
| PB10/PB11 | USB D+/D- | USB 2.0 FS（不可作 GPIO） |

### 项目结构

```
ch582/
├── APP/              # 应用层
│   ├── main.c           # 入口 + 7 阶段初始化
│   ├── at_cmds.c        # AT 命令表 + 键盘路由
│   ├── at_parser.c      # 行解析器（UART + CDC 双通道）
│   ├── hidkbd_ble.c     # BLE 键盘（广播/连接/报告）
│   ├── hidkbd_usb.c     # USB 键盘报告发送
│   ├── usb_dev.c        # USB 复合设备（CDC + HID）
│   ├── HWS/             # 硬件服务（KEY, LED, RTC, SLEEP, GPIO, ADC, I2C, WDG）
│   └── BLE/             # BLE GATT 服务（HID Dev, Battery, Device Info, Dongle）
├── LIB/              # BLE 协议栈（libCH58xBLE.a）
├── StdPeriphDriver/  # 外设驱动（GPIO、UART、I²C、ADC、USB……）
├── RVMSIS/           # RISC-V 内核访问层（NVIC、PFIC）
├── Startup/          # 复位向量与中断表
├── Ld/               # 链接脚本
└── obj/              # 构建输出目录
```

---

### License

MIT. Built with the [WCH CH583 SDK](https://www.wch.cn/).

### Roadmap

| Variant | Platform | Status |
|---------|----------|--------|
| `at-node` | CH582F BLE + USB | ✅ Active |
| `at-node-esp` | ESP32-C3 WiFi + BLE | ✅ Active |
| `at-node-nrf` | nRF52840 + Zephyr | 📋 Planned |

All variants share the same AT command protocol — Agent code works across platforms.
# at-node

**AT-command driven agent I/O module — the hands & feet of AI**

> Designed for **AI Agents**, not humans. No GUI, no app — just AT commands over serial.
> Connect an LLM to the physical world: type keystrokes, send IR, read sensors.

[🇬🇧 English](#english) · [🇨🇳 中文](#chinese)

---

## <a name="english"></a>🇬🇧 English

**at-node** is an IoT firmware for the [WCH CH582F](https://www.wch.cn/products/CH582.html) (RISC-V BLE 5.0 MCU) that bridges AT commands to keyboard input, sensor reading, and I/O control. It can be seamlessly integrated into AI Agents to perform simple computer operations — wake up a PC, type text, press hotkeys, or read environmental data.

> ⚠️ **Security Warning**
>
> This device emulates a keyboard and can send arbitrary keystrokes to your computer. **This is equivalent to giving anyone with serial access unrestricted keyboard control.**
>
> - **Do not** connect this device to a computer you do not fully trust.
> - **Do not** expose the AT command interface (UART / USB CDC) to untrusted AI agents, public networks, or other people's devices.
> - **Do not** leave the device in an unattended state with an active serial/CDC connection — a malicious script could execute arbitrary commands (open a terminal, run a reverse shell, install malware, etc.) in seconds.
> - **Always** consider the physical serial port as a privileged administrative console, not a toy interface.
>
> You are responsible for securing the physical and logical access to this device.

### Features

| Feature | Status | Description |
|---------|--------|-------------|
| **BLE HID Keyboard** | ✅ **Implemented** | Bluetooth keyboard via HID over GATT. Device name: "AT-Node" |
| **Wired (USB) Keyboard** | 🚧 *Planned* | USB HID device mode for wired operation |
| **AT Command Interface** | 🚧 *Planned* | Text-based command/response protocol over serial |
| **UART Serial Port** | ✅ **Implemented** | Hardware UART1 for debug output (TX), expandable to RX/TX |
| **USB CDC (Virtual Serial)** | 🚧 *Planned* | USB virtual COM port for AT commands and firmware updates |
| **GPIO Control** | 🚧 *Planned* | Read/write digital pins via commands |
| **ADC Sampling** | 🚧 *Planned* | External analog channel reading via commands |
| **I²C Sensor Scan** | 🚧 *Planned* | Scan and read I²C sensors (e.g. temperature, humidity) |
| **Remote Wake-on-LAN** | 🚧 *Planned* | Send Magic Packet or keystrokes to wake a sleeping PC |

> **Legend**: ✅ Implemented · 🚧 Planned (driver layer ready) · ⏳ Future

### How It Works

```
┌──────────────────────────────────────────────────┐
│                   AI Agent / Host                  │
│  (Python script, automation tool, LLM pipeline)   │
└──────────┬───────────────────────────┬────────────┘
           │ AT Commands               │ AT Commands
           ▼                           ▼
┌──────────────────────┐   ┌──────────────────────┐
│   UART / USB CDC     │   │   USB HID (Wired)    │
│   (control channel)  │   │   (keyboard out)     │
└──────────┬───────────┘   └──────────────────────┘
           │
           ▼
┌──────────────────────────────────────────────────┐
│                   at-node (CH582F)                 │
│  ┌────────┐ ┌─────────┐ ┌──────┐ ┌────────┐     │
│  │AT Parser│ │BLE HID  │ │I²C   │ │GPIO/ADC│     │
│  │        │ │Keyboard │ │Sensor│ │Control │     │
│  └────────┘ └─────────┘ └──────┘ └────────┘     │
└──────────────────────────────────────────────────┘
```

### Hardware

| Current | Planned |
|---------|---------|
| **CH582F** (RISC-V rv32imac, 60 MHz) | **CH592** (RISC-V, lower power, lower cost) |
| 448 KB Flash / 32 KB RAM | Smaller flash/ram footprint |
| BLE 4.2/5.0 | BLE 5.4 |
| Multi GPIO, I²C, SPI, ADC, PWM, USB | Same peripheral set |

The project is designed with a hardware abstraction layer so the same firmware can be ported across the WCH BLE MCU family, enabling the lowest possible BOM cost and power consumption for each use case.

### Build

Requires MounRiver Studio toolchain (`riscv-none-embed-gcc`, `make`) on PATH.

```bash
cd ch582/obj && make --no-print-directory main-build
```

Output: `ch582/obj/at-node.elf` / `.hex` / `.lst` / `.map`.

### Pinout (Default)

| Pin | Function | Notes |
|-----|----------|-------|
| PA8 | LED1 | Push-pull output, active low |
| PB22 | KEY1 | Pull-up input, active low |
| PB4 | KEY2 | Pull-up input, active low |
| PA4 (TXD1) | UART1 TX | Debug output (115200 baud) |
| — | I²C | TBD (to be assigned) |
| — | ADC | TBD (to be assigned) |

### Project Structure

```
ch582/
├── APP/              # Application layer (AT parser, HID keyboard logic)
├── HAL/              # Hardware abstraction (KEY, LED, RTC, SLEEP, MCU)
├── Profile/          # BLE GATT profiles (HID, Battery, Device Info)
├── LIB/              # BLE stack (libCH58xBLE.a)
├── StdPeriphDriver/  # Peripheral drivers (GPIO, UART, I²C, ADC, USB...)
├── RVMSIS/           # RISC-V core access layer (NVIC, PFIC)
├── Startup/          # Reset vector & interrupt table
├── Ld/               # Linker script
└── obj/              # Build output
```

---

## <a name="chinese"></a>🇨🇳 中文

**at-node** 是 **AI Agent 的物理 I/O 外设**——LLM 的手和脚。通过 USB 连接，Python Agent 发送 AT 命令即可: 输入键盘、发射红外、读取传感器、控制 GPIO。它不是给人类用的 App，而是给 Agent 用的硬件接口。

> ⚠️ **安全警告**
>
> 本设备可模拟键盘，向电脑发送任意按键。**这意味着任何拥有串口访问权限的人都可以无限制地控制你的键盘。**
>
> - **切勿**将该设备连接到你不完全信任的电脑。
> - **切勿**将 AT 命令接口（UART / USB CDC）暴露给不信任的 AI Agent、公共网络或其他人的设备。
> - **切勿**在无人值守的情况下保持串口/CDC 连接——恶意脚本可以在几秒内执行任意命令（打开终端、运行反弹 Shell、安装恶意软件等）。
> - **始终**将物理串口视为特权管理控制台，而非玩具接口。
>
> 你有责任确保本设备的物理和逻辑访问安全。

### 功能一览

| 功能 | 状态 | 说明 |
|------|------|------|
| **BLE 蓝牙键盘** | ✅ **已实现** | 通过 BLE HID over GATT 模拟键盘，设备名 "AT-Node" |
| **有线 USB 键盘** | 🚧 *计划中* | USB HID 设备模式，即插即用 |
| **AT 命令接口** | 🚧 *计划中* | 基于串口的文本命令/响应协议 |
| **UART 串口** | ✅ **已实现** | 硬件 UART1 调试输出（TX），可扩展为收发 |
| **USB 虚拟串口** | 🚧 *计划中* | USB CDC 虚拟 COM 口，用于 AT 命令与升级 |
| **GPIO 控制** | 🚧 *计划中* | 通过命令读写数字引脚 |
| **ADC 采样** | 🚧 *计划中* | 通过命令读取外部模拟通道 |
| **I²C 传感器扫描** | 🚧 *计划中* | 扫描并读取 I²C 传感器（温湿度等） |
| **远程唤醒电脑** | 🚧 *计划中* | 发送 Magic Packet / 模拟按键唤醒睡眠中的 PC |

> **图例**: ✅ 已实现 · 🚧 计划中（驱动层已就绪）· ⏳ 后续规划

### 工作方式

```
┌──────────────────────────────────────────────────┐
│                    AI Agent / 上位机                │
│      （Python 脚本、自动化工具、LLM 管线）           │
└──────────┬───────────────────────────┬────────────┘
           │ AT 命令                    │ AT 命令
           ▼                           ▼
┌──────────────────────┐   ┌──────────────────────┐
│   UART / USB CDC     │   │   USB HID (有线)      │
│   （控制通道）        │   │   （键盘输出）         │
└──────────┬───────────┘   └──────────────────────┘
           │
           ▼
┌──────────────────────────────────────────────────┐
│                   at-node (CH582F)                 │
│  ┌────────┐ ┌─────────┐ ┌──────┐ ┌────────┐     │
│  │AT 解析器│ │BLE 键盘  │ │I²C   │ │GPIO/ADC│     │
│  │        │ │         │ │传感器│ │控制    │     │
│  └────────┘ └─────────┘ └──────┘ └────────┘     │
└──────────────────────────────────────────────────┘
```

### 硬件规格

| 当前 | 规划中 |
|------|--------|
| **CH582F**（RISC-V rv32imac，60 MHz） | **CH592**（RISC-V，更低功耗、更低成本） |
| 448 KB Flash / 32 KB RAM | 更小的 Flash/RAM 需求 |
| BLE 4.2/5.0 | BLE 5.4 |
| 多路 GPIO、I²C、SPI、ADC、PWM、USB | 相同外设集 |

本项目设计了硬件抽象层（HAL），同一固件可移植至 WCH BLE MCU 全系列，针对不同场景实现最低 BOM 成本和功耗。

### 构建方法

需要安装 MounRiver Studio 工具链（`riscv-none-embed-gcc`、`make`）并加入 PATH。

```bash
cd ch582/obj && make --no-print-directory main-build
```

输出文件：`ch582/obj/at-node.elf` / `.hex` / `.lst` / `.map`。

### 默认引脚

| 引脚 | 功能 | 说明 |
|------|------|------|
| PA8 | LED1 | 推挽输出，低电平亮 |
| PB22 | KEY1 | 上拉输入，低电平触发 |
| PB4 | KEY2 | 上拉输入，低电平触发 |
| PA4 (TXD1) | UART1 TX | 调试输出（115200 波特率） |
| — | I²C | 待分配 |
| — | ADC | 待分配 |

### 项目结构

```
ch582/
├── APP/              # 应用层（AT 解析器、HID 键盘逻辑）
├── HAL/              # 硬件抽象层（按键、LED、RTC、休眠、MCU 初始化）
├── Profile/          # BLE GATT 服务（HID、电池、设备信息）
├── LIB/              # BLE 协议栈（libCH58xBLE.a）
├── StdPeriphDriver/  # 外设驱动（GPIO、UART、I²C、ADC、USB……）
├── RVMSIS/           # RISC-V 内核访问层（NVIC、PFIC）
├── Startup/          # 复位向量与中断表
├── Ld/               # 链接脚本
└── obj/              # 构建输出目录
```

---

### License

MIT. Built with the [WCH CH583 SDK](https://www.wch.cn/).

### Roadmap

| Variant | Platform | Status |
|---------|----------|--------|
| `at-node` | CH582F BLE | ✅ Active |
| `at-node-nrf` | nRF52840 + Zephyr | 📋 Planned |
| `at-node-esp` | ESP32-S3 + WiFi/TLS | 📋 Planned |

All variants share the same AT command protocol — Agent code works across platforms.
