# SKILL-Linux — Linux 开发环境操作手册

> 版本：v0.1 · 2026-07-21
> 适用：在原生 Linux 上进行 at-node 固件的构建 / 烧录 / 测试(PLAN.md §5 的落地)
> 读者：人类开发者 + AI agent(遵循 DESIGN.md §1.4 AI 可读性原则)

---

## 1. 环境组件总览

### 1.1 WCH 工具链(`~/wch`)

| 路径 | 内容 | 用途 |
|------|------|------|
| `~/wch/Toolchain/RISC-V Embedded GCC/bin` | **GCC 8.2.0,前缀 `riscv-none-embed-*`** | ✅ 本项目构建使用 |
| `~/wch/Toolchain/RISC-V Embedded GCC12/bin` | GCC 12,前缀 `riscv-wch-elf-*` | 不使用(前缀与 makefile 不匹配) |
| `~/wch/Toolchain/RISC-V Embedded GCC15/bin` | GCC 15.2,前缀 `riscv32-wch-elf-*` | 不使用(同上) |
| `~/wch/Toolchain/arm-none-eabi-gcc` | ARM 工具链 | 与本项目无关 |
| `~/wch/OpenOCD/OpenOCD/bin/openocd` | OpenOCD 0.11.0-dev,含 `wch-riscv.cfg` | 备选调试路径(未验证) |
| `~/.local/bin/wlink` | wlink 0.1.2(Rust,WCH-Link 协议) | ✅ **烧录首选,已验证支持 CH582** |
| `~/.local/bin/wchisp` | wchisp(USB ISP 烧录) | 备选(未验证) |

关键点:`software/obj/` 的 makefile 硬编码调用 `riscv-none-embed-gcc`,
只有 GCC 8.2(xPack)那个目录提供此前缀。换工具链必须同步改 makefile。

### 1.2 Python 环境(uv)

仓库根有 `pyproject.toml`(依赖:pyserial),`uv run python tools/xxx.py` 自动建
`.venv` 并装依赖。`uv.lock` 提交进 git 保证可复现;`.venv/` 已 gitignore。

### 1.3 串口工具

- **lser**(cargo 已装):列出有效串口 + VID:PID + 描述,首选。
  脚本场景用 `lser --plain`(CSV)或 `lser --json`(JSON),便于解析。
- pyserial 兜底:`uv run python -m serial.tools.list_ports -v`

## 2. 环境初始化

```bash
source env.sh        # 仓库根,幂等,仅向 PATH 前置两个目录
```

验证输出:`at-node env OK: riscv-none-embed-gcc (xPack ...) 8.2.0`

⚠️ 注意:env.sh 只对当前 shell 生效。AI agent / CI 的每条命令都是新 shell,
**每次构建都要重新 source**(或写入 ~/.bashrc)。

## 3. 构建

```bash
source env.sh
cd software/obj && make --no-print-directory main-build
# dongle 变体(BLE HID 接收器):make clean 后
make --no-print-directory main-build DONGLE=1
# 两个变体一起构建:tools/ci/build_all.sh(产物在 tools/ci/out/)
```

⚠️ 变体切换必须先 `make clean`——make 不追踪编译宏变化，不 clean 会把
上一变体的 .o 链进新固件(build_all.sh 已内置此纪律，且结束后清理 obj 树)。

产物(全部 gitignore):`at-node.elf / .hex / .lst / .map / .siz`

**基线占用(main 分支 @2aa506d,与 Windows 构建一致):**

```
FLASH: 162616 B / 448 KB (35.45%)
RAM:    18996 B / 32  KB (57.97%)
```

### 3.1 Linux 化修复(已完成,勿回退)

MRS(MounRiver Studio)自动生成的 makefile 原本是 Windows 绝对路径,
已批量改为相对路径(make 始终在 `software/obj/` 下执行,`..` = `software/`,
Windows/Linux 通用):

- `obj/makefile`:`-T "../Ld/Link.ld"` + `-L".." -L"../LIB" -L"../StdPeriphDriver"`
- `obj/**/subdir.mk`(7 个):所有 `-I"e:/Projects/..."` → `-I"../..."`

若 Windows 端 MRS 重新生成工程文件覆盖了这些路径,一键修复:

```bash
cd software/obj
sed -i 's|e:/Projects/at_node/software|..|g' */subdir.mk APP/*/subdir.mk makefile
```

### 3.2 大小写敏感

Linux 文件系统区分大小写。`#include "hws.h"` 找不到 `HWS.h`。
已修复:`APP/HWS/HWS.h` → `hws.h`(git mv,符合 HWS 命名约定)。
新增文件时严格遵守 `hws_<subsys>.c/h` 小写命名,避免复发。

## 4. 串口权限(一次性配置,需 sudo)

问题:`/dev/ttyACM0/1`(at-node 板)属 `root:dialout`,当前用户不在 dialout 组,
**Python 工具打不开串口**。WCH-Link(ttyACM2)属 `plugdev`,用户已在组内,无碍。

**推荐方案 — udev 规则**(按 VID:PID 匹配,归属 plugdev,免重新登录):

```bash
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="2107", GROUP="plugdev", MODE="0660"' \
  | sudo tee /etc/udev/rules.d/99-at-node.rules
sudo udevadm control --reload && sudo udevadm trigger
```

替代方案:`sudo usermod -aG dialout $USER`(需注销重登生效)。

## 5. 烧录

### 方案 A:wlink + WCH-Link(✅ 已验证,首选)

wlink 0.1.2 实测支持 CH582(2026-07-21 验证:WCH-LinkE v2.16 → CH582,
ChipID 0x82000000,WCH-V4A 内核):

```bash
wlink status                       # 探测:确认 Attached chip: CH582
wlink flash software/obj/at-node.hex    # 烧录 + 自动复位,162 KB 约 18 秒
wlink flash -e firmware.hex        # 可选:先整片擦除
```

烧的是 WCH-Link **调试线物理连接**的那块板(与 ttyACM 端口号无关),
双板台切换目标板 = 重插调试线。

### 方案 B:wchisp(USB ISP,✅ 已验证 2026-07-21)

**关键事实（实测修正）**:
- CH582 ISP bootloader 枚举为 **`4348:55e0`**(不是 1a86:8010——那是 WCH-Link;
  两者同总线共存互不干扰，wchisp 能正确区分)
- wchisp 0.3.0 实测识别 CH582[0x8216] 并完成 擦除→写入→校验→复位 全流程
- boot ROM ISP 窗口 ~10s，且 **USB 握手极不稳定**——probe 到设备不代表能连上，
  必须**高频重试 `wchisp flash` 抓时机**(实测第 4 次尝试才成功;
  单次 probe→分析→flash 的串行流程必错过窗口)

**标准用法——`tools/ci/isp_flash.py`(双线程：一个死循环锤 flash，一个发 AT+ISP):**

```bash
# 板子跑的是新固件(有 AT+ISP):一条命令完成
uv run python tools/ci/isp_flash.py tools/ci/out/kbd.hex --port /dev/ttyACM0

# 空片/手动 BOOT 键:先启脚本,再插线或按 BOOT 上电
uv run python tools/ci/isp_flash.py tools/ci/out/kbd.hex
```

板子进 ISP 的方式:
1. **`AT+ISP` 命令**(✅ 已实测):擦 page0 + 软复位，免按键;窗口 ~10s
2. **按住 BOOT 键上电**(硬件方式,✅ 已实测);app 完好,窗口过期自动回 app
3. 空片每次复位/上电自动进 ISP

⚠️ AT+ISP 会擦 page0：若 10s 窗口内没刷上，芯片跳进空 app 挂死——
复位/重插再给一次窗口，wlink 调试线永远可救。
⚠️ wchisp flash 只擦 code flash,DataFlash(SNV 绑定、角色标志)不动——
刷机后绑定和 AT+ROLE 角色保持。

### ⚠️ 烧录纪律

当前两块板是 PLAN.md 阶段一的双板测试台(kbd + dongle 固件),
**任何烧录前必须确认目标板身份**(`AT+VER` 角色标签),避免误刷测试环境。

## 6. 测试

### 6.1 串口操作标准流程:先定位,后指定

**原则:先用 lser 确定端口,再用 python 直接操作指定端口——不要逐个端口试探。**

```bash
# 1. 列出端口(--plain 输出 CSV:name,vendor,product,usb)
lser --plain

# 2. 按 VID:PID 过滤取端口名(1a86:2107 = at-node 板;1a86:8010 = WCH-Link/ISP,排除)
PORT=$(lser --plain | awk -F, '$4=="1a86:2107"{print $1; exit}')

# 3. 对指定端口直接操作
uv run python -c "
import serial, time
s = serial.Serial('$PORT', 115200, timeout=1)
s.write(b'AT+VER\n'); time.sleep(0.4)
print(s.read(s.in_waiting or 1).decode())
"
```

双板场景:取出全部 `2107` 端口后,逐板发 `AT+VER` 读角色标签(`[kbd]`/`[dongle]`)
确定身份,之后固定操作对应端口。AI agent 操作串口时同样遵守此流程。

### 6.2 工具脚本一览

**测试:**

```bash
uv run python tools/test_at.py [port]     # AT 回归 6 项(自动找第一块板,可指定端口)
uv run python tools/test_dongle_loop.py   # 双板闭环:扫描→配对→订阅→按键转发
uv run python tools/test_dongle_hardening.py  # 阶段二:回连/BT_LIST/hold(需已绑定)
uv run python tools/send_key.py 0x04 --mode BLE
```

**日常操作:**

```bash
# at_cli.py — 按角色/端口发 AT 命令(最常用)
uv run python tools/at_cli.py --role kbd "AT+VER"
uv run python tools/at_cli.py --role dongle "AT+BT_AUTO=0" "AT+BT_PAIR"
uv run python tools/at_cli.py --role dongle --listen 6 "AT+BT_SCAN=3"

uv run python tools/ci/board_roles.py     # 列出全部板子 端口→角色(--wait/--require)
```

**构建与烧录:**

```bash
tools/ci/build_all.sh                     # 三变体构建 → tools/ci/out/{kbd,dongle,dual}.hex
tools/ci/flash.sh <hex> [role]            # wlink 烧调试线连着的那块 + 烧后角色校验
uv run python tools/ci/isp_flash.py <hex> --port /dev/ttyACMx  # ISP 免挪线烧录(§5 方案 B)
tools/ci/loop_test.sh                     # M3 一键:构建→ISP 烧双板→闭环测试(--wlink 走旧流程)
```

端口识别约定:VID `0x1A86` 且 PID ≠ `0x8010`(排除 WCH-Link;
ISP bootloader 是 `4348:55e0`,VID 不同,天然不干扰)。
at-node 板实际 PID = **`0x2107`**(见 `usb_dev.c` 设备描述符)。

典型双板测试台(lser 输出):

```
/dev/ttyACM0  1a86:2107  at-node CDC+HID   ← 板子(角色以 AT+VER 为准)
/dev/ttyACM1  1a86:2107  at-node CDC+HID   ← 板子
/dev/ttyACM2  1a86:8010  WCH-Link          ← 调试器,工具脚本已排除
```

## 7. 已知坑清单

| 坑 | 现象 | 对策 |
|----|------|------|
| shell 状态不保持 | 构建报 `riscv-none-embed-gcc: not found` | 每条命令前 `source env.sh` |
| MRS 重新生成 | Windows 路径复发 | §3.1 的 sed 一键修复 |
| 大小写 | `fatal error: xxx.h: No such file` | 文件名全小写约定 |
| 串口权限 | `PermissionError: /dev/ttyACM0` | §4 udev 规则 |
| `sed */subdir.mk` 漏层 | APP/BLE、APP/HWS 是两层深 | 必须加 `APP/*/subdir.mk` |
| ttyS0–31 噪音 | pyserial 列出 32 个主板串口 | 按 VID 过滤,工具脚本已处理 |
| **绑定失配回连环** | 一侧擦了绑定另一侧没擦 → dongle 拿着旧 LTK 回连 → 加密失败 → 断 → 立即再连,`+BT_*` 刷屏淹没 AT 响应,板子最终失去响应 | 预防:换绑时**两侧都擦**(kbd 和 dongle 的 `AT+BT_PAIR` 各管各的);恢复:刷屏间隙 spam `AT+BT_AUTO=0` 止环 → `AT+BT_PAIR` 清绑 → 重配对;不行就 wlink `flash -e` 全擦重刷。固件已加固:未 armed 即断的连续 5 次失败自动 hold 并提示重配对 |
| **dongle 板 ISP 握手不稳** | `isp_flash.py` 对 dongle 板反复 handshake miss(kbd 板每次都成) | 该板 USB 连接疑似临界;dongle 用 wlink 烧录(台架调试线常驻 dongle 板),`loop_test.sh` 默认即此混合模式 |
| **VMware USB 仲裁** | 设备复位/重枚举后从 VM 消失(被 Windows 主机抢走);WCH-Link/ISP 设备"掉线" | VM 设置 → USB 控制器:勾选"自动连接新 USB 设备" + "显示所有 USB 输入设备"(at-node 是 HID 键盘,默认被当输入设备隐藏) |

## 8. 下一步(PLAN.md M3)

- [x] 烧录工具落地:wlink(WCH-Link,已验证 CH582)/ wchisp(已装,未验证)
- [x] Linux 构建闭环:env.sh + makefile 相对路径化,产物与 Windows 基线一致
- [ ] 串口权限:udev 规则待执行(§4,需 sudo)
- [x] `tools/ci/build_all.sh / flash.sh / loop_test.sh` 三脚本(待双板实测一键全绿)
- [ ] OpenOCD + WCH-Link 备选路径验证(只读先行)
- [ ] `AT+VER` 角色检测 + udev 按角色固定端口名(by-path 或序列号软链)
