# at-node 代码审计报告

- **审计日期**: 2026-07-26
- **审计范围**: CH582F 固件（`ch582/APP/`、`ch582/APP/HWS/`、`ch582/APP/BLE/`）+ ESP32-C3 变体（`esp32/esp32_at_node/`）
- **审计方式**: 静态代码审查，关键发现已对照原始 WCH EVT SDK 验证
- **处理方式**: 简单且无副作用的问题已直接修复（见 §A）；涉及硬件验证、架构权衡或安全设计的问题留待人工评估（见 §B）

## 总览

| # | 严重度 | 问题 | 文件 | 状态 |
|---|--------|------|------|------|
| 1 | 严重 | 休眠路径重复调用 `SYS_DisableAllIrq`，中断状态被覆盖 | [hws_sleep.c](file://e:/Projects/at_node/ch582/APP/HWS/hws_sleep.c) | ✅ 已修复 |
| 2 | 严重 | `Idle_Value[2]` 数组越界（HID 接口号为 2） | [usb_dev.c](file://e:/Projects/at_node/ch582/APP/usb_dev.c) | ✅ 已修复 |
| 3 | 中等 | 电池电压换算 `uint16_t` 减法下溢 + 与 config.h 文档的 ×4 因子不一致 | [hws_batt.c](file://e:/Projects/at_node/ch582/APP/HWS/hws_batt.c) | ✅ 已验证关闭（用户实测无问题） |
| 4 | 中等 | ADC 换算 pga=0/1 分支同样的减法下溢 | [hws_adc.c](file://e:/Projects/at_node/ch582/APP/HWS/hws_adc.c) | ✅ 已验证关闭（用户实测无问题） |
| 5 | 中等 | ESP32 HTTP 控制端点无任何认证 | [esp32_at_node.ino](file://e:/Projects/at_node/esp32/esp32_at_node/esp32_at_node.ino) | ✅ 已解决（本地可信 NAT + 现有 `AT+HTTP` 开关） |
| 6 | 低 | `keystr_buf` 不完整类型前置声明 + 重复定义 | [at_cmds.c](file://e:/Projects/at_node/ch582/APP/at_cmds.c) | ✅ 已修复 |
| 7 | 低 | `AT+ADC` 处理器中 `pga > 3` 重复检查（死代码） | [at_cmds.c](file://e:/Projects/at_node/ch582/APP/at_cmds.c) | ✅ 已修复 |
| 8 | 低 | `kb_flush` 预算式忙等待，最长阻塞调度器约 240ms | [at_cmds.c](file://e:/Projects/at_node/ch582/APP/at_cmds.c) | ⏳ 待评估（架构权衡） |
| 9 | 低 | 扫描响应连接间隔注释（"100ms"/"1s"）与实际值不符 | [hidkbd_ble.c](file://e:/Projects/at_node/ch582/APP/hidkbd_ble.c) | ✅ 已修复 |
| 10 | 低 | `at_write_uart` 逐字节忙等待 TX FIFO | [at_parser.c](file://e:/Projects/at_node/ch582/APP/at_parser.c) | ⏳ 待评估（性能） |

**总体评价**: 代码质量相当高——有完善的编译期配置校验（`#error`）、现场问题记录（FIELD-NOTES）、边界检查和防御性编程。#1 和 #2 是真实的内存/中断安全 bug，已修复并通过编译；#3/#4 经用户实测验证读数正确，已关闭；#5 经评估为本地可信 NAT 部署，复用现有 `AT+HTTP` 开关（可关 HTTP 仅留 MQTT）即满足安全需求；#8/#10 为性能权衡，留待观察。

---

## §A 已修复的问题

### #1 休眠路径重复禁用全局中断（严重）

**文件**: [hws_sleep.c](file://e:/Projects/at_node/ch582/APP/HWS/hws_sleep.c) `hws_sleep_enter()`

**问题**: `SYS_DisableAllIrq(&irq_status)` 被连续调用两次。第二次调用把"已全关"的状态再次写入 `irq_status`，覆盖了第一次保存的真实中断现场。随后 `SYS_RecoverIrq(irq_status)` 恢复的是"全关闭"状态，导致函数返回后全局中断保持关闭，系统挂死。

**验证**: 对照原始 EVT 代码 [SLEEP.c](file://e:/Projects/at_node/EVT/EXAM/BLE/HAL/SLEEP.c) 的 `CH58X_LowPower`，原始实现只调用一次 `SYS_DisableAllIrq`，确认双调用是移植/修改时引入的回归。

**修复**: 删除重复的第二行调用，恢复为单次调用。

```c
// 修复前
SYS_DisableAllIrq(&irq_status);

SYS_DisableAllIrq(&irq_status);   // ← 重复，覆盖 irq_status
time_curr = RTC_GetCycle32k();

// 修复后
SYS_DisableAllIrq(&irq_status);
time_curr = RTC_GetCycle32k();
```

**备注**: 修复本身是无歧义的单行删除，但位于休眠/中断路径，条件允许时建议在硬件上做一次休眠-唤醒回归测试。

### #2 USB HID `Idle_Value` 数组越界（严重）

**文件**: [usb_dev.c](file://e:/Projects/at_node/ch582/APP/usb_dev.c)

**问题**: 本设备 HID 键盘接口号为 **2**（CDC 占用接口 0/1）。SET_IDLE/GET_IDLE 处理用 `wIndexLo == 2` 作为下标访问 `Idle_Value`，但数组只声明了 2 个元素（下标 0/1），`Idle_Value[2]` 越界读写，破坏相邻静态变量。

**修复**: 数组扩容为 3 个元素并加注释说明下标语义。

```c
// 修复前
static uint8_t  Idle_Value[2] = {0,0};

// 修复后
static uint8_t  Idle_Value[3] = {0,0,0};  /* indexed by HID interface number (2); [0]/[1] unused */
```

### #6 `keystr_buf` 不完整类型前置声明 + 重复定义（低）

**文件**: [at_cmds.c](file://e:/Projects/at_node/ch582/APP/at_cmds.c)

**问题**: 文件顶部用 `static char keystr_buf[];`（不完整数组类型）做前置声明，完整定义 `static char keystr_buf[KEYSTR_MAX + 1];` 在数百行之后。虽然 C 标准允许"不完整 tentative 定义 + 后续补全"，但这种写法脆弱且易误导；`keystr_len/idx/active` 也存在两处重复定义。

**修复**: 将 `#define KEYSTR_MAX` 和完整定义整体上移到首次使用处之前，删除下方的重复定义。现在符号只定义一次。

### #7 `AT+ADC` 死代码重复检查（低）

**文件**: [at_cmds.c](file://e:/Projects/at_node/ch582/APP/at_cmds.c) `at_cmd_ADC()`

**问题**: 第一个 `if` 已检查 `pga > 3` 并返回，第二个 `if (pga > 3)` 永远不可达（死代码）。

**修复**: 合并为单个检查，保留信息更完整的错误提示。

```c
// 修复后
if (ch > 13 || pga > 3) { AT_Response("ERROR: ch 0-13, pga 0-3 (0=-12dB,1=-6dB,2=0dB,3=6dB)"); return -1; }
```

### #9 BLE 扫描响应连接间隔注释错误（低）

**文件**: [hidkbd_ble.c](file://e:/Projects/at_node/ch582/APP/hidkbd_ble.c) `scanRspData[]`

**问题**: 注释标注最小/最大连接间隔为 "100ms"/"1s"，但连接间隔单位是 1.25ms，实际宏值为 KBD_MULTI 下 12/24（= 15ms/30ms），其他模式 8（= 10ms）。注释与实际相差一个数量级。

**修复**: 更正注释，标明单位与按模式的实际取值。

---

## §B 待人工评估的问题

> #3/#4/#5 已完成评估并关闭（见各条「处理结论」）；#8/#10 为性能/架构权衡，暂不修改，留待观察。

### #3 电池电压换算下溢 + ×4 因子不一致（中等）

**文件**: [hws_batt.c](file://e:/Projects/at_node/ch582/APP/HWS/hws_batt.c) 第 28 行；[config.h](file://e:/Projects/at_node/ch582/APP/include/config.h) 第 243 行附近

**问题 A（下溢）**:
```c
return (uint16_t)((raw * vref / 512) - 3 * vref);
```
整个表达式以 `uint32_t` 计算。当 `raw < 1536`（设 vref=1050）时减法结果为负，回绕成巨大无符号值，再截断为 `uint16_t`。下游 `hws_batt_read_percent()` 见 `mv >= HWS_BATT_MAX_MV` 直接返回 **100%**——即**低电量被误报为满电**。

**问题 B（文档不一致）**: `config.h` 中的文档公式写的是 `VDD = ((raw × Vref / 512) − 3 × Vref) × 4`（带 ×4，对应 4:1 电阻衰减器），而 `hws_batt.c` 的实现与注释声称"-12 dB PGA 本身已提供 1/4 衰减，无需额外乘数"（无 ×4）。两处对同一物理量的换算不一致，必有一处错误。

**建议**:
1. 对照 CH58x 数据手册 Table 15-2（pga=0）确认 -12 dB 通道的真实传递函数，判定 ×4 归属（模拟域衰减 vs 数字域乘数，二者不可叠加）。
2. 修正公式后，对减法结果做下溢钳位（如 `raw < 1536` 时返回 0 或 `HWS_BATT_MIN_MV`）。
3. 实测已知电压（如 3.0V/3.7V/4.2V）校准 `HWS_ADC_VREF_MV`。

**处理结论（2026-07-26）**: ✅ 已关闭。用户已实测验证电池电压读数正确，实际工作区间不会触发下溢。按「代码实现优先」原则不修改。

### #4 ADC 外部通道换算下溢（中等）

**文件**: [hws_adc.c](file://e:/Projects/at_node/ch582/APP/HWS/hws_adc.c) 第 36-39 行 `pga_raw_to_mv()`

**问题**: 与 #3 同源的减法下溢：
- pga=0（-12dB）: `(raw×vref/512) − 3×vref`，`raw < 1536` 时下溢
- pga=1（-6dB）: `(raw×vref/1024) − 1×vref`，`raw < 1024` 时下溢

下溢后返回的巨大值**不会**等于 `0xFFFF`，因此调用方的 `mv == 0xFFFF`（坏通道）检查无法拦截，会把错误读数当有效值上报。

**建议**: 与 #3 一并核对数据手册公式后，为两个分支加下溢钳位（结果为负时返回 0）。pga=2/3 分支为纯乘除，无此问题。

**处理结论（2026-07-26）**: ✅ 已关闭。与 #3 同源，用户实测验证 ADC 读数正确。按「代码实现优先」原则不修改。

### #5 ESP32 HTTP 控制面无认证（中等 · 安全）

**文件**: [esp32_at_node.ino](file://e:/Projects/at_node/esp32/esp32_at_node/esp32_at_node.ino) 第 2518-2545 行

**问题**: 以下 HTTP 端点无任何认证/鉴权，局域网内任意设备均可调用：
- `/at-node/at`（POST，任意 AT 命令透传）
- `/at-node/cmd/keyboard/tap|text|key`（注入按键）
- `/at-node/cmd/gpio/write`、`/at-node/cmd/i2c/write`（物理 I/O 写入）
- `/at-node/cmd/ble/bonds/clear`（清除绑定）
- `/at-node/cmd/wifi/config`、`/at-node/cmd/mqtt/config`（改写凭据）

设备作为"AI Agent 的物理执行器"，可远程注入键盘输入、驱动 GPIO/I2C，未认证的写端点意味着同网段攻击者可物理操控目标主机。

**建议**（按成本递增）:
1. 最小方案：为所有 POST 端点加共享 token/密码头校验（`Authorization` 或自定义头），token 存于 `wifi_config.h`。
2. 绑定 mDNS + 仅响应 `.local` 来源，或限制只监听特定接口。
3. 长期：HTTPS + 双向证书（`certs/` 目录已有基础设施），或迁移到已带 TLS 的 MQTT 控制面。

**处理结论（2026-07-26）**: ✅ 已解决。经核查代码已内置完整 HTTP 开关，无需新增：
- `AT+HTTP=0` / `AT+HTTP=1`（或 `AT+HTTP=enable,<0|1>`）可关闭/开启 HTTP，状态持久化到 NVS（`http_enable`），重启后保持；关闭后监听套接字立即关闭（`set_http_enabled()`）。
- MQTT 控制面独立（独立任务与客户端），关闭 HTTP 后不受影响，即“关 HTTP 仅留 MQTT”。
- 启动时按 NVS 配置决定是否 `g_http.begin()`，`loop()` 仅在启用时 `handleClient()`。
- 用法已文档化于 `esp32/API.md`（`AT+HTTP`，含“关闭后需经串口/AP portal/MQTT 重新开启”的提示）与 `esp32/README.md`。

用户评估：本项目部署于本地可信 NAT 环境，该开关已满足安全需求。上述「建议」保留为未来部署到不可信网络时的备选加固方案。

**策略落档（2026-07-26）**: 「HTTP 仅应在可信本地 NAT 开启」已写入 `esp32/README.md`（安全策略章节）、`esp32/API.md`（§7 安全提示）、设备帮助页 `/at-node/help`（安全公告）及 HTTP 启动/开关串口日志。

### #8 `kb_flush` 预算式忙等待阻塞调度器（低 · 性能/架构）

**文件**: [at_cmds.c](file://e:/Projects/at_node/ch582/APP/at_cmds.c) 第 55-60 行

**问题**: BLE 多连接发送报告时，若 TX 队列满（`blePending`/`bleMemAllocError`），代码以最多 60 次 × 80000 次 `__nop()` 的预算自旋重试，最长阻塞 TMOS 调度器约 240ms。期间其他 TMOS 任务（按键扫描、AT 轮询等）无法运行。

**说明**: 代码注释记录了这是有意的现场权衡（2026-07-24：20ms 预算不足以排空一个连接事件，导致 "helloworld" 打成 "hewo"）。LL 运行在独立 IRQ，自旋期间队列仍会排空。

**建议**: 若后续出现按键扫描漏键/AT 响应延迟，可考虑改为 TMOS 事件驱动的非阻塞重发（把待发报告入队，由 TMOS 定时任务重试）。当前若功能正常可暂不改动，但应知晓此阻塞窗口的存在。

### #10 `at_write_uart` 逐字节忙等待（低 · 性能）

**文件**: [at_parser.c](file://e:/Projects/at_node/ch582/APP/at_parser.c) 第 91-95 行

**问题**:
```c
static void at_write_uart(uint8_t ch)
{
    while (!(R8_UART1_LSR & RB_LSR_TX_FIFO_EMP));
    R8_UART1_THR = ch;
}
```
每个字节都自旋等待 TX FIFO 空。一条 256 字节的 `AT_Response` 会逐字节阻塞。UART 波特率有限时，长响应的 CPU 占用较高。

**建议**: 功能正确，仅为效率问题。若 AT 响应吞吐成为瓶颈，可改用 FIFO 批量写入（一次检查 FIFO 空位后连写多字节）或 DMA。当前优先级低。

---

## 修复清单（本次已应用）

| 文件 | 改动 |
|------|------|
| `ch582/APP/HWS/hws_sleep.c` | 删除重复的 `SYS_DisableAllIrq(&irq_status);` |
| `ch582/APP/usb_dev.c` | `Idle_Value[2]` → `Idle_Value[3]` |
| `ch582/APP/at_cmds.c` | `keystr_buf` 定义上移合并；删除 `AT+ADC` 死代码检查 |
| `ch582/APP/hidkbd_ble.c` | 更正扫描响应连接间隔注释 |

> 注：以上修复均为静态审查结论，`hws_sleep.c` 涉及休眠路径，建议条件允许时做一次硬件回归（休眠-唤醒、USB 枚举、SET_IDLE/GET_IDLE）。
