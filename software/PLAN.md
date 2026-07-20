# at-node 下一步调试与开发计划

> 版本：v0.1 · 2026-07-21
> 范围：BLE 接收器（dongle）调试收尾 → 硬化 → 真机验证 → 角色切换
> 目标环境：Linux 自动化构建/烧录/测试闭环

---

## 0. 现状快照

| 项 | 状态 |
|----|------|
| main 分支 | 键盘固件生产态：AT 命令全通、BT_DISC/BT_PAIR、VER 角色标签、RAM 58% |
| dongle-wip 分支 | 接收器 v1：扫描/连接/配对(Just Works+MITM)/绑定/GATT 发现/报告 fallback 全通 |
| **卡点** | **报告句柄解析产生垃圾值**（0x23A8 等，真实范围 0x0008–0x0034）→ CCCD 写错句柄 → 键盘零通知。raw dump 打印已编码（`+BT_DISC: plen= np= raw ...`），未捕获 |
| 参考对照 | ESP32-C3 probe 已验证 AT-Node 键盘端全流程（连接→订阅→8 字节报告流） |

---

## 1. 阶段一：句柄解析修复（预计 1 个工作日）

**假设**：`attReadByTypeRsp_t.len` 不是"每对字节数"或 `pDataList` 布局与
TI 文档不符（WCH 实现差异），导致按 `i*len+3` 取 value handle 错位。

1. 双板就位：B 板烧 main（键盘），A 板烧 dongle-wip。
2. 跑 `tools/test_dongle_loop.py`，捕获 `+BT_DISC: plen=N np=M raw <14B>`。
3. 对照 ATT 规范（char declaration = handle(2)+props(1)+vhandle(2)+uuid(2)=7B/对）
   定位真实 stride/offset，修 `DISC_RPT_CHAR`/`DISC_BOOT_CHAR`/`DISC_PROTO_CHAR`
   三处解析（共用同一 helper 函数 `dgl_parse_char_vhandle()`）。
4. 循环测试通过：A 板收到 `+BT_NTF` 且字节与注入一致；PC 收到 USB 回打按键。

**完成判据**：`test_dongle_loop.py` 全 PASS（连续 3 次）。

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
| M1 | 句柄解析修复 | loop test 3 连过（§1） |
| M2 | dongle 硬化 | 自动重连 + DIAG 门控 + RK 回测通过（§2/§3） |
| M3 | Linux CI 闭环 | `loop_test.sh` 一键全绿（§5） |
| M4 | 角色切换 + 合并 main | F1.16–1.21 全量实现，dongle-wip 合并（§4） |
