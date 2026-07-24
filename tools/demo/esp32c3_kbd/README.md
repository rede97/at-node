# esp32c3_kbd — ESP32-C3 可编程模拟键盘(PLAN.md §8 阶段六)

> 任务简报(2026-07-22 立项,Windows 侧开发)
> 目标:ESP32-C3 作为 BLE HID 键盘外设 + WiFi HTTP 控制面,
> 替代/补充 CH582 kbd 板,作为 CH582 dongle 的异构测试陪练。

## 背景(必读)

- 完整动机与步骤见 `software/PLAN.md` §8 与 §8.1
- Windows 开发踩坑要点见 `.pi/skills/esp32-windows/SKILL.md`（必读）
- 参照实现(同仓库):
  - `tools/esp32c3_probe/` — C3 BLE HID **主机** probe(验证过 AT-Node kbd 全链路)
- 被测对象:CH582 dongle(`DONGLE=1` 或 `MODE=DUAL` 固件),
  行为见 `software/APP/BLE/ble_dongle.c` —— boot 优先订阅,
  无 boot 走 report fallback(订阅全部 report CCCD,转发 len>=8 报告)

## 交付物

### ① 最简键盘 sketch(本目录,优先)

- BLE HID over GATT Peripheral,设备名 "C3-Kbd",广播含 0x1812
- boot keyboard input report(8 字节标准布局)可订阅
- Just Works 配对(NoInputNoOutput 或键盘常用配置)
- 通过 USB 串口接收简单文本命令发键(后备控制面)
- **验收**:对 CH582 dongle 跑通 `tools/test_dongle_loop.py`
  的等价流程(扫描可见→配对→订阅→按键通知到达)

### ② WiFi HTTP 控制面(主控制面,API 见 PLAN §8.1)

- mDNS `esp32kbd.local`
- v1 端点:`GET /status`、`POST /tap`、`POST /text`
- WiFi+BLE 共存(乐鑫官方支持),注意按键延迟抖动 10-100ms
  (功能测试无影响,不可用于延迟指标测量)

### ③ 场景扩展(后续,对应 PLAN ②③④)

- `POST /rpa?enable=&period_s=` — LE Privacy 地址轮换
- `POST /rf?state=off|on` — 射频硬关断模拟断电
- ~~`POST /map?name=boot|nkro_multi` — Report Map 变体~~
  **不实现**（RK/复杂键盘支持已废弃，见 PLAN §3）

## 快速开始（Windows + arduino-cli）

1. 拷贝 WiFi 配置：
   ```powershell
   Copy-Item wifi_config.h.example wifi_config.h
   # 编辑 wifi_config.h 填入 SSID 和密码
   ```

2. 首次安装依赖库（已装则跳过）：
   ```powershell
   # 只需要 NimBLE-Arduino，随 ESP32 core 3.x 已自带，无需额外安装。
   # 若编译提示缺少，可尝试：
   # arduino-cli lib install NimBLE-Arduino
   ```

3. 编译/上传：
   ```powershell
   cd tools/esp32c3_kbd
   .\build.ps1 -Port COM3   # C3 通常为 COM3，以实际为准
   ```

4. 作为键盘输入打字（需先连上 dongle，见第 5 步）：
   ```powershell
   cd tools
   .venv\Scripts\python c3_type.py --ip 192.168.1.27 "Hello World"
   # 若觉得太快丢键，可加大间隔：
   .venv\Scripts\python c3_type.py --ip 192.168.1.27 --ms 60 --gap 100 "Hello World"
   ```

5. 与 CH582 dongle 闭环测试：
   ```powershell
   cd tools
   .venv\Scripts\python test_dongle_c3.py --dongle-port COM4 --c3-ip 192.168.1.27
   # 或依赖 mDNS：
   # .venv\Scripts\python test_dongle_c3.py --dongle-port COM4
   ```

## 技术要点

- 库选择:**NimBLE-Arduino 直接实现最小 HID boot keyboard**，不再依赖高层封装。
  原因：ESP32-BLE-Keyboard 与 esp32 core 3.3.10 不兼容；ESP32BLECombo
  不暴露带 READ 属性的 boot input report，导致 CH582  dongle 的
  Read-Using-Char-UUID 发现失败。直接 NimBLE 可以精确控制特征属性。
- 配对:Just Works 即可(CH582 dongle 支持;MITM 不是当前目标)
- 串口命令风格对齐 at-node:一行一条,`KEY <mods>,<k1>..<k6>` / `TAP <ms>,<mods>,<k>`
- USB 串口只作后备,主控走 HTTP

## 测试对接

- 现有双板脚本 `tools/test_dongle_loop.py` 按 `AT+VER` 角色识别
  CH582 板;C3 侧控制后续扩展为 HTTP 客户端(角色标签 `c3`)
- 回归基线:`tools/test_dongle_loop.py` + `tools/test_dongle_hardening.py`
  全 PASS(CH582 kbd 板版本),C3 版需达到同等通过

## 约束

- 代码注释 ASCII only;sketch 放本目录 `tools/esp32c3_kbd/`
- 提交信息风格参照 git log(类型前缀 + 简述)
