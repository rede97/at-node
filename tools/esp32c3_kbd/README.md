# esp32c3_kbd — ESP32-C3 可编程模拟键盘(PLAN.md §8 阶段六)

> 任务简报(2026-07-22 立项,Windows 侧开发)
> 目标:ESP32-C3 作为 BLE HID 键盘外设 + WiFi HTTP 控制面,
> 替代/补充 CH582 kbd 板,作为 CH582 dongle 的异构测试陪练。

## 背景(必读)

- 完整动机与步骤见 `software/PLAN.md` §8 与 §8.1
- 参照实现(同仓库):
  - `tools/esp32c3_probe/` — C3 BLE HID **主机** probe(验证过 AT-Node kbd 全链路)
  - `tools/rk_recon/rk_recon.ino` — C3 对 RK 键盘的 GATT 侦察 sketch
    (含 boot 订阅、protocol-mode 写、Report Map dump 的完整代码模式)
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
- `POST /map?name=boot|nkro_multi` — Report Map 变体
  (RK 风格:NKRO ID1 + boot8 ID2 + 消费控制 ID4)

## 技术要点

- 库选择:**NimBLE** 优先(内存小、行为透明);`ESP32-BLE-Keyboard`
  库(Bluedroid)可作为快速起点但注意体积
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
