# Zephyr → Rust 迁移任务准则（esp32/rust/）

> 版本：v1.0 · 2026-08-20
> 性质：**约束文档**。后续所有 Rust 版开发任务以此为准；偏离本文档的决策必须
> 先改文档（记录原因），再动代码。
> 前身：ESP32-S3 Zephyr 实现（已归档 commit `e759a2a`，其 README 的 bug 表
> 是本文档 §6 的来源）。

---

## 1. 背景与动机

Zephyr 路线已放弃（退休 commit `c1de43e`），原因：ESP32 在 Zephyr 是二等公民——
WiFi/BLE 二进制 blob 崩溃无源码符号、特性组合零上游测试、Kconfig/API 主线漂移、
错误信息不指向根因。S3 固件转向 **esp-rs 裸机生态（esp-hal + Embassy)**。

Zephyr 版留下三种可复用资产：

1. **需求与语义**：完整 AT 命令集、HTTP 路由表、MQTT topic 模型、配置键空间
   （权威来源：esp32/arduino/，次权威：`e759a2a` 的 src/at_core.c + httpd.c)。
2. **硬件事实**:nanoESP32-S3 引脚分配、USB 单 PHY 双控制器、PSRAM 40MHz、
   ESPLink 串口复位特性、DWC2 注意事项（见 §6)。
3. **已验证的实现模式**：配对窗口状态机、kbd 路由位掩码、cfg 注册表语义、
   WiFi 看门狗节奏。

## 2. 目标与非目标

### 目标（与 Zephyr 版对齐，验收标准不降低）

- AT 命令语义与 esp32/arduino 完全一致（serial + HTTP + MQTT 三通道等价）。
- 键盘输出：BLE HID(boot keyboard,60s 配对窗口 + bonded-only 重连）
  + USB HID,`AT+DEV=USB|BLE|ALL` 位掩码路由；tap/text 原子 press+release。
- 控制面：HTTP `/at-node/*` 全路由 + 共享 SPA(`esp32/arduino/web_page.h`
  的 gzip 字节原样复用，前端零改动）+ MQTT TLS(CA 强校验）。
- 配置：NVS 持久化注册表，键空间与 Arduino 对齐，密钥 write-only。
- GPIO/ADC/I2C 命令；WS2812 状态灯（预设 + `AT+LED` 自由色）。
- 全程可在真实硬件上分阶段验证，每个阶段交付可烧录、可演示的固件。

### 非目标（明确不做）

- 不追求与 Zephyr 版代码结构相似——按 Rust/Embassy 惯用结构重新组织。
- 不做 FreeRTOS / ESP-IDF 路线（std on ESP-IDF 被否决：继承 FreeRTOS 包袱）。
- IR 暂不做（esp-hal 有 RMT，可做，但排在外设阶段之后单独评估）。
- rathole、AP 配网页、mDNS 不做（与 Zephyr 版 delta 一致）。
- 不维护与 Zephyr 版的兼容性/双份实现——Zephyr 版已归档，禁止回头修补。

## 3. 技术选型（已决策）

| 领域 | 选型 | 理由 / 约束 |
|---|---|---|
| 语言/目标 | Rust no_std,`xtensa-esp32s3-none-elf`,espup toolchain(`~/export-esp.sh`) | esp-rs 官方路线 |
| 异步运行时 | `embassy-executor`（双核各一个 executor 或单核起步） | 事实标准；不引 RTIC |
| HAL | `esp-hal`(S3)+ `esp-alloc`(PSRAM 堆）+ `esp-println` + `esp-backtrace` | panic 必须带 Rust 栈回溯 |
| WiFi/BLE | `esp-radio`(WiFi STA + BLE 共存）| 官方；BLE host 用 `trouble` |
| BLE HID | `trouble` HID over GATT(boot keyboard,report map 照抄 Zephyr 版描述符） | 配对语义复刻 §5.2 |
| USB HID | `esp-hal` USB-OTG + `usb-device` + `usbd-hid` | 注意 §6 PHY 约束 |
| MQTT | ~~`rust-mqtt`~~ → **自实现 MQTT 3.1.1 mini client**(mqttc.rs 内置)+ `embedded-tls`(pki 验证器,CA DER 内嵌) | 2026-08-20 R3 变更:rust-mqtt 0.3 v3 全 stub、0.5 v3 模块为空;atnode broker(amqtt,含云端)协议级别=4 只讲 3.1.1。embedded-tls 用 pki 不用 webpki:ring 不为 xtensa 构建。不碰 mbedTLS |
| HTTP | `picoserve`（静态 gzip SPA + JSON 路由） | 无 std HTTP server |
| NVS 配置 | `esp-storage` + `sequential-storage`(map 模式） | 键值语义对齐 cfg.c |
| WS2812 | `esp-hal-smartled`(RMT) | 异步 buffer 模式，勿阻塞写 |
| 构建/烧录 | `cargo build --release` + `espflash flash --monitor` | 串口纪律见 §5.6 |

选型变更必须改本表并记录原因。

## 4. 目录与模块结构

```
esp32/rust/
├── MIGRATION.md          # 本文档
├── Cargo.toml
├── rust-toolchain.toml
├── .cargo/config.toml    # target/runner/链接参数(esp-generate 产出)
├── build.rs
└── src/
    ├── main.rs           # 启动:时钟/PSRAM/看门狗/executor 拉起,任务 spawn
    ├── cfg.rs            # NVS 配置注册表(对齐 Zephyr 版 cfg.h 语义)
    ├── at.rs             # AT 解析/分发核心(通道无关,async)
    ├── at_serial.rs      # UART0 控制台
    ├── kbd/mod.rs        # 路由层(位掩码)+ tap/text 序列任务
    ├── kbd/ble.rs        # trouble BLE HID + 配对状态机
    ├── kbd/usb.rs        # usbd-hid 键盘
    ├── wifi.rs           # STA + 15s 重连看门狗
    ├── httpd.rs          # picoserve 路由 + SPA
    ├── mqttc.rs          # MQTT/TLS + LWT + cmd->resp 回环
    ├── hws.rs            # GPIO/ADC/I2C(引脚黑名单照抄)
    ├── led.rs            # WS2812 状态灯
    └── web_page.rs       # include_bytes! 共享 SPA gzip
```

约束：**一个 crate**，不分 workspace（规模不够）；模块边界 = 上表；
禁止跨模块直接访问全局可变状态，任务间通信用 embassy Channel/Signal。

## 5. 工程纪律

### 5.1 AT 语义对齐（最高优先级）

- 每个命令实现时对照 esp32/arduino/arduino.ino 的对应分支；参数顺序、
  取值范围、响应行格式、错误文案逐一对齐。响应约定：数据行在前，
  以恰好一行 `OK` / `ERROR <reason>` 结尾。
- 三通道（serial/HTTP/MQTT）必须调用**同一个** `at::handle_line()`，
  禁止通道各自实现命令。
- 注入纪律沿用 FIELD-NOTES F18：常规注入走 tap/text 序列（自动配对
  press/release)，裸 `AT+KEY` 必须文档提示 `AT+KEY=0,0` 释放。

### 5.2 并发与 ISR 纪律（Zephyr bug #1 的教训）

- **中断/回调上下文只做 `Signal::signal` / `channel.try_send`**,
  绝不执行 SPI/I2C/UART 事务、绝不阻塞等待。所有外设事务在 async 任务里。
- 定时需求用 `embassy_time::Timer`(async 任务内）,禁止"定时器回调里干活"
  的 Zephyr 模式。
- 共享外设：`&'static Mutex<CriticalSectionRawMutex, T>`；持锁期间不 await。
- BLE/USB 事件回调（非 async 上下文）只投递事件到 Channel，状态机在任务里。

### 5.3 内存策略

- `esp-alloc` 开 PSRAM 堆；>4KB 的 buffer(SPA 副本、TLS IO、HTTP 缓冲）
  显式分配在 PSRAM；内部 RAM 优先留给 WiFi/BLE 驱动和 DMA。
- WiFi 驱动启动前打印内部 RAM 余量；每个阶段验收时记录堆水位（对比 Zephyr
  版 DRAM 99% 的窘境，Rust 版内部 RAM 余量应显著更宽）。
- 禁止每命令堆分配：AT 行缓冲、响应缓冲用静态/池化 buffer。

### 5.4 TLS 与网络

- `embedded-tls` + CA DER 内嵌（`tools/gen_certs.sh` 方案沿用，
  SAN 含 broker LAN IP);TLS 校验失败必须报可读错误，禁止静默 insecure。
- MQTT:client_id/topics/LWT/retained 语义照抄 Zephyr 版 mqttc.h 契约；
  **断线重连必须正确释放上一次 socket**(Zephyr bug #3 的教训：
  重试泄漏连接上下文）——每次重连前 `drop` 旧连接并确认资源回收。
- WiFi 看门狗：断线 15s 重试节奏；链路恢复事件经 Signal 通知 MQTT/HTTP。

### 5.5 测试与验收

- 每个阶段（§7）交付 = 编译绿 + 硬件冒烟脚本/步骤 + 结果记录进 README。
- HTTP 冒烟复用 curl 命令集（对齐 Zephyr 版验证过的端点清单）;
  MQTT 冒烟复用 `tools/mqtt_probe.py` 思路（broker + cmd/resp 回环）。
- 前端兼容验收：浏览器开 SPA,Status/BLE/MQTT/Config 页字段全部正常
  （字段名/类型对照 esp32/arduino/web/app.js，布尔 vs 字符串是易错点）。
- 禁止"编译通过即完成"。禁止用 mock/仿真代替硬件验证。

### 5.6 串口与烧录纪律

- ESPLink 串口打开即复位：调试期用常驻控制台（复用
  `esp32/zephyr/tools/at_console.py` 的模式或 espflash monitor 常驻）,
  烧录前必须停掉占用串口的进程（Zephyr bug #8)。
- 原生 USB 口：固件一旦启用 USB OTG，该口不再是 JTAG/下载口（§6 PHY 事实）;
  需要 probe-rs/JTAG 调试时固件先关 USB OTG。

### 5.7 代码规范

- no_std;`#![no_main]` esp-hal 入口。注释 ASCII-only，中文注释仅限
  迁移注释（引用 Zephyr 版语义时）。
- clippy 零警告（`cargo clippy --release -- -D warnings`）作为每个阶段的
  提交门槛；不允许 `unwrap()` 出现在库路径（main/任务入口允许 expect 带信息）。
- 每个模块头部注释：职责 + 对齐的 Zephyr 版文件/Arduino 语义出处。

## 6. 硬件事实与 Zephyr 教训（迁移时必须遵守）

| # | 事实/教训 | Rust 版约束 |
|---|---|---|
| H1 | S3 单 USB PHY,USB-Serial-JTAG 与 USB OTG 互斥；固件用 OTG 后原生口失去 JTAG/下载能力 | 烧录/调试走 ESPLink 口；JTAG 需求时固件关 OTG |
| H2 | DWC2 在 Zephyr 的 experimental DMA 有 cache 空指针问题 | Rust 版用 esp-hal USB 驱动，若见同类 cache 报错先记录再排查 |
| H3 | **ISR 里做 SPI 会腐蚀调度器**(Zephyr bug #1,16 秒后无辜线程 panic) | §5.2 纪律；评审时专门查"回调里的事务调用" |
| H4 | 重连泄漏 sockets(Zephyr bug #3) | §5.4;MQTT 重连路径必须配对 drop，写泄漏测试 |
| H5 | p256m 与 TLS 的 ECDHE 冲突(Zephyr bug #2) | Rust 版无 mbedTLS；用 embedded-tls 的纯软件 ECC，同类"两个子系统各自正常、组合死"的坑在集成阶段逐一验证 |
| H6 | 引脚黑名单：0,3,19,20,26-32,33-37,43,44,45,46(strap/flash/PSRAM/USB/UART0) | hws.rs 照抄；GPIO 命令复用同一校验表 |
| H7 | PSRAM 八线 40MHz(80MHz 需 timing tuning) | esp-hal PSRAM 默认配置即可，勿自行超频 |
| H8 | WiFi 驱动 boot 连接窗口不可靠，需循环重试看门狗 | wifi.rs 15s 节奏照抄 |

## 7. 阶段计划（每阶段 = 可烧录可演示的固件）

| 阶段 | 内容 | 验收 |
|---|---|---|
| R0 | esp-generate 骨架：S3 + Embassy + esp-println + esp-backtrace,WS2812 点亮，UART0 echo | 烧录启动，串口可见 banner,LED 亮 |
| R1 | cfg.rs(esp-storage 注册表）+ at.rs 核心 + at_serial + AT/VER/HELP/SET/GET/KEYS/RST + LED 预设/自由色 | 串口全命令通过；复位后配置保持 |
| R2 | wifi.rs(STA + 看门狗）+ AT+WIFI/STATUS | 冷启动连接、拔插 AP 自愈、RSSI/IP 上报 |
| R3 | mqttc.rs(TLS + LWT + cmd→resp)+ AT+MQTT 系列 | 本地 TLS broker 回环全通；杀 broker 重连 20 轮无泄漏（H4 测试）【✅ 2026-08-20 通过】 |
| R4 | httpd.rs 全路由 + 共享 SPA + config/ble/wifi/mqtt 端点 | curl 端点清单全过；浏览器 SPA 三页正常【✅ 2026-08-20 通过；ble/kbd 端点随 R5/R6 补】 |
| R5 | kbd/usb.rs(usbd-hid)+ kbd 路由 + AT+TAP/KEY/KEY_STR/KEY_SEQ/DEV | 主机枚举 + 打字实测（用户验收） |
| R6 | kbd_ble.rs(trouble-host 0.6 HID + 配对窗口状态机 + `ble.bond` NVS 持久化）+ `wifi` 降级为 cargo feature + `ble` 变体 | 【✅ 2026-08-24 通过】配对/打字/CapsLock LED 回读/bonded-only 重连/非 bonded 拒连/29 轮快连快断零泄漏；trouble 0.4→0.6.0(bt-hci 0.8);WiFi+BLE 内部堆不共存（`BLE assert emi.c 164`)→ ble 变体 WiFi OFF；细节见 `BLE-HID.md` §8 坑录 |
| R7 | hws.rs(GPIO/ADC/I2C)+ 对应 AT/HTTP 端点 | 命令与端点全过【✅ 2026-08-22 通过;I2C_SCAN 改 bit-bang;ADC ch7/8 移除(与 I2C 共脚)】 |
| R8 | 收尾：README、REQUIREMENTS.md 状态、AGENTS.md、内存水位记录、已知问题清单 | 文档齐，clippy 零警告 |
| R9 | rathole.rs(协议 v1 单隧道,plain TCP)+ AT/HTTP/MQTT 隧道面 + `tunnel.1.*` 注册表键 | 【✅ 2026-08-23 通过】本地 rathole server 端到端:echo 双访客、HTTP 64KB 逐字节一致;`tunnel.1.local` 限 LAN 主机(smoltcp 无环回);路由合并修复执行器栈溢出;MQTT 共存实测 |
| R5 | kbd_usb.rs(usb-device + usbd-hid boot 键盘)+ kb.rs 路由/时序层 + AT/HTTP/MQTT 键盘面;编译期功能矩阵(6 features)+ build.sh 变体(full/remoter/base/rathole) | 【✅ 2026-08-24 通过】原生 USB 枚举 303A:8201;AT+TAP/KEY_STR/KEY_SEQ/HTTP/MQTT 注入 evdev 实测;4 变体 clippy 零警告 |

依赖关系：R0→R1→R2 顺序；R3/R5/R7 互不依赖可并行；R4 依赖 R1;
R6 依赖 R5（共享路由层）。

## 8. 风险登记

| 风险 | 影响 | 对策 |
|---|---|---|
| trouble 的 HID over GATT 在 S3 上不成熟 | R6 延期 | 先跑 trouble 官方 HID 样例验证；不行退到 esp-radio 裸 GATT 手写（参考 Zephyr 版 GATT 表） |
| ~~esp-radio WiFi+BLE 共存内存压力~~ | ~~R6 不稳定~~ | 【已解 2026-08-24】堆 77.7K→129.7K + `ble_max_act≥2`(max_connections=3）即共存；初判"内存不足"实为 ble_max_act=1 引发的 HCI 0x07，详见 BLE-HID.md §8 |
| embedded-tls 与自签 CA 的 IP SAN 校验 | R3 阻塞 | 提前用 R0 骨架做 TLS 握手探针实验（复用 gen_certs.sh 证书） |
| picoserve 路由/中间件能力与 Zephyr httpd 不等价 | R4 返工 | 先实现 3 个代表端点（status/at/config）验证可行性再铺全量 |
| USB OTG + WiFi 同开的电源/中断问题 | R5 不稳定 | 参考 Zephyr 版：枚举正常、打字未测；出问题先查供电与 PHY 冲突（H1/H2) |

## 9. 完成定义（整个迁移）

- §7 全部阶段验收通过；AT 命令集与 Arduino 对齐表逐行核对无遗漏；
- 前端 SPA 未做任何修改且全部页面可用；
- README（构建/烧录/AT 集/配置键/内存水位/已知问题）+ REQUIREMENTS.md
  与 AGENTS.md 状态更新；
- clippy 零警告；无 unwrap-in-library；无未记录的非目标外扩。
