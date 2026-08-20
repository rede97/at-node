---
name: esp32-linux
description: ESP32 (C3/S3) firmware development on Linux. Covers nanoESP32-S3 dual-USB architecture, ESPLink serial discipline, esptool/west/espflash flashing, openocd JTAG debugging over native USB, Rust (esp-rs) toolchain, MQTT/TLS test rig, and the Zephyr-era pit list. Use when building/flashing/debugging at-node ESP32 firmware on Linux.
---

# SKILL-esp32-linux — ESP32 Linux 开发环境操作手册

> 版本：v0.1 · 2026-08-20
> 适用：在原生 Linux 上进行 at-node ESP32 系列（C3 Arduino / S3 Rust)开发
> 姊妹篇：`.pi/skills/esp32-windows/`(Windows+C3 坑）、`.pi/skills/esp32-hardware/`（硬件坑）
> 读者：人类开发者 + AI agent

---

## 1. 环境组件总览

| 组件 | 路径 | 状态 |
|---|---|---|
| Zephyr 工作区（**已归档，仅供考古**) | `~/zephyrproject`(main 2026-08)+ `~/.local/zephyr-sdk-1.0.0` | Zephyr 路线已弃（见 §7)，归档 commit `e759a2a` |
| openocd-esp32(JTAG) | `~/tools/openocd-esp32`(v0.12.0-esp32-20260703) | ✅ S3 验证，需 sudo |
| Rust 工具链 | rustup stable/nightly + **espup 装的 esp channel**(`~/export-esp.sh`) | ✅ 已装 |
| espflash | `~/.cargo/bin/espflash` v4.5.0 | ✅ 已装 |
| espup | `~/.cargo/bin/espup` | Xtensa toolchain 管理 |
| uv venv | 仓库根 `uv run python ...`(pyserial/amqtt) | 同 CH582 |

Rust 环境激活（每个新 shell 都要）:

```bash
. ~/export-esp.sh        # 注入 esp toolchain 的 PATH/LIBCLANG_PATH
```

## 2. 板子与 USB 身份（nanoESP32-S3)

**两个 USB-C 口，两个世界**，别插错：

| 口 | 芯片侧 | 枚举 | 用途 |
|---|---|---|---|
| 调试口 | 板载 ESPLink(DAPLink 调试器） | `0d28:0204`(NXP ARM mbed)→ ttyACMx | **烧录/串口控制台/AT**，首选 |
| 原生口 | S3 GPIO19/20 USB PHY | ROM: `303a:1001`(USB JTAG/serial)；固件 OTG: 如 `2fe3:0007` | ROM 下载/JTAG；固件启用 USB OTG 后变成 HID 键盘口 |

关键事实：
- S3 **只有一个 USB PHY**,USB-Serial-JTAG 与 USB OTG(DWC2）互斥。固件一旦
  启用 OTG,`303a:1001` 从总线消失——该口不再是 JTAG/下载口。
- ESPLink 串口 by-id（编号漂移免疫）:
  `/dev/serial/by-id/usb-MuseLab_DAPLink_CMSIS-DAP_*-if01`
- "设备管理器里显示 Nordic 键盘"之类的名称是**主机侧缓存/描述符联想**，
  判断身份只认 VID:PID（我们固件是 `2fe3:0007` Zephyr 开发 VID)。

## 3. 串口纪律（血泪）

1. **打开 ESPLink 串口 = 复位板子**(DTR/RTS → EN/IO0 电路）。每次 python
   `serial.Serial()` open 都是一次重启：打断 WiFi/MQTT 会话、丢复位前日志。
2. 调试期用**常驻双向控制台**持有端口（复位只发生一次）:
   `esp32/zephyr/tools/at_console.py`（取回：`git show e759a2a:esp32/zephyr/tools/at_console.py`)
   — stdin 转发 AT,stdout 打日志，dtr/rts 拉低。
3. **烧录前必须停掉控制台进程**，否则 esptool 与它抢端口 →
   "The chip stopped responding"（现象像板子死了，其实是串口竞争）。
4. 需要在固件运行时强制复位：另一个进程短暂 open + 拉 DTR/RTS 再松开
   （常驻控制台不受影响）。
5. ESPLink 115200 突发日志会丢数据（出现 `ESPLink:Overflow`)——
   调试期关 DBG 级日志。

## 4. 烧录

```bash
# Rust(espflash,首选)— 自动进下载模式、烧完可 --monitor 常驻
espflash flash --port /dev/ttyACM0 --monitor target/xtensa-esp32s3-none-elf/release/<app>

# Zephyr(考古)— 需工作区环境
export PATH=$HOME/zephyrproject/.venv/bin:$PATH
export ZEPHYR_SDK_INSTALL_DIR=$HOME/.local/zephyr-sdk-1.0.0
export ZEPHYR_BASE=$HOME/zephyrproject/zephyr
west flash -d <build_dir> --esp-device /dev/ttyACM0
```

## 5. JTAG 调试（免 eFuse，原生 USB 口）

ESPLink 的 CMSIS-DAP JTAG 要烧 eFuse(`STRAP_JTAG_SEL`，不可逆，**别用**)。
原生口 USB-JTAG 直接用：

```bash
# 前提:固件未占用 USB OTG PHY(占用时 303a:1001 消失 → openocd 报
# "could not find or open device";Zephyr 版留了 triage_no_usb.overlay 专门应对)
sudo -n ~/tools/openocd-esp32/bin/openocd \
  -s ~/tools/openocd-esp32/share/openocd/scripts \
  -f interface/esp_usb_jtag.cfg -f target/esp32s3.cfg -c 'adapter speed 10000'

# gdb(用 Zephyr SDK 或 esp 工具链的 xtensa gdb)
xtensa-espressif_esp32s3_zephyr-elf-gdb <firmware.elf> -ex "target remote :3333"
```

Xtensa 崩溃现场读取（Zephyr 版 WiFi 崩溃就是这样破的）:
- **必须用硬件断点**(`hbreak print_fatal_exception`)——软断点会被启动时
  IRAM 段重载刷掉，永不命中。
- `print_fatal_exception(print_stack, …)` 第一个参数指向的内存首字是 BSA
  指针；gdb 里 `set $bsa = *(_xtensa_irq_bsa_t **)print_stack; p *$bsa`
  解出真正的 pc/ps/exccause/a0。
- `pc` 落在 `xtensa_arch_except_epc` = 软件主动 `k_panic`/`k_except`(a2=reason,
  4=KERNEL_PANIC 常是 blob `abort()`);`EXCCAUSE 28` + VADDR = 空指针解引用。
- 窗口化 ABI 返回地址：高 2 位是 callN 窗口标记，真实 PC = `a0 & 0x3FFFFFFF | 0x40000000`。
- "Current thread: (unknown)" 只是说线程没命名，别过度解读；用 nm + 地址区间
  定位静态线程对象，k_malloc 堆里的线程对象是驱动动态创建的。

Rust 版调试会简单得多：`esp-backtrace` panic 直接打 Rust 栈回溯，
probe-rs/defmt 走同一原生口（固件不开 OTG 时）。

## 6. MQTT/TLS 测试台（本地自签）

```bash
# 1. 证书(SAN 必须含 broker 的 LAN IP;mbedTLS 对 IP 主机名走 IP SAN)
git show e759a2a:esp32/zephyr/tools/gen_certs.sh > /tmp/gen_certs.sh  # 或 Rust 版 tools/
./gen_certs.sh 192.168.1.42        # 生成 certs/ + ca_cert.h

# 2. broker(amqtt,0.0.0.0:8883,匿名)
uv run python tools/broker_8883.py  # 源码见 e759a2a:esp32/zephyr/tools/

# 3. 验证客户端(订阅 state/info/resp + 发 cmd)
uv run python tools/mqtt_probe.py atnode/<device-name>/cmd "AT+VER"
```

排障顺序（demo DEBUGGING §7):VM 内自连 → 裸 TCP 探测 → TLS——先证明网络通
再怀疑 TLS，能省几小时。VMware 里 ESP32 要够到 VM 服务：**桥接网卡**是唯一
省心方案（NAT 映射 + portproxy 都被 Windows 防火墙静默丢包）。

## 7. Zephyr 路线（已归档）

Zephyr 变体 2026-08-20 放弃（`REQUIREMENTS.md` §7.1)：二进制 blob 无符号、
特性组合零上游测试、API 主线漂移。**完整实现与全部坑录在 commit `e759a2a`**
（取回：`git show e759a2a:esp32/zephyr/README.md`)。其中可复用的 Linux 操作
知识已并入本文件；S3 后继开发在 `esp32/rust/`(esp-hal + Embassy，准则见
`esp32/rust/MIGRATION.md`)。

## 8. 已知坑清单

| 坑 | 现象 | 对策 |
|----|------|------|
| **ISR/定时器回调里做 SPI/I2C** | Zephyr:~16s 后无辜线程 panic("blocking pend from ISR context")，现场离真凶极远 | 回调只 signal/send，事务一律在线程/任务；Rust/Embassy 里同理（§MIGRATION 5.2) |
| **p256m 与 TLS ECDHE 冲突**(Zephyr) | MQTT TLS 握手 -0x7F80 HW_ACCEL_FAILED | `CONFIG_MBEDTLS_PSA_P256M_DRIVER_ENABLED=n`;Rust 用 embedded-tls 无此坑 |
| **重连泄漏连接/sockets**(Zephyr) | 重试 N 轮后 NET_MAX_CONN 耗尽 | 每次重连前配对 close/drop；写"杀 server 20 轮"泄漏测试 |
| **USB OTG 抢走 PHY** | 原生口 303a:1001 消失，JTAG/下载失效；崩溃后主机报"无法识别的 USB 设备" | 烧录走 ESPLink 口；JTAG 时固件关 OTG |
| **DWC2 experimental DMA**(Zephyr) | `esp_cache_msync: null pointer` 刷日志 | `CONFIG_UDC_DWC2_DMA=n` 回退 slave-FIFO |
| **DRAM 预算**(S3 512KB SRAM) | WiFi+BT blob + TLS + 大堆 → 链接溢出 | 逐项砍堆；Rust 版用 PSRAM 堆（esp-alloc）缓解 |
| **HTTP service 链接缺段**(Zephyr) | `undefined reference to _http_resource_desc_*_list_start` | 应用必须声明 per-service iterable section(sections-rom.ld) |
| **BT 节点未 enable**(Zephyr) | hal bt.c 编译报 Kconfig 符号未定义 | dts `&esp32_bt_hci { status="okay"; }` |
| **GPIO 黑名单**(S3) | 写 flash/PSRAM/strap 脚直接炸 | 禁：0,3,19,20,26-32,33-37,43,44,45,46;I2C 用 8/9,ADC0=GPIO1-10 |
| **VMware USB 仲裁** | 设备被 Windows 抢走从 VM 消失；HID 键盘默认被当输入设备不透传 | VM 设置勾选"自动连接新 USB 设备"+"显示所有 USB 输入设备";usbip 是终极方案 |
| **sudo 免密 openocd** | openocd 找不到设备（权限） | `sudo -n` 已配置可用；或 udev 规则给 303a 设备 plugdev |

## 9. Rust 开发循环（esp32/rust/)

```bash
. ~/export-esp.sh
cd esp32/rust
cargo build --release                              # xtensa-esp32s3-none-elf
espflash flash --port /dev/ttyACM0 --monitor target/xtensa-esp32s3-none-elf/release/<app>
cargo clippy --release -- -D warnings              # 阶段提交门槛(MIGRATION.md §5.7)
```

迁移准则与阶段计划（R0–R8):`esp32/rust/MIGRATION.md`——先读它再动手。
