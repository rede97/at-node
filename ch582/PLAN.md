# at-node 下一步调试与开发计划

> 版本：v0.1 · 2026-07-21
> 范围：BLE 接收器（dongle）调试收尾 → 硬化 → 真机验证 → 角色切换
> 目标环境：Linux 自动化构建/烧录/测试闭环

---

## 0. 现状快照

| 项 | 状态 |
|----|------|
| main 分支 | 键盘固件生产态：AT 命令全通、BT_DISC/BT_PAIR、VER 角色标签、RAM 58%；**dongle-wip 已合并（2026-07-21)**，`BLE_DONGLE` 编译期切换 |
| dongle-wip 分支 | 已合并进 main。接收器 v1：扫描/连接/配对(Just Works+MITM)/绑定/GATT 发现/boot 订阅全通 |
| ~~卡点~~ | **已修复（2026-07-21,Linux 双板调试）**：① Read By Type 按 value UUID 请求时响应对项 = `[值句柄(2)][当前值]`，句柄在偏移 0（原代码按 TI declaration 布局取偏移 3 → 0x23A8 垃圾）;② `dgl_cccd_list[0]` 陈旧值导致重连后 keep-smallest 过滤误判 → 重连必现 no CCCD;③ RPA 设备（midea 刷屏）撑爆 8 槽扫描列表 → 键盘被挤出 |
| 验证 | `test_dongle_loop.py` **连续 3 次全 PASS(M1 达成）** |
| 参考对照 | ESP32-C3 probe 已验证 AT-Node 键盘端全流程（连接→订阅→8 字节报告流） |

---

## 1. 阶段一：句柄解析修复（✅ 已完成 2026-07-21)

**假设**：`attReadByTypeRsp_t.len` 不是"每对字节数"或 `pDataList` 布局与
TI 文档不符（WCH 实现差异），导致按 `i*len+3` 取 value handle 错位。

**结论（实证）**：假设方向正确但更本质——`GATT_ReadUsingCharUUID` 按
**value UUID** 请求时，WCH 响应对项 = `[值句柄(2)][当前值(...)]`，句柄恒在
**偏移 0**;TI 文档的 declaration 布局（偏移 3）只在按 0x2803 请求时成立。
修复：统一 helper `dgl_rbt_vhandle()` 取偏移 0 + svc 范围校验，覆盖
B/P/R/C 四处。另修复两个连带 bug:CCCD 列表陈旧值（重连必现 no CCCD)、
RPA 刷屏挤出扫描列表（16 槽 + RSSI 最弱逐出）。

## 2. 阶段二：dongle 硬化（✅ 已完成 2026-07-25）

**自动重连**（dongle 侧发起）：

- 触发条件：kbd 复位（`AT+RST`/断电）或 DEV 切回 BLE（`AT+DEV=BLE`）后，kbd 恢复广播
- 流程：kbd 恢复广播 → dongle（`AT+BT_AUTO=1`）扫描到 → 加密重连（bonded）→ GATT 发现 → armed → 按键转发恢复
- 实现：dongle 从 SNV bond 0 读 identity 地址 + addrType，
  `EstablishLink(highDutyCycle=TRUE, whiteList=FALSE)`
- 开关：`AT+BT_AUTO[=0|1]`（dongle 侧命令，默认 1）

**`AT+BT_DISC`**（kbd 或 dongle 均可发起）：

- kbd 侧 `AT+BT_DISC`：kbd 主动断开 BLE 连接，**不恢复广播**（不弹回来）
- dongle 侧 `AT+BT_DISC`：dongle 断开连接，**抑制自动重连**（hold）
- 恢复：dongle 侧需 `AT+BT_AUTO=1` 显式重新启用
- 区别于自动重连：DISC 是**用户主动结束**连接，不应自动弹回

**断链清理** ✅：`dgl_reset_link_state()` 在 LINK_ESTABLISHED（成功/失败）/
LINK_TERMINATED 三处调用，句柄表/CCCD 队列/passkey 挂起全复位。

**清理 DIAG** ✅（门控）：`+BT_ADV`/`+BT_GATT`/`+BT_DISC` raw/`+BT_RD`/`+BT_NTF`
全部由 `BLE_DONGLE_DEBUG` 宏门控（默认 TRUE，量产翻 FALSE)。

**AT+BT_LIST** ✅ 双板实测：SNV 查询正确。

**已验证（2026-07-25）**：
- 开机自动回连 ✅（dongle `AT+BT_AUTO=1` + kbd 冷启动广播）
- DEV 切换自动重连 ✅（`AT+DEV=USB→BLE`，kbd 恢复广播，dongle 自动回连 armed + 按键）
- 绑定失配退避保护 ✅（连断 5 次自动 hold）
- 量产模式 `BLE_DONGLE_DEBUG=FALSE` ✅
- hold 抑制 ✅（`AT+BT_DISC` 后不自动回连，需 `AT+BT_AUTO=1` 恢复）
- Bonded 重连 GATT 发现 ✅（`PAIRING_STATE_BONDED` 回调重启发现）

## 3. 阶段三：RK 真机验证（❌ 不实现）

> **决策变更（2026-07-22）**：RK-S75RGB 类多功能键盘的完整 HID 主机支持
> **正式从当前项目移除**，不在 dongle 固件中实现，也不在 C3 测试台中验证。
> 理由见 §3.0。保留并继续验证的仅 **Just Works + boot keyboard input report
> （8 字节标准布局）** 路径。

原计划的 RK 回测项全部取消：

- ~~RPA 地址旋转下的重复配对/重连~~
- ~~8 个报告特征的正确订阅~~
- ~~MITM/Just Works 两种配对路径~~
- ~~长时间连接稳定性（LSI 时钟，评估是否需要 `-DCLK_OSC32K=0` 外晶振）~~
- ~~**监督超时失效排查（2026-07-21 双板发现）**：对端硬消失（断电/重刷，无 LL
  terminate）后本端链路悬挂 8 分钟+，5s supervision timeout 似未触发，
  双向均复现 —— 与 LSI 时钟评估合并排查，真机场景=主机睡眠/关机后键盘不重新广播~~

**监督超时悬挂问题**单独作为通用连接可靠性项保留（§2 断链清理已覆盖部分，
不再依赖 RK 真机复现）；LSI 时钟评估仅作为可选优化记录，不再阻塞主线。

### 3.0 RK-S75RGB 问题复盘与废弃决策（2026-07-22）

> **结论前置**：RK 类多功能键盘的完整 HID 主机支持 **超出本项目范围，
> 明确废弃**，不在固件、不在测试矩阵、不在 C3 测试台中实现。
> 仅保留 Just Works + boot keyboard input report（8 字节）路径，
> 这也是 AT-Node 键盘板自身的工作模式。

**现象链（历史记录）**：配对绑定全过 → 订阅"成功" → 键盘零通知 → 早期版本数秒后被
0x13 踢下；修复版/C3 则保住连接、键盘屏显已配对、电池通知照推，唯独
无键盘输入报告。

**根因链（三层）**：

1. **解析 bug（已修，M1）**：`readByTypeRsp` 取错偏移 → 垃圾句柄 → CCCD
   写到无效地址 → 键盘判"非正经 HID 主机"踢人。**被踢 vs 保连接的分水岭
   是 CCCD 写对，不是读不读 Report Map。**
2. **订错特征**：RK 的 boot 特征（0x2A22）疑似空壳（订阅无数据）；
   C3/Bluedroid 按 UUID 字符串去重 map，只能订到**任意一个** 0x2A4D 实例
   （未必是键盘输入）→ 双路径全空。AT-Node dongle 按句柄订阅全部 CCCD，
   不受此限。
3. **Report ID 盲区**：RK Report Map（331B，已 dump 解析）显示——
   `ID1`=NKRO 位图键盘（8 修饰位 + 104 键位图，16B）、`ID2`=标准 8 字节
   boot 布局、`ID3`=厂商、`ID4`=消费控制 16bit、`ID5`=系统控制。
   通知格式 `[ID][数据]`，原转发逻辑把 r[0] 当修饰键 → 撞上 ID 字节
   （0x02=左 Shift），键值全错位。

**废弃理由**：

1. **远超项目需求**：AT-Node 的产品定位是"BLE/USB HID 键盘 + 接收器"，
   不是通用 BLE HID 主机。支持第三方复杂键盘（NKRO、Report Map 解析、
   多 Report ID 分派）属于通用操作系统/_HID 类驱动_的工作，不是 60 KB RAM
   的 CH582 该承担的复杂度。
2. **投入产出失衡**：RK 只是冰山一角；真实世界键盘的 Report Map 千差万别，
   要做到"即插即用"需要持续维护 HID 描述符解析器 + 大量真机样本，
   远超当前里程碑预算。
3. **已有路径够用**：AT-Node 键盘板自身就是简单 HID 外设，
   Just Works + boot report 即可满足产品目标；C3 测试台也只需要模拟
   这一简化模型。

**因此**：

- ~~阶段三.5 的 `APP/BLE/ble_hid_map.c/h` 迷你 HID 描述符解析器~~ **不实现**，
  文件不会创建。
- dongle 转发逻辑保持现有"boot 优先、无 boot 则 report fallback（订阅全部
  report CCCD，转发 len>=8 报告，按 8 字节 boot 布局解释）"。
- **C3 测试台只实现 boot 报告模型**，不实现 `nkro_multi` 等 RK 风格 Report Map。
- 相关历史分析保留在此供将来若项目范围扩大时参考。

**保留的简单路径规则**：

- 配对：Just Works（NoInputNoOutput）。
- 特征：boot keyboard input report（UUID 0x2A22），8 字节 `[mods, r0, ..., r5]`，
  无 Report ID 前缀。
- C3 测试台作为该路径的异构验证陪练。

## 4. 阶段四：角色切换（需求 F1.16–1.21) ✅ 双板实测通过（2026-07-21)

- ✅ `BLE_MODE` 三态宏（KBD/DONGLE/DUAL）接入 config.h，派生
  `BLE_HAS_KBD/BLE_HAS_DONGLE` 门控，旧 `BLE_DONGLE` 归一化别名（向后兼容）
- ✅ DUAL 构建（`make MODE=DUAL`):`AT+ROLE=KBD|DONGLE` → DataFlash 标志
  （偏移 0x7C00,magic+role+反码校验，擦除态回退 KBD)→ 软复位 → 按标志启动
- ✅ BT 命令运行期角色分发（`ROLE_GUARD_*`),`AT+VER` 角色标签改为运行期
- ✅ 实测:dual 板 KBD↔DONGLE 双向切换、角色标志掉电/刷机保持（wchisp 只擦
  code flash)、dongle 角色 BT_SCAN/BT_STATE 可用、跨角色命令正确拒绝并提示
- 构建产物：dual FLASH 43.52% / RAM 63.61%（双角色同编，预算内）
- 附产:ISP 升级链路同日打通（`tools/ci/isp_flash.py`,wchisp 0.3.0,
  设备 4348:55e0，高频重试抓 10s 窗口）

---

## 5. Linux 自动化环境

**动机**：构建/烧录/测试全流程脚本化，摆脱手工点按。

### 5.1 工具链

| 组件 | Linux 方案 |
|------|-----------|
| 编译 | `riscv-none-embed-gcc`（MounRiver 提供 Linux 版，或 xPack riscv-none-elf-gcc）；现有 Makefile 直接用，`cd ch582/obj && make main-build` |
| 烧录 | **wchisp**（开源 Rust 工具，`cargo install wchisp`）支持 CH582 USB/串口 ISP；或官方 WCHISPTool 无 Linux 版 |
| 串口测试 | 现有 Python 脚本全部跨平台（pyserial），无需改动 |
| ESP32-C3 | arduino-cli Linux 原生，esp32 core 官方源速度通常正常 |

注意：WSL2 不建议（USB 透传 usbipd 对 CDC/ISP 设备不稳定），
用原生 Linux 机器/虚拟机直通 USB。

### 5.2 自动化脚本（拟新增 `tools/ci/`）

```
tools/ci/build_all.sh    # ✅ 两个变体:make DONGLE=1 参数化 → out/kbd.hex + out/dongle.hex
tools/ci/flash.sh <hex> [role]   # ✅ wlink + 烧后 AT+VER 角色校验(board_roles.py)
tools/ci/loop_test.sh    # ✅ build → 提示挪调试线 → 烧双板 → test_dongle_loop.py
```

(2026-07-21 已实现，待双板一键全绿实测；单 WCH-Link 台架烧双板需挪调试线，脚本有停顿提示，-y 跳过)

目标：`./tools/ci/loop_test.sh` 一条命令完成"改代码→编译→烧双板→闭环测试"。
（2026-07-21 达成。默认混合模式：kbd 走 `isp_flash.py`(AT+ISP 免挪线),
dongle 走 wlink——dongle 板 ISP 握手实测不稳（kbd 板每次都成，dongle 板从未成功,
疑似该板 USB 连接临界）,wlink 是可靠路径;`--isp` 双 ISP / `--wlink` 双 wlink 可选）

### 5.3 验证矩阵（每次 dongle-wip 提交必跑）

| 测试 | 工具 | 通过标准 |
|------|------|---------|
| AT 回归 | `tools/test/test_at.py` | 6/6 PASS |
| KEY_SEQ 长命令 | 内嵌 loop 脚本 | 完整回显 + queued |
| 双板闭环 | `tools/test/test_dongle_loop.py` | 2/2 键转发字节一致 |
| 断开/配对 | AT+BT_DISC / BT_PAIR + C3 probe | 重连/重配对成功 |

---

## 6. 里程碑

| # | 内容 | 判据 |
|---|------|------|
| M1 | 句柄解析修复 | ✅ 2026-07-21 达成：loop test 3 连过（§1） |
| M2 | dongle 硬化 | ✅ 2026-07-25 达成：自动重连+DIAG门控+bonded GATT 发现修复（§2） |
| M3 | Linux CI 闭环 | ✅ 2026-07-21 `loop_test.sh` 一键全绿 |
| M4 | 角色切换 + 合并 main | ✅ 2026-07-21 双板实测通过（§4) |
| M5 | 外设驱动落地 | ✅ GPIO/ADC(PGA+Vref)/I2C/IR/TEMP 已实现，冒烟已验（§7） |

## 7. 阶段五：外设驱动（需求 F6/F7/F8/F11,2026-07-22 启动）

按需求文档"AI Agent 的手和脚"定位，补齐基本外设。架构约定：
驱动一律放 **HWS 层**（`hws_*.c/h`，纯寄存器操作，不含协议栈逻辑）,
每个子系统一个**编译期宏开关**（`HWS_GPIO` / `HWS_ADC` / `HWS_I2C` / `HWS_IR`),
AT 命令处理只做参数解析，调用 `hws_*` API。

| 子系统 | 宏 | AT 命令 | 要点 |
|--------|-----|---------|------|
| GPIO | `HWS_GPIO` | `AT+GPIO_W=<pin>,<level>` / `AT+GPIO_R=<pin>` | 线性引脚号（PA=0–15, PB=16–39)，模式配置（推挽/上拉/浮空） |
| ADC | `HWS_ADC` | `AT+ADC=<ch>[,<pga>]` | 外部单通道采样，可选 PGA（0=-12dB/1=-6dB/2=0dB默认/3=6dB），返回 `+ADC:<raw>,<mV> mV` |
| I²C | `HWS_I2C` | `AT+I2C_SCAN` / `AT+I2C_R` / `AT+I2C_W` | 主机模式 100k/400k，扫 0x00–0x7F |
| IR | `HWS_IR` | `AT+IR=NEC\|SIRC\|RAW,...` | PWM4 38kHz 载波 + TMR1 门控状态机（需求 §3.10)，busy 时阻止 BLE 休眠 |
| TEMP | （无宏，常驻） | `AT+TEMP` | 内部温度传感器（ADC 通道），读出原始 12-bit ADC 值或换算 °C。`hws_get_temp()` 已实现（BLE 校准用），仅需 AT 命令封装 |

实现状态（2026-07-22）：四子系统均已落地（`hws_gpio/adc/i2c/ir.c`），三变体构建
通过（dual FLASH 44.68% / RAM 64.59%），冒烟验证：GPIO 写读、ADC 浮空读数、
I²C 扫描不挂死、IR 命令受理；AT 回归 + 双板 loop/hardening 全 PASS。

**ADC PGA + Vref 校准（✅ 已完成 2026-07-25）**：当前 `AT+ADC` 硬编码 PGA=0dB，换算用 `raw*3300/4095`（未按
datasheet Table 15-2 公式）。方案：① 加可选 `<pga>` 参数，默认 0dB 向后兼容；
② 换算改用 `HWS_ADC_VREF_MV`（VINTA 典型 1050mV，可校准）+ 每档 PGA 独立公式；
③ 响应格式改为 `+ADC:<raw>,<mV> mV` 同时返回原始值。详见 §7.1。

**Vref 校准（✅ 已完成 2026-07-25）**：当前 `HWS_ADC_FULLSCALE_MV=3300` 和
`HWS_BATT_ADC_FULLSCALE_MV=13200` 均为理论值，未基于实际 VINTA 标定。
VINTA 芯片间差异 ±15mV（≈1.5%），加上 PGA 增益误差，电池电压读数可能偏差 3-5%。
解决方案：加 `HWS_ADC_VREF_MV` 宏（默认 1050），ADC 和电池统一用该值按 datasheet
公式换算；用户可实测 VINTA 后覆盖宏值完成单点校准。详见 §7.1。

待完整实测：GPIO 回环、ADC 定压（含 PGA 各档）、I²C 挂 EEPROM/传感器、IR 示波器/空调验证。
已知设计点：GPIO 读固定切上拉输入（读输出引脚会读到上拉电平，非回读驱动态）。

**ADC 通道与引脚对照**（外部通道 0-13）：

| 通道 | 引脚 | 状态 |
|------|------|------|
| AIN0 | PA4 | 空闲 |
| AIN1 | PA5 | 空闲 |
| AIN2 | PA12 | IR PWM4 占用 |
| AIN3 | PA13 | SWCLK（调试） |
| AIN4 | PA14 | SWDIO（调试） |
| AIN5 | PA15 | 空闲 |
| AIN6 | PA3 | 空闲 |
| AIN7 | PA2 | 空闲 |
| AIN8 | PA1 | 空闲 |
| AIN9 | PA0 | 空闲 |
| AIN10 | PA6 | 空闲 |
| AIN11 | PA7 | 空闲 |
| AIN12 | PA8 | LED1 + DEBUG UART RX |
| AIN13 | PA9 | DEBUG UART TX |

> 内部通道：14 = VBAT（VDD 供电电压，1/4 分压），15 = 温度传感器（差分模式）。

### 7.1 ADC PGA + Vref 校准方案（✅ 已完成 2026-07-25）

**动机**：
- `AT+ADC` 硬编码 0dB PGA，无法测量高于 2V 或低于 0.6V 的信号
- 当前换算 `raw * 3300 / 4095` 不符合 datasheet Table 15-2 公式，不同 PGA 档位误差更大
- 电池电压（`HWS_BATT_ADC_FULLSCALE_MV=13200`）同样未校准，依赖 VINTA 典型值
- VINTA 芯片间差异 ±15mV（≈1.5%），叠加 PGA 增益误差，电池读数可能偏差 3-5%

**PGA 参数设计**：

```
AT+ADC=<ch>[,<pga>]
  ch   : 0-13 外部通道
  pga  : 0=-12dB(1/4x), 1=-6dB(1/2x), 2=0dB(1x,默认), 3=6dB(2x)

响应：+ADC:<raw>,<mV> mV
  例：AT+ADC=5    → +ADC:2048,1050 mV   (PGA 默认 0dB)
  例：AT+ADC=5,1  → +ADC:2048,1574 mV   (PGA=-6dB)
  例：AT+ADC=5,3  → +ADC:3500,604 mV    (PGA=6dB, 接近饱和)
```

**换算公式**（datasheet Table 15-2，Vref=VINTA）：

| PGA | 公式 | ADC=4095 满量程 (Vref=1050mV) |
|-----|------|-------------------------------|
| 0 (-12dB) | mV = raw × Vref/512 − 3×Vref | ~5248 mV（可测到 VIO33） |
| 1 (-6dB) | mV = raw × Vref/1024 − 1×Vref | ~3149 mV |
| 2 (0dB) | mV = raw × Vref/2048 | ~2100 mV |
| 3 (6dB) | mV = (raw + 2048) × Vref/4096 | ~1575 mV |

**Vref 校准**：

```c
// config.h — 默认典型值，用户可按实测覆盖
#define HWS_ADC_VREF_MV  1050   // VINTA 典型值，范围 1035-1065mV
```

校准步骤：
1. 选一个已知电压源（如万用表实测的 3.3V 或 1.8V 引脚）
2. 选匹配的 PGA 档（3.3V→0 档, 1.8V→2 档）
3. `AT+ADC=<ch>,<pga>` 读 raw 值
4. 反算 Vref：`Vref = 公式逆解(mV_已知, raw, pga)`
5. 更新 `HWS_ADC_VREF_MV`，所有通道（含电池）统一生效

**改动清单**：

| 文件 | 改动 | 行数 |
|------|------|------|
| `config.h` | 新增 `HWS_ADC_VREF_MV`，替代 `HWS_ADC_FULLSCALE_MV` | +3 |
| `hws_adc.h` | `hws_adc_read_mv(uint8_t ch, uint8_t pga)` 加 pga 参数 | ~2 |
| `hws_adc.c` | PGA 传入 init，按 Table 15-2 公式换算 mV | ~15 |
| `hws_batt.c` | 改用 `HWS_ADC_VREF_MV` + -12dB 公式（替代 `HWS_BATT_ADC_FULLSCALE_MV`） | ~3 |
| `at_cmds.c` | 解析可选 PGA 参数，响应 `+ADC:<raw>,<mV> mV` | ~8 |
| **总计** | | **~30 行** |

**向后兼容**：pga 参数省略时默认 2（0dB），响应格式从 `<mV> mV` 变为 `+ADC:<raw>,<mV> mV`（脚本需更新正则，影响面小）。

## 8. 阶段六

**动机**：当前双板台架是 CH582↔CH582 同栈互测，存在盲区（RK 失败已证明
真实世界键盘多样性不可省略）；且 kbd 板的烧录/重连摩擦大。引入
ESP32-C3 作为**可编程模拟键盘**（BLE HID Peripheral），获得：

- **异构第二栈互操作验证**（NimBLE/Bluedroid 开源栈 vs CH582 闭源栈）
- **场景可编程**：RPA 地址旋转 / boot·report 模式变体 /
  "断电式消失"——覆盖实体键盘买不起也买不全的边界场景。
- **烧录稳定**（esptool / arduino-cli），消除 WCH 工具链的 USB 重连摩擦。
- **直接 NimBLE 实现**：不依赖 `ESP32-BLE-Keyboard`/`ESP32BLECombo` 高层封装，
  精确控制 boot input/output report 与 protocol mode 属性，与 CH582 dongle
  的发现逻辑完全对齐。

**实施步骤**：

| # | 内容 | 产出 | 解锁的测试 |
|---|------|------|-----------|
| ① | 最简键盘 sketch（**NimBLE-Arduino 直接实现 boot keyboard**，8 字节报告）放 `tools/demo/esp32c3_kbd/` | 替代 kbd 板跑通 loop/hardening | ✅ 闭环实测通过（COM3 C3 + COM4 dongle） |
| ② | LE Privacy RPA 周期轮换 | RPA 键盘模拟 | dongle RPA 重连（TEST-TODO C 区） |
| ③ | ~~手工 Report Map 变体~~ | ~~F1.22 黄金测试键盘~~ | **不实现**（RK/复杂键盘支持已废弃，见 §3） |
| ④ | 射频硬关断模拟"断电消失" | 监督超时触发器 | C1/C2 精确复现（通用连接可靠性） |

**与现有台架关系**：脚本沿用双板模式（`test_dongle_loop.py` 的角色识别
扩展出 `c3` 标签）；CH582 kbd 板保留（同栈回归仍有价值）。
优先级：排在 TEST-TODO C 区之前——它是 C 区多项测试的使能器。

### 8.1 控制面：HTTP API（2026-07-22 评估通过）

C3 同时运行 WiFi HTTP 服务，测试脚本/Agent 以 HTTP 请求驱动键盘动作,
**键盘侧彻底去 USB 化**（仅需供电），绕开 VMware USB 仲裁痛点。

```
测试脚本/Agent --HTTP(WiFi)--> ESP32-C3(HTTP 服务 + BLE 键盘外设)
                                      | BLE HID reports
                                      v
                                 CH582 dongle --USB--> 主机
```

**API 设计**（草案）：

| 端点 | 功能 |
|------|------|
| `GET  /status` | 状态：BLE 连接/RPA/当前 Map/IP |
| `POST /tap?ms=&mods=&k=` | 点按（按下+自动释放） |
| `POST /key?mods=&k=` | 手动报告（k=0 为释放） |
| `POST /text?s=` | 字符串打字 |
| `POST /seq?d=&r=` | 按键序列回放 |
| `POST /rpa?enable=&period_s=` | RPA 地址轮换开关/周期 |
| `POST /rf?state=off\|on` | 模拟断电消失/恢复（射频硬关断） |
| `POST /pair` `/disconnect` | 配对/断链控制 |

寻址用 mDNS（`esp32kbd.local`）。

**开发参考**：Windows 下 C3 开发踩坑要点见 `.pi/skills/esp32-windows/SKILL.md`
（arduino-cli、CDCOnBoot、端口占用、 bleak 等）。

**评估结论（可行，推荐为主控制面）**：
- WiFi+BLE 共存为乐鑫官方支持组合（射频时分复用），C3 资源充足；
- 按键延迟抖动 ~10-100ms，**功能/内容验证无影响，延迟指标不测它**;
- 时序敏感动作（断电窗口等）由 C3 固件本地执行，HTTP 只发触发；
- 串口控制面保留作后备/调试；
- 工作量：v1（/status+/tap+/text+mDNS）约 0.5-1 天，场景端点随 ②③ 扩展。

## 9. 开放项（2026-07-25）

> M1–M5 全部完成，KBD_MULTI 已完成。剩余非代码项：

### 9.1 UART 物理通道 + HWS_SLEEP（待硬件）

- 接线：USB-TTL → PA4(TX)/PA5(RX)/GND
- UART 与 CDC 双通道命令/响应一致性
- `HWS_SLEEP=TRUE` 构建下测 `AT+SLEEP=<mode>,<sec>`

## 10. KBD_MULTI 键盘多模（✅ 已完成 2026-07-24）

> 依据 4263525 文档定稿：变体 `MODE=KBD_MULTI`,3 主机绑定，
> `AT+DEV=<target>`(target=USB|BLE1|BLE2|BLE3|ALL）无缝切换，无需复位。
> **列表类输出统一约定：索引在行首**(AT+DEV 查询、BT_SCAN 均如此，机器可解析)。

### 10.1 现状障碍（代码勘察 2026-07-24)

| 点 | 位置 | 单连接假设 |
|---|------|-----------|
| 连接句柄 | `hidkbd_ble.c:152` `ble_hid_emu_conn_handle` 单全局 | 发送/断连全绑一个 |
| 报告发送 | `ble_hid_dev.c` `ble_hid_dev_report()` 用全局 `gapConnHandle` | 无句柄参数 |
| 广播策略 | `ble_hid_dev.c` hidDevGapStateCB:CONNECTED 即停广播 | 多模须"未满 3 连接持续广播" |
| 断开重播 | hidDevDisconnected：仅 bonded 才重播 | 多模按空闲槽位重播 |
| 绑定存储 | config.h `BLE_SNV_NUM=1`,SNV 512B 单块 | 须 3 槽 |
| 堆 | kbd 5KB | 3 连接 ~8KB(F1.15: ~1.5KB/conn) |

### 10.2 实施步骤

**P0 构建骨架**
1. config.h:`BLE_MODE_KBD_MULTI = 3`;KBD_MULTI ⇒
   `PERIPHERAL_MAX_CONNECTION=3` / `BLE_SNV_NUM=3` / `BLE_MEMHEAP_SIZE`→8KB;
   `BLE_HAS_KBD` 纳入 KBD_MULTI;#error 排斥 DONGLE/DUAL 组合
2. makefile:`MODE=KBD_MULTI` → `CFG_DEFS := -DBLE_MODE=3`;
   build_all.sh 增 kbd_multi 变体 → `tools/ci/out/kbd_multi.hex`
3. role.c:KBD_MULTI 初始化路径 = 单模键盘（Peripheral)

**P1 多连接核心（主工作量）**
4. `hidkbd_ble.c`:`kbd_conns[3] {handle, addr, state}` 槽位表；
   GAPROLE_CONNECTED 分配 / 断开释放；单模构建下槽 0 即原全局（`#if` 保持 KBD/DUAL 路径不动）
5. `ble_hid_dev.c`:`gapConnHandle` → 数组；`ble_hid_dev_report()` 加句柄参数；
   CONNECTED 不再无条件停广播——连接数 < 3 继续广播；断开按空闲槽重播
6. 连接参数更新（latency/interval）按句柄逐个下发

**P2 路由 + AT 层**
7. `kb_target` 位掩码：USB|BLE1|BLE2|BLE3;`kb_flush()` 按激活掩码路由；
   单模构建的 KB_USB/KB_BLE/KB_BOTH 映射到掩码，保持兼容
8. `AT+DEV=<target>` 设定 / `AT+DEV` 查询——**索引行首**:
   `1,USB,ready` / `2,BLE1,AA:BB:CC:DD:EE:FF,connected` / `3,BLE2,-,unbound`
9. `AT+KB` 桥接到 DEV 语义（BOTH⇔ALL);AT+STATUS 增加激活目标字段

**P3 绑定 3 槽**
10. `BLE_SNV_NUM=3`：核算 SNV Flash 布局（3 块）；槽位=绑定顺序即 BLE1/2/3
    稳定身份（非连接顺序）；`AT+BT_UNBIND=<slot>` 单槽擦除

**P4 验证（TEST-TODO D5)**
11. 三主机台架：PC(BLE) + 手机 + dongle 板（`AT+BT_CONN=AT-Node`);
    绑 3 → `AT+DEV` 列表核对 → 逐目标 `AT+KEY` → 各主机独立收到；
    `AT+DEV=ALL` 广播验证；固化 `tools/test_multi.py`

### 10.3 风险与对策

- **RAM**:堆 5→8KB 约 +3KB,kbd 68%→~78%，每步构建看 size 表，超 80% 即收
- **WCH GAPRole 多链路广播行为**：半连接状态下的广播间隔/可连性需实测调参
- **SNV 布局变更使旧绑定失效**：可接受，commit 注明；先 P0+P1 用"无绑定直连"跑通再做 P3
- **dongle 板当主机**：其 `AT+BT_CONN` 已支持按名连接，可直接当第 3 主机，无需新硬件

## 11. 交接进展(2026-07-24 深夜暂停点)

### 已完成(全部入库,本地 HEAD=9e2e0e2 + 123ec1b,GitHub 网络不通待推)

**KBD_MULTI 多模键盘全链路**(F1.10–F1.15 收官):
- 单活动链路模型:仅 AT+DEV 目标槽持有连接,其他已绑定主机秒拒(省电/消灭多链路复杂度)
- 槽位持久化 slotmap(DataFlash 0x7B00):预留地址/名字/节奏/MAC/地址类型,跨刷机
- 配对窗口:AT+BT_PAIR[=<BLEn>] 60s,窗口外未知主机拒连(安全,市面键盘范式)
- 每槽独立 MAC:默认芯片 MAC+slot(BLE1 兼容旧绑定),AT+MAC 可配持久化
- AT+NAME 助记 / AT+PACE 每槽节奏(默认30ms)/ AT+FACTORY 出厂复位
- +KEY_DONE 回放完成 URC(agent 同步点);KEY_STR 特殊字符映射表修复(F17)
- 定向广播(ADV_DIRECT)代码就绪(A/B 调试中,见下)
- dongle AT+BT_CONN=mac|name,<目标>[,秒] watch 式连接(超时默认5s)
- FIELD-NOTES.md F1–F17 坑录;tools/bt_host.py / bt_agent.py / test_multi.py 固化

### 当前断点(下次从这里继续)

**[OPEN] 广播复活路径回归**:切 AT+DEV=USB → BLE1 后广播不再发出
(btmon 零包,dongle 无法回连)。冷启动广播正常;怀疑点:
① kbd_adv_update 的 off→EVENT_TYPE→want 序列;
② kb_ble_apply_addr 与 kbd_adv_update 双重开关;
③ DEV 循环后 GAPRole 状态异常。下一步:reset 后先验证 boot 广播,
再单步 DEV 切换定位失效点;必要时恢复 hidDevDisconnected 的简单重开逻辑。

**[OPEN] dongle 板固件待更新**:watch 模式 AT+BT_CONN 在 dongle 固件中,
dongle 板仍跑旧版(调试线目前在 kbd 板,需挪线刷 dongle.hex)。

**[OPEN] dongle auto_hold 闭锁**:5 连败后 AT+BT_AUTO=1 单独解锁有时不生效
(消息提示要 AT+BT_PAIR),查 dgl_auto_hold 清理路径。

### 台架状态

- kbd 板(kbd_multi 固件,调试线接着):BLE1=dongle 预留 / BLE2=空 / BLE3=Windows 预留
- dongle 板:旧固件,auto 重连可用(配对窗开着时手动 AT+BT_CONN 有效)
- 已验证矩阵:BLE1 dongle 按键通路 / BLE2 手机打字 / BLE3 Windows 长文本+回车
- GitHub:网络抖动,本地 2 提交未推(123ec1b 映射修复 + 9e2e0e2 定向广播/watch)
