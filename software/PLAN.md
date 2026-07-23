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

## 2. 阶段二：dongle 硬化（2–3 天） 🚧 进行中（2026-07-21 起）

- **自动重连** ✅ 双板实测（2026-07-21)：开机/断链后自动回连，按键转发恢复
  （`test_dongle_hardening.py` 全 PASS)。**实现改为直连绑定地址**（从 SNV bond 0
  读 identity 地址 + addrType,`EstablishLink(highDutyCycle=TRUE, whiteList=FALSE)`)——
  白名单路径（`AUTO_SYNC_WL/RL`）实测不可靠：establish 启动但 25s 无匹配。
  ~~RPA 键盘（RK）可能需重回 WL/RL 路径 → 阶段三验证~~。
  `AT+BT_AUTO[=0|1]` 开关（默认 1),`AT+BT_DISC` 单次抑制（hold)✅ 实测。
- **清理 DIAG** ✅（门控）:`+BT_ADV`/`+BT_GATT`/`+BT_DISC` raw/`+BT_RD`/`+BT_NTF`
  全部由 `BLE_DONGLE_DEBUG` 宏门控（默认 TRUE，量产翻 FALSE)。
- **解析固化**：boot 或任一 report 句柄 + len≥8 过滤已在代码。
  ~~按 Report Map 识别键盘输入报告~~ — 不实现（RK/复杂键盘支持已废弃，见 §3）。
- **断链清理** ✅:`dgl_reset_link_state()` 在 LINK_ESTABLISHED(成功/失败)/
  LINK_TERMINATED 三处调用，句柄表/CCCD 队列/passkey 挂起全复位。
- **AT+BT_LIST** ✅ 双板实测：SNV 查询正确（地址 = 扫描到的 kbd 板地址）。
- **已验证（2026-07-21 双板）**：自动回连连环（断→回连→转发）✅、BT_LIST 地址 ✅、
  hold/恢复 ✅、开机自动回连 ✅、**绑定失配退避保护**（连断 5 次自动 hold,
  AT 不再被淹没）✅、**量产模式 `BLE_DONGLE_DEBUG=FALSE`**（信道干净无诊断行,
  扫描/连接/armed 全通）✅。测试固化：`tools/test_dongle_hardening.py`。

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
| 编译 | `riscv-none-embed-gcc`（MounRiver 提供 Linux 版，或 xPack riscv-none-elf-gcc）；现有 Makefile 直接用，`cd software/obj && make main-build` |
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
| AT 回归 | `tools/test_at.py` | 6/6 PASS |
| KEY_SEQ 长命令 | 内嵌 loop 脚本 | 完整回显 + queued |
| 双板闭环 | `tools/test_dongle_loop.py` | 2/2 键转发字节一致 |
| 断开/配对 | AT+BT_DISC / BT_PAIR + C3 probe | 重连/重配对成功 |

---

## 6. 里程碑

| # | 内容 | 判据 |
|---|------|------|
| M1 | 句柄解析修复 | ✅ 2026-07-21 达成：loop test 3 连过（§1） |
| M2 | dongle 硬化 | 自动重连 + DIAG 门控 + Just Works boot 路径验证（§2） |
| M3 | Linux CI 闭环 | ✅ 2026-07-21 `loop_test.sh` 一键全绿（混合模式：kbd 走 ISP,dongle 走 wlink） |
| M4 | 角色切换 + 合并 main | ✅ 2026-07-21 双板实测通过（§4),dongle-wip 早已合并 |
| M5 | 外设驱动落地 | 🚧 GPIO/ADC/I2C/IR 已在 HWS 实现（宏可配），冒烟已验，待完整硬件实测（§7） |

## 7. 阶段五：外设驱动（需求 F6/F7/F8/F11,2026-07-22 启动）

按需求文档"AI Agent 的手和脚"定位，补齐基本外设。架构约定：
驱动一律放 **HWS 层**（`hws_*.c/h`，纯寄存器操作，不含协议栈逻辑）,
每个子系统一个**编译期宏开关**（`HWS_GPIO` / `HWS_ADC` / `HWS_I2C` / `HWS_IR`),
AT 命令处理只做参数解析，调用 `hws_*` API。

| 子系统 | 宏 | AT 命令 | 要点 |
|--------|-----|---------|------|
| GPIO | `HWS_GPIO` | `AT+GPIO_W=<pin>,<level>` / `AT+GPIO_R=<pin>` | 线性引脚号（PA=0–15, PB=16–39)，模式配置（推挽/上拉/浮空） |
| ADC | `HWS_ADC` | `AT+ADC=<ch>` | 外部单通道采样，返回 mV（内部温度/电池电压已有） |
| I²C | `HWS_I2C` | `AT+I2C_SCAN` / `AT+I2C_R` / `AT+I2C_W` | 主机模式 100k/400k，扫 0x00–0x7F |
| IR | `HWS_IR` | `AT+IR=NEC\|SIRC\|RAW,...` | PWM4 38kHz 载波 + TMR1 门控状态机（需求 §3.10)，busy 时阻止 BLE 休眠 |

实现状态（2026-07-22）：四子系统均已落地（`hws_gpio/adc/i2c/ir.c`），三变体构建
通过（dual FLASH 44.68% / RAM 64.59%），冒烟验证：GPIO 写读、ADC 浮空读数、
I²C 扫描不挂死、IR 命令受理；AT 回归 + 双板 loop/hardening 全 PASS。
待完整实测：GPIO 回环、ADC 定压、I²C 挂 EEPROM/传感器、IR 示波器/空调验证。
已知设计点：GPIO 读固定切上拉输入（读输出引脚会读到上拉电平，非回读驱动态）。

## 8. 阶段六：ESP32-C3 模拟键盘测试台（2026-07-22 立项）

**动机**：当前双板台架是 CH582↔CH582 同栈互测，存在盲区（RK 失败已证明
真实世界键盘多样性不可省略）；且 kbd 板的烧录/重连摩擦大。引入
ESP32-C3 作为**可编程模拟键盘**（BLE HID Peripheral），获得：

- **异构第二栈互操作验证**（NimBLE/Bluedroid 开源栈 vs CH582 闭源栈）
- **场景可编程**：RPA 地址旋转 / boot·report 模式变体 /
  "断电式消失"——覆盖实体键盘买不起也买不全的边界场景。
- **烧录稳定**（esptool / arduino-cli），消除 WCH 工具链的 USB 重连摩擦。
- **标准库即可**：使用 `ESP32BLECombo` 标准 Arduino 库（NimBLE 底层），提供 boot
  keyboard input report + Just Works 配对，与 AT-Node 键盘板工作模式一致。

**实施步骤**：

| # | 内容 | 产出 | 解锁的测试 |
|---|------|------|-----------|
| ① | 最简键盘 sketch（**ESP32BLECombo** 库，NimBLE，boot 报告）放 `tools/esp32c3_kbd/` | 替代 kbd 板跑通 loop/hardening | 🚧 代码/上传完成，待 dongle 闭环实测 |
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

**开发参考**：Windows 下 C3 开发踩坑要点见 `software/SKILL-Windows.md`
（arduino-cli、CDCOnBoot、端口占用、 bleak 等）。

**评估结论（可行，推荐为主控制面）**：
- WiFi+BLE 共存为乐鑫官方支持组合（射频时分复用），C3 资源充足；
- 按键延迟抖动 ~10-100ms，**功能/内容验证无影响，延迟指标不测它**;
- 时序敏感动作（断电窗口等）由 C3 固件本地执行，HTTP 只发触发；
- 串口控制面保留作后备/调试；
- 工作量：v1（/status+/tap+/text+mDNS）约 0.5-1 天，场景端点随 ②③ 扩展。
