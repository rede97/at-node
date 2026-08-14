# ATNode

**AT-command driven agent I/O node — the hands & feet of AI**

> Designed for **AI Agents**, not humans. No GUI, no app — just AT commands.
> One command semantics, multiple hardware platforms.

[🇬🇧 English](#english) · [🇨🇳 中文](#chinese)

---

## <a name="english"></a>🇬🇧 English

**ATNode** is a family of firmware variants that bridge AT commands to keyboard input,
sensor reading, and I/O control. An LLM agent drives the physical world through serial,
USB CDC, WiFi HTTP, or MQTT — same command semantics on every platform.

> ⚠️ **Security Warning**
>
> These devices emulate a keyboard and can send arbitrary keystrokes. **Anyone with
> control-channel access gets unrestricted keyboard control.**
> Do not attach to computers you do not trust; do not expose the AT/HTTP interface
> to untrusted networks or agents; treat the control channel as a privileged console.

### Platform variants

| Series | Variant | Chip | Stack | Control plane | Status |
|---|---|---|---|---|---|
| WCH BLE | [`wchble/mr2/`](wchble/mr2/) | CH582F | MounRiver Studio 2, bare-metal + TMOS | USB CDC + UART | ✅ Active |
| ESP32 | [`esp32/arduino/`](esp32/arduino/) | ESP32-C3, classic ESP32 | Arduino-ESP32 | WiFi HTTP + MQTT TLS | ✅ Active |
| ESP32 | [`esp32/zephyr/`](esp32/zephyr/) | ESP32-S3 & PSRAM chips | Zephyr | WiFi HTTP + MQTT TLS | 📋 TODO |
| Nordic | [`nordic/zephyr/`](nordic/zephyr/) | nRF52840 | Zephyr (nRF Connect SDK) | USB CDC | 📋 TODO |

Cross-hardware requirement differences are recorded as **final decisions** with
pointers in [REQUIREMENTS.md](REQUIREMENTS.md) §4. Per-chip hardware info and
issue records live in each platform directory — see the doc map below.

### Doc map

| Doc | Content |
|---|---|
| [REQUIREMENTS.md](REQUIREMENTS.md) | Consolidated requirements, reclassified by platform; decision registry §4 |
| [DESIGN.md](DESIGN.md) | Cross-platform design philosophy |
| [AGENTS.md](AGENTS.md) | Agent-facing repo manual (architecture, build, conventions) |
| [AUDIT.md](AUDIT.md) | Code audit report (2026-07-26) |
| [wchble/README.md](wchble/README.md) | WCH BLE series overview → CH582 docs (hardware, manual, field notes, power) |
| [esp32/README.md](esp32/README.md) | ESP32 series overview → Arduino/Zephyr variants, S3 decision, compat report |
| [nordic/README.md](nordic/README.md) | Nordic series overview |
| [tools/README.md](tools/README.md) | Test/broker/CI tooling |

---

## <a name="chinese"></a>🇨🇳 中文

**ATNode** 是 **AI Agent 的物理 I/O 外设** 固件家族——LLM 的手和脚。
一套 AT 命令语义，多个硬件平台：Agent 脚本跨平台复用，换硬件只换传输层。

> ⚠️ **安全警告**
>
> 本设备可模拟键盘，向电脑发送任意按键。**任何拥有控制通道访问权限的人都可以
> 无限制地控制你的键盘。** 不要连接不信任的电脑；不要把 AT/HTTP 接口暴露给
> 不可信的网络或 Agent；始终把控制通道当作特权管理控制台。

### 平台变体

| 系列 | 变体 | 芯片 | 栈 | 主控制面 | 状态 |
|---|---|---|---|---|---|
| WCH BLE | [`wchble/mr2/`](wchble/mr2/) | CH582F | MounRiver Studio 2，裸机 + TMOS | USB CDC + UART | ✅ Active |
| ESP32 | [`esp32/arduino/`](esp32/arduino/) | ESP32-C3、原版 ESP32 | Arduino-ESP32 | WiFi HTTP + MQTT TLS | ✅ Active |
| ESP32 | [`esp32/zephyr/`](esp32/zephyr/) | ESP32-S3 等 PSRAM 机型 | Zephyr | WiFi HTTP + MQTT TLS | 📋 TODO |
| Nordic | [`nordic/zephyr/`](nordic/zephyr/) | nRF52840 | Zephyr（nRF Connect SDK） | USB CDC | 📋 TODO |

跨硬件需求差异只登记**最终决定**并指向平台文档：见
[REQUIREMENTS.md](REQUIREMENTS.md) §4。各芯片硬件信息与问题原因记录在各平台目录内。

### 快速开始

| 平台 | 构建/刷机 |
|---|---|
| CH582（wchble/mr2） | `cd wchble/mr2/obj && make --no-print-directory main-build`（需 MounRiver 工具链，`source env.sh`） |
| ESP32-C3 SuperMini | `esp32/arduino/build-c3.ps1 -Port COMx` |
| 标准 ESP32 | `esp32/arduino/build-esp32.ps1 -Port COMx` |

Agent 刷机**默认使用板卡专用脚本**（`build-c3.ps1` / `build-esp32.ps1`），
禁止裸 arduino-cli / IDE 默认路径——原因见 [REQUIREMENTS.md](REQUIREMENTS.md) §4 D8。

### 远程控制

📡 云 broker（MQTT + HTTP 代理）快速上手：[`tools/broker/GET_START.md`](tools/broker/GET_START.md)

### License

MIT. CH582 变体基于 [WCH CH583 SDK](https://www.wch.cn/) 构建。
