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

## 2. 阶段二：dongle 硬化（2–3 天）

- **自动重连**：记录绑定键盘身份地址，开机/断链后自动回连（无需再 scan+conn）。
  处理定向广播窗口短（~1.28s）的问题 — 断链后立即进入高占空比扫描。
- **清理 DIAG**：量产版去掉 `+BT_ADV`/`+BT_GATT`/`+BT_DISC`/`+BT_RD`/`+BT_NTF`
  诊断打印（宏 `BLE_DONGLE_DEBUG` 门控保留）。
- **解析固化**：报告特征>1 时按 Report Map 识别键盘输入报告（先 boot，
  无 boot 再 report；report 模式暂按"首个 input report"策略 + len≥8 过滤）。
- **断链清理**：conn handle/disc state/队列全复位（已有，补测边界）。
- **AT+BT_LIST**：列出已绑定设备（SNV 查询）。

## 3. 阶段三：RK 真机验证（1 天）

用阶段一修复后的固件回测 RK-S75RGB（真实世界考题）：

- RPA 地址旋转下的重复配对/重连
- 8 个报告特征的正确订阅（修复后应自动正确）
- MITM/Just Works 两种配对路径
- 长时间连接稳定性（LSI 时钟，评估是否需要 `-DCLK_OSC32K=0` 外晶振）

## 4. 阶段四：角色切换（需求 F1.16–1.21，另排期）

- `BLE_MODE` 三态宏（KBD/DONGLE/DUAL）替代 `BLE_DONGLE` 布尔
- DUAL 构建：`AT+ROLE=KBD|DONGLE` → DataFlash 标志 → 软复位 → 按标志启动
- 合并 dongle-wip → main（含 `USB_ENABLE` + `#error` 配置校验）

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
tools/ci/build_all.sh    # 两个变体：main(kbd.hex) + dongle-wip(dongle.hex)
tools/ci/flash.sh <board> <hex>   # wchisp，按序列号/端口区分 A/B 板
tools/ci/loop_test.sh    # flash 两板 → test_dongle_loop.py → 输出报告
```

目标：`./tools/ci/loop_test.sh` 一条命令完成"改代码→编译→烧双板→闭环测试"。

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
| M2 | dongle 硬化 | 自动重连 + DIAG 门控 + RK 回测通过（§2/§3） |
| M3 | Linux CI 闭环 | `loop_test.sh` 一键全绿（§5） |
| M4 | 角色切换 + 合并 main | F1.16–1.21 全量实现，dongle-wip 合并（§4） |
