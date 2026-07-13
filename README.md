# at-node

**AT-command driven IoT keyboard & sensor node**

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
| **BLE HID Keyboard** | ✅ **Implemented** | Bluetooth keyboard via HID over GATT. Device name: "Martillo-T00" |
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
cd software/obj && make --no-print-directory main-build
```

Output: `software/obj/at-node.elf` / `.hex` / `.lst` / `.map`.

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
software/
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

**at-node** 是基于 [WCH CH582F](https://www.wch.cn/products/CH582.html)（RISC-V BLE 5.0 MCU）的 IoT 固件，通过 AT 命令模拟键盘输入、读取传感器、控制 I/O。它可以无缝嵌入 AI Agent 之中，实现对电脑的基本远程操作——唤醒、打字、按快捷键，或读取环境数据。

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
| **BLE 蓝牙键盘** | ✅ **已实现** | 通过 BLE HID over GATT 模拟键盘，设备名 "Martillo-T00" |
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
cd software/obj && make --no-print-directory main-build
```

输出文件：`software/obj/at-node.elf` / `.hex` / `.lst` / `.map`。

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
software/
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

MIT — see [LICENSE](LICENSE) (if applicable). Built with the [WCH CH583 SDK](https://www.wch.cn/).
