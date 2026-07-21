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
  RPA 键盘（RK）可能需重回 WL/RL 路径 → 阶段三验证。
  `AT+BT_AUTO[=0|1]` 开关（默认 1),`AT+BT_DISC` 单次抑制（hold)✅ 实测。
- **清理 DIAG** ✅（门控）:`+BT_ADV`/`+BT_GATT`/`+BT_DISC` raw/`+BT_RD`/`+BT_NTF`
  全部由 `BLE_DONGLE_DEBUG` 宏门控（默认 TRUE，量产翻 FALSE)。
- **解析固化**：boot 或任一 report 句柄 + len≥8 过滤已在代码；按 Report Map
  识别键盘输入报告 → 移至阶段三（需 RK 真机数据）。
- **断链清理** ✅:`dgl_reset_link_state()` 在 LINK_ESTABLISHED(成功/失败)/
  LINK_TERMINATED 三处调用，句柄表/CCCD 队列/passkey 挂起全复位。
- **AT+BT_LIST** ✅ 双板实测：SNV 查询正确（地址 = 扫描到的 kbd 板地址）。
- **已验证（2026-07-21 双板）**：自动回连连环（断→回连→转发）✅、BT_LIST 地址 ✅、
  hold/恢复 ✅、开机自动回连 ✅、**绑定失配退避保护**（连断 5 次自动 hold,
  AT 不再被淹没）✅、**量产模式 `BLE_DONGLE_DEBUG=FALSE`**（信道干净无诊断行,
  扫描/连接/armed 全通）✅。测试固化：`tools/test_dongle_hardening.py`。

## 3. 阶段三：RK 真机验证（1 天）

用阶段一修复后的固件回测 RK-S75RGB（真实世界考题）：

- RPA 地址旋转下的重复配对/重连
- 8 个报告特征的正确订阅（修复后应自动正确）
- MITM/Just Works 两种配对路径
- 长时间连接稳定性（LSI 时钟，评估是否需要 `-DCLK_OSC32K=0` 外晶振）
- **监督超时失效排查（2026-07-21 双板发现）**：对端硬消失（断电/重刷，无 LL
  terminate）后本端链路悬挂 8 分钟+，5s supervision timeout 似未触发，
  双向均复现 —— 与 LSI 时钟评估合并排查，真机场景=主机睡眠/关机后键盘不重新广播

### 3.0 RK-S75RGB 问题复盘与收获（2026-07-22，ESP32-C3 对照实验）

**现象链**：配对绑定全过 → 订阅"成功" → 键盘零通知 → 早期版本数秒后被
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

**收获（沉淀为规则）**：

- Windows 能用的原因 = 全量 HOGP：读 Report Map → 按图订阅 → 按 ID 分派，
  NKRO 位图由 HID 类驱动原生解析。**地图驱动，绝不猜布局。**
- 简单键盘（我们的 AT-Node 键盘板）Just Works + boot 特征 + 单报告无 ID，
  所以"蒙"也能通；复杂键盘（RK 类多功能）必须走规范路径。
- Bluedroid Arduino 封装（`getCharacteristics` 按 UUID 去重）无法枚举
  同 UUID 多实例 — C3 probe 定位为侦察工具（GATT dump / Report Map），
  不做按键验证主机。
- RK 绑定后广播变脸（名字/HID UUID 消失，甚至定向）— 测试循环必须先
  清绑定再进配对模式；C3 端"永久扫描"消除 5 秒窗口竞态。

**规范化实施方案（新增阶段三.5，取代"盲转"；⏸ 暂缓实施，记录备查）**：

```
发现：读 Report Map 0x2A4B → 迷你解析器 → 报告表
       {report_id, 布局=boot8/NKRO/16bit/其他, 句柄↔ID 经 0x2908 绑定}
订阅：句柄级全 input report CCCD（已有）
分发：[ID2] 直接转 USB · [ID1] NKRO 位图→6 键报告 · [ID4/5] 暂丢弃
```

新文件 `APP/BLE/ble_hid_map.c/h`（迷你 HID 描述符解析器，~150 行，
与 ble_dongle.c 解耦）。验证：双板回归（无 ID 前缀兼容）+ RK 实机。

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
| M2 | dongle 硬化 | 自动重连 + DIAG 门控 + RK 回测通过（§2/§3） |
| M3 | Linux CI 闭环 | ✅ 2026-07-21 `loop_test.sh` 一键全绿（混合模式：kbd 走 ISP,dongle 走 wlink） |
| M4 | 角色切换 + 合并 main | ✅ 2026-07-21 双板实测通过（§4),dongle-wip 早已合并 |
