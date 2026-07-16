# at-node 设计文档

> 版本：v0.2 · 最后更新：2026-07-16

---

## 0. 项目定位：CH582 生态开发模板

### 0.1 我们为什么要做这个

**性价比悖论。** CH582F — RISC-V 60MHz, BLE 4.2/5.0, 448K Flash, 32K RAM — 芯片单价 $0.50。nRF52840 — ARM Cortex-M4 64MHz, BLE 5.4 + 802.15.4, 1MB Flash, 256K RAM — 芯片单价 $2.50+。CH582 芯片成本是 nRF 的 **1/5**。

但开发成本是反过来的。nRF52840 有 Zephyr RTOS、nRF Connect SDK、Nordic 官方文档体系、数十个高质量示例项目。CH582 呢？WCH 官方 EVT 代码——单片式结构、命名不一致（`HAL_Init` / `HalLedSet` / `HAL_KEY_SW_1` / `halTaskID` 四种风格共存于同一文件）、中文注释混杂、错误处理缺失、无抽象层。

拿着 WCH EVT 代码做项目：80% 时间在理解和清理遗留代码，15% 时间在修 bug，5% 时间在写真正的业务逻辑。**芯片省下的 $2，在开发人力上亏掉 $2000。**

**at-node** 的设计初衷就是把 CH582 的性价比从纸面变成现实——用高质量的模板抹平开发成本差距。一个好的模板：

- 开发者拿到就能编译、烧录、看到效果（LED 闪、串口有输出、BLE 能连）
- 加个新功能（GPIO 控制、ADC 采样、I²C 传感器）有标准步骤，不用翻 EVT 代码猜
- 代码结构本身是文档——分层清、命名一致、注释是英文 ASCII
- 功耗是设计约束不是事后补丁——从 `config.h` 宏定义到 TMOS 事件粒度都考虑了低功耗

**at-node 的目标不是做一个"能用就行"的键盘固件，而是成为 CH582/CH592 系列 MCU 的高质量开发模板——让低成本芯片也能有接近 nRF 生态的开发体验。**

### 0.2 模板承诺

| 承诺 | 含义 |
|------|------|
| **可读性优先** | 代码即文档。新加入的开发者不需要通过中文注释来理解，结构本身就是表达 |
| **AI 原生可读** | 每个模块有文件头注释、分层 ASCII 图、CLAUDE.md 引导——AI 读完上下文直接能改代码 |
| **分层干净** | HWS→BLE→APP 三层分离，下层不知上层，依赖方向单向 |
| **命名一致** | 同层同风格，前缀统一，无 Hal/HAL/hal 混用 |
| **扩展点明确** | 加新 BLE Service？加新 AT 命令？加新外设？有标准步骤，不用翻源码猜 |
| **功耗是设计约束** | 不是事后优化。HWS_SLEEP 控制从宏定义到事件驱动都考虑了低功耗 |
| **纯 C99，零依赖** | 不绑 IDE（MounRiver 可以，GCC Makefile 也行），不绑 RTOS（TMOS 足够） |

### 0.3 谁应该用这个模板

- **初学者** — 从 `main.c` 初始化序列开始读，逐层深入理解 CH582 的开发模式
- **原型开发者** — 改 `config.h` 选芯片，在 `at_cmds.c` 加命令，在 `APP/` 加模块，快速出原型
- **产品团队** — 基于此模板建立自己的产品固件，HWS/BLE 层可复用，只改 APP 层
- **开源社区** — Fork 后加自己的 BLE Service、传感器驱动、AT 命令集

---

## 1. 设计原则

### 1.1 分层原则

```
┌──────────────────────────────────────────────────────┐
│  APP 层  — 应用逻辑，AT 命令，键盘管理，USB 描述符       │
│  依赖 → HWS + BLE + StdPeriphDriver                   │
├──────────────────────────────────────────────────────┤
│  BLE 层  — GATT Service 声明，HID/BATT/DEV_INFO 等    │
│  依赖 → HWS + BLE Stack (libCH58xBLE.a)               │
│  不依赖 → APP 层                                      │
├──────────────────────────────────────────────────────┤
│  HWS 层  — 硬件服务（Hardware Services）               │
│  KEY, LED, RTC, SLEEP, 温度传感器, 校准               │
│  依赖 → StdPeriphDriver + config.h                    │
│  不依赖 → BLE 层, APP 层                              │
├──────────────────────────────────────────────────────┤
│  StdPeriphDriver  — 外设寄存器驱动（WCH 提供）           │
│  GPIO, UART, I2C, ADC, SPI, PWM, USB, Flash           │
│  依赖 → RVMSIS + CH583SFR.h                           │
├──────────────────────────────────────────────────────┤
│  RVMSIS + Startup  — RISC-V 内核 + 启动向量            │
│  PFIC/NVIC, core_riscv, 中断表                        │
└──────────────────────────────────────────────────────┘
```

**核心规则：**

- **上层可依赖下层，下层不知上层。**
- **HWS 不依赖 BLE。** HWS 是纯粹的硬件寄存器操作，不包含任何协议栈逻辑。
- **BLE 不依赖 APP。** BLE Service 是独立的 GATT 服务声明，不知道谁在调用它。
- **APP 编排一切。** `main.c` 负责初始化顺序，`at_cmds.c` 负责业务逻辑。

### 1.2 HWS 纯净原则

HWS（Hardware Services）= 硬件寄存器访问层。不是 HAL（Hardware Abstraction Layer）的命名巧合——有刻意区别：

| 概念 | HWS（本项目） | 传统 HAL |
|------|-------------|---------|
| 职责 | 寄存器操作 + 事件循环包装 | 硬件抽象 + 驱动逻辑 |
| 协议栈耦合 | **零** | 经常有 |
| 可移植性 | 只绑 CH58x 寄存器 | 理论上跨平台 |
| TMOS 耦合 | 有（事件调度） | 通常无 |

HWS 故意不做跨平台 HAL。CH582 和 CH592 的寄存器兼容，抽象已经足够。跨到 nRF 或 ESP32 需要重写 HWS 层，APP/BLE 层才能复用——这是有意为之的设计边界。

### 1.3 命名规范

| 层级 | 公共函数前缀 | 静态函数前缀 | 宏前缀 | 头文件守卫 |
|------|------------|------------|--------|----------|
| HWS | `hws_` | `hws_` | `HWS_` | `__HWS_xxx_H` |
| BLE | `ble_` | `ble_` | `BLE_` | `__BLE_xxx_H` |
| APP | `kb_`, `AT_`, `USB_` | 不限制 | `KB_`, `AT_`, `USB_` | `__APP_xxx_H` |

**为什么不用 `HAL_`：**
- `HAL_` 是 WCH 原始代码的前缀，与 `Hal` 混用，混乱
- `HWS_` 语义更精确——这是 Hardware Services，不是完全的硬件抽象
- 与 `BLE_` 前缀形成对称：HWS 是硬件，BLE 是无线

### 1.4 AI 原生可读设计

不是事后写文档，而是代码本身携带 AI 所需的上下文。设计决策：

1. **CLAUDE.md 引导文件。** 仓库根目录的 CLAUDE.md 包含完整的分层架构、初始化序列、TMOS 任务注册表、命名规范、关键约束。AI 读完直接建立心智模型，不需要翻 20 个文件猜结构。
2. **每个模块的文件头注释。** 一句话说明职责 + 数据流向。例如 `at_parser.c` 的头注释直接写清了 UART 环形缓冲 → 行解析器 → CDC 回显 的完整数据流。
3. **分层 ASCII 图。** REQUIREMENTS.md 和 DESIGN.md 的架构图不是给人看的装饰——AI 从图里提取分层依赖关系和初始化顺序，精度比自然语言描述高。
4. **命名自解释。** `hws_led_set()` 不需要注释也知道是硬件服务层的 LED 设置。`ble_stack_init()` 不需要看源码也知道初始化 BLE 协议栈。前缀 = 层级，动词 = 操作。
5. **纯 ASCII 英文注释。** 没有 GB2312 乱码风险，所有文本编辑器、所有 AI 模型一致解析。
6. **CLAUDE.md + DESIGN.md 双重引导。** CLAUDE.md 提供分层架构和约束，DESIGN.md 提供设计哲学和扩展指南——AI 读完即建立完整心智模型。

**效果：** 一个没看过这个项目的 AI Agent，读完 CLAUDE.md + DESIGN.md + 目标文件的头注释，就能开始正确地修改代码。不需要人工解释"这个 HAL_SLEEP 其实是 HWS_SLEEP 但还没改完"这类上下文。

### 1.5 低功耗优先设计

低功耗不是"加上 sleep 宏"就完事。设计层面有几个关键决策：

1. **TMOS 事件粒度控制在 10ms+。** 没有 1ms 的忙轮询。按键扫 100ms，AT 命令轮询 10ms，LED 闪烁按 duty cycle 算下次唤醒时间。
2. **USB 和 SLEEP 编译期互斥。** `HWS_SLEEP=TRUE` → USB 不初始化。运行期 `AT+SLEEP` 先关 USB 再睡。没有"先睡再发现 USB 挂了"的运行时惊喜。
3. **校准任务合并到一个事件。** RF 校准 + LSI 校准同频同周期，不碎片化定时器。
4. **LED 闪烁利用 TMOS 延迟启动。** 不阻塞等待，计算下次触发时间后立即让出 CPU。

---

## 2. 目录结构

```
software/
├── APP/                           # 应用层
│   ├── main.c                     # 入口 + 初始化序列
│   ├── ble_init.c                 # BLE Peripheral 初始化聚合
│   ├── at_cmds.c                  # AT 命令表 + 处理函数 + 键盘路由
│   ├── at_parser.c                # AT 解析器（UART+CDC 双通道）
│   ├── hidkbd_ble.c               # BLE 键盘广播/连接/报告
│   ├── hidkbd_usb.c               # USB 键盘报告发送
│   ├── usb_dev.c                  # USB 复合设备（CDC+HID）
│   ├── include/
│   │   ├── config.h               # 全局配置宏（芯片/功耗/内存）
│   │   ├── at_parser.h            # AT 解析器接口
│   │   ├── ble_init.h             # BLE 初始化接口
│   │   ├── hidkbd_common.h        # 键盘路由层接口
│   │   ├── hidkbd.h               # BLE 键盘事件定义
│   │   └── usb_dev.h              # USB 设备接口
│   ├── HWS/                       # 硬件服务层
│   │   ├── hws.h                  # 伞形头文件
│   │   ├── hws_core.c             # 初始化 + TMOS 事件处理 + 温度 + 校准
│   │   ├── hws_key.c / hws_key.h  # 按键轮询
│   │   ├── hws_led.c / hws_led.h  # LED 控制
│   │   ├── hws_rtc.c / hws_rtc.h  # RTC 定时
│   │   └── hws_sleep.c / hws_sleep.h  # 睡眠管理
│   └── BLE/                       # BLE GATT 服务层
│       ├── ble_stack.c / ble_stack.h      # BLE 协议栈初始化
│       ├── ble_hid_dev.c / ble_hid_dev.h  # HID Device GATT Service
│       ├── ble_hid_kbd.c / ble_hid_kbd.h  # HID Keyboard GATT Service
│       ├── ble_batt.c / ble_batt.h        # Battery Service
│       ├── ble_dev_info.c / ble_dev_info.h # Device Information Service
│       └── ble_scan_param.c / ble_scan_param.h # Scan Parameters Service
├── StdPeriphDriver/               # WCH 外设驱动（只读）
├── LIB/                           # BLE 协议栈二进制库
├── RVMSIS/                        # RISC-V 内核访问
├── Startup/                       # 启动向量 + 中断表
├── obj/                           # 构建输出
├── DESIGN.md                      # 本文件 — 设计文档
├── REQUIREMENTS.md                # 需求文档
└── POWER.md                       # 低功耗设计指南
```

---

## 3. 核心模块设计

### 3.1 初始化序列

```c
int main(void) {
    hws_platform_init();        // 1. 电源 + 时钟 + GPIO + 调试串口
    ble_stack_init();           // 2. BLE 协议栈（此步初始化 TMOS 调度器！）
    hws_init(key_press);        // 3. HWS TMOS 任务 + RTC/SLEEP/LED/KEY
    at_init();                  // 4. AT 命令解析器（UART1 + CDC）
    ble_peripheral_init();      // 5. GAP + GATT Service + 开始广播
    usb_init();                 // 6. USB 复合设备 或 睡眠模式
    main_loop();                // 7. TMOS_SystemProcess() 无限循环
}
```

**初始化顺序是精心设计的：** Stage 2 必须在 Stage 3 之前——`BLE_LibInit()` 初始化 TMOS 调度器，而 `hws_init()` 调用 `TMOS_ProcessEventRegister()` 依赖 TMOS 已就绪。`usb_init()` 封装了 USB/睡眠的编译期互斥（`HWS_SLEEP`）。`ble_peripheral_init()` 聚合了 GAP 角色 + HID Device + HID Emu + 广播启动，在 [ble_init.c](APP/ble_init.c) 中实现。

### 3.2 TMOS 任务注册表

| 任务 ID | 注册函数 | 事件类型 | 典型周期 |
|---------|---------|---------|---------|
| hws_task_id | `hws_init()` | HWS_KEY_EVENT, HWS_LED_BLINK_EVENT, HWS_CALIB_EVENT | 100ms / on-demand / 120s |
| ble_hid_dev_task_id | `ble_hid_dev_init()` | GATT 状态机 | 事件驱动 |
| ble_hid_emu_task_id | `ble_hid_emu_init()` | BLE 广播/连接/参数更新 | 事件驱动 |
| at_task_id | `at_init()` | AT_EVENT | 10ms |

### 3.3 AT 命令系统

**设计目标：** AI Agent 通过文本协议控制硬件。不是给人用的 UI，是给 LLM 用的 API。

```
AT 命令 → at_parser.c（行缓冲 + 参数分词）
       → at_cmds.c 命令表查找 + handler 执行
       → kb_* 函数（键盘路由层）
       → kb_flush() 按 kb_mode 分发
       → BLE: kb_ble_send_report() → ble_hid_dev_report()
       → USB: kb_usb_send_report() → USB_HID_SendReport()
```

**通道模型：**

- UART1 → 环形缓冲区 → 行解析器 → 命令处理 → 响应回 UART1
- CDC   → USB_CDC_Read() → 行解析器 → 命令处理 → 响应回 CDC（先回显输入行）

两个通道共享同一个命令表，响应路由回来源通道。未来 BLE NUS 添加第三个通道——只需加一个 `AT_CH_BLE` 通道标记和对应的 `at_write_ble()` 函数。

### 3.4 扩展指南

#### 添加新 AT 命令

1. 在 `at_cmds.c` 中写 handler 函数：`static int at_cmd_XXX(int argc, char *argv[])`
2. 在 `cmd_table[]` 数组中添加一行：`{ "AT+XXX", "description", at_cmd_XXX }`
3. 完成。解析器自动处理 `=`/`,` 分词和 `OK`/`ERROR` 响应。

#### 添加新 BLE Service

1. 在 `APP/BLE/` 下创建 `ble_xxx.c` / `ble_xxx.h`
2. 实现 `ble_xxx_init()` 注册 GATT Service
3. 头文件只声明 `ble_xxx_init()` 和必要的 get/set 函数
4. 在 `main.c` 初始化序列中加入 `ble_xxx_init()`
5. **不要**在 Service 文件中直接操作 GPIO 或依赖 APP 层符号

#### 添加新硬件外设

1. 在 `APP/HWS/` 下创建 `hws_xxx.c` / `hws_xxx.h`
2. 只操作对应外设的寄存器（通过 StdPeriphDriver API）
3. 如需周期性事件，在 `hws_process_event()` 中注册新事件
4. 在 `hws_init()` 中加入初始化调用
5. **不要**在 HWS 层引用 `at_cmds`、`usb_dev`、BLE Service 等上层符号

---

## 4. 关键约束

### 4.1 USB 与低功耗互斥

CH582F 硬件限制。进入任何休眠模式（Sleep/Shutdown）后 USB 时钟停止 -> 主机侧设备消失（错误代码 43）。

**应对策略：**

| 场景 | HWS_SLEEP | USB | 通信通道 |
|------|-----------|-----|---------|
| 桌面开发 | FALSE | 启用 | USB CDC + USB HID |
| AI Agent 有线 | FALSE | 启用 | USB CDC AT 命令 |
| 电池低功耗 | TRUE | 禁用 | 硬件 UART AT 命令 |
| BLE 双模键盘 | FALSE | 启用 | USB + BLE 双模 |

编译期决策：`#if HWS_SLEEP == TRUE` 跳过 USB 初始化。运行时 `AT+SLEEP` 需先调用 USB 关闭序列。

### 4.2 BLE 绑定存储

- SNV 地址：`0x77E00`（Data Flash 最后 512B）
- 最多 1 个绑定设备，新配对覆盖旧绑定
- SNV 读写通过 `EEPROM_READ`/`EEPROM_WRITE`，由 BLE 库通过回调 (`readFlashCB`/`writeFlashCB`) 调用

### 4.3 内存布局

```
RAM (32KB)
├── .data / .bss     # 全局变量
├── Stack            # ↓ 向下增长
├── Heap             # ↑ 向上增长（malloc 用）
└── MEM_BUF[]        # BLE 协议栈专用堆 (≥6KB)
```

Flash (448KB)
```
0x00000 ┌──────────────┐
        │ Startup + APP │  ~440KB
0x77E00 ├──────────────┤
        │ Data Flash    │  2KB (SNV 用最后 512B)
0x78000 └──────────────┘
```

---

## 5. 编码规范

### 5.1 文件头

每个 `.c`/`.h` 文件必须包含：

```c
/********************************** (C) COPYRIGHT *******************************
 * File Name          : xxx.c
 * Author             : at-node
 * Description        : 一行说明
 ********************************************************************************/
```

### 5.2 函数注释

```c
/**
 * @brief  函数功能的一句话描述
 *
 * @param  param1 - 参数说明
 * @param  param2 - 参数说明
 *
 * @return 返回值说明
 */
```

### 5.3 命名规则

| 元素 | 规则 | 示例 |
|------|------|------|
| 公共函数 | 层级前缀 + 小写 + 下划线 | `hws_led_set()`, `ble_stack_init()` |
| 静态函数 | 同上，无特殊前缀 | `static void kb_flush(void)` |
| 公共宏 | 层级前缀 + 大写 + 下划线 | `HWS_LED_MODE_ON`, `BLE_HID_RPT_ID_KEY_IN` |
| 私有宏（仅在 .c 内用） | 小写或大写，前后一致 | `AT_RX_BUF_SIZE`, `EP0_SIZE` |
| 类型 | 层级前缀 + 小写 + `_t` | `hws_key_cb_t`, `kb_mode_t` |
| 枚举值 | 层级前缀 + 大写 | `KB_USB`, `KB_BLE`, `KB_BOTH` |

### 5.4 注释语言

- **代码注释：英文。** 所有注释以 ASCII 书写，键值、寄存器名、协议字段保留原文。
- **设计文档：中文/英文不限。** 面向不同读者群。

---

## 6. 版本历史

| 版本 | 日期 | 内容 |
|------|------|------|
| v0.1 | 2026-07-13 | BLE HID 键盘基础功能，单体结构 |
| v0.2 | 2026-07-16 | 重构：HWS/BLE/APP 三层分离，命名统一 `hws_`/`ble_` 前缀，设计哲学文档化 |

---

## 7. 参考

- [CH583 数据手册](https://www.wch.cn/products/CH583.html)
- [USB HID Usage Tables](https://www.usb.org/hid)
- [BLE HID over GATT Profile](https://www.bluetooth.com/specifications/specs/hid-over-gatt-profile/)
- [WCH 官方 EVT 代码](../EVT/) — 参考但不复用其代码结构
