# R6:BLE HID 键盘(Rust S3)实施计划

> 状态:📋 计划(未开工) · 维护:esp32/rust/
> 关联:`MIGRATION.md` §7 R6 行、`esp32/arduino/README.md`(Arduino BLE 安全模型)

## 1. 目标

Rust S3 变体实现 **BLE HID 键盘**,与 USB HID(R5)共用 `kb.rs` 路由层;
安全模型对齐 Arduino 变体(配对窗口 + bond 持久化)。

```
AT/HTTP/MQTT → kb.rs(时序引擎)
   ├─ kbd_usb(R5,已完成)
   └─ kbd_ble(本计划)—— trouble-host,peripheral
        ├─ GATT HID 0x1812
        ├─ 配对窗口 + bond(NVS)
        └─ 广播/连接管理
```

## 2. 技术选型

| 项 | 决定 | 依据 |
|---|---|---|
| BLE 栈 | esp-radio 0.18 `ble` feature + **trouble-host 0.4** | 与 esp-hal 1.1 官方对齐;embassy 原生 |
| 控制器 | `BleConnector` → `ExternalController<_, 1>` | esp-hal 官方样例模式 |
| 资源 | `HostResources<_, DefaultPacketPool, 1, 1>`,缓冲池放 **PSRAM_HEAP** | 已定内存纪律(README 内存布局) |
| GATT | trouble attribute table(手写或宏) | HID 服务 0x1812 全套 |
| 键盘格式 | boot protocol 8B report,与 kbd_usb 同一 Report Map | 双模行为一致 |
| bond 存储 | NVS cfg 注册表 `ble.*` 键(**append-only 追加**,勿插入) | 复用现有注册表 |
| cargo feature | `kbd-ble`(占位已存在,本轮落地) | 功能矩阵(Cargo.toml) |

## 3. 阶段分解

| 阶段 | 交付物 | 验收标准 |
|---|---|---|
| **R6.0** 控制器点亮 | esp-radio `ble` feature 接线;`BleConnector` + `ExternalController` + `HostResources`;trouble peripheral 最小广播(device.name) | `bluetoothctl scan on`(hci0)发现 AT-Node;heap 水位记录 |
| **R6.1** GATT HID | HID Service 0x1812:Report Map / HID Information / HID Control Point / Protocol Mode / Input Report(notify)/ Output Report(LED)/ Boot Keyboard In·Out;广播含 HID UUID + keyboard appearance | `bluetoothctl info` 可见 0x1812 服务与全部特征 |
| **R6.2** 路由接入 | `kb::emit()` → `kbd_ble` 后端(连接态写 input report notification);`AT+DEV=BLE` 生效;无连接时丢弃不阻塞引擎 | `AT+DEV=BLE` + `AT+KEY_STR=Hi`,BLE 主机收到击键 |
| **R6.3** 配对窗口安全 | 默认不可配对;`AT+PAIR` / `ble/pair` 开 60s 窗(Just Works);bond 写 NVS;开窗后仅 bonded 主机可重连(地址过滤);`AT+BLE=status\|bonds\|clear`;HTTP/MQTT 面对齐 Arduino | 窗口内配对成功;窗口外配对被拒;bond 重启保持;清 bond 后需重新配对 |
| **R6.4** 验收 | hci0 + bluetoothctl 作 BLE 主机(BlueZ HoG → VM evdev 键盘) | 配对→打字(VM evdev 实测)→断连→**bonded-only 重连**→清 bond;快连快断 20 轮无泄漏;`btmon` 全程留证 |

## 4. 涉及文件

| 文件 | 改动 |
|---|---|
| `esp32/rust/Cargo.toml` | esp-radio `ble` feature;trouble-host 依赖 |
| `src/kbd_ble.rs`(新) | BLE 控制器、GATT HID、配对窗口状态机 |
| `src/kb.rs` | `emit()` 增加 BLE 后端分支(Target::Ble) |
| `src/cfg.rs` | `ble.enable` / `ble.auto` / `ble.bonds`(append-only 追加) |
| `src/at.rs` | `AT+BLE=` / `AT+PAIR` 命令 |
| `src/httpd.rs` | ble 系列端点(对齐 Arduino:`ble/status|pair|bonds|clear`) |
| `src/mqttc.rs` | ble/* RPC 方法 |
| `src/api.rs` | ability `ble:true` + catalog 条目 |
| `main.rs` | ble runner + host task 接线 |

## 5. 已知风险与对策

| 风险 | 对策 |
|---|---|
| esp-radio 0.18 在 S3 上加密 bond 重连间歇失败([esp-hal#5877](https://github.com/esp-rs/esp-hal/issues/5877)) | R6.4 含 20 轮重连压测;命中则重连间隔纪律(≥3s),或降级为"窗口内免 bond 直连"并记录 |
| trouble-host 内存峰值 | buffer pool 进 PSRAM_HEAP;R6.0 记录 heap 水位作为基线 |
| BLE+WiFi 单射频共存(Arduino 实测:键盘连着时 WiFi 周期性抖动) | 预期内物理约束,记录;MQTT/HTTP 已有重连纪律覆盖 |
| GATT 表内存 | attribute table 静态化;PDU 走 PSRAM pool |

## 6. 测试基建(已就位 2026-08-24)

- **nRF52840 Dongle**(hci_usb 固件)→ VM 的 `hci0`(btusb 直挂)
  - `bluetoothctl`:扫描/配对/连接
  - `btmon -i hci0`:HCI 全解码抓包,验收留证
- **BlueZ HoG**:BLE HID 设备在 VM 上直接出现为 evdev 键盘——与 USB HID 同一套击键断言路径(`/dev/input/event*`)
- 主机 evdev 事件流:击键(按下=1/重复=2/释放=0)可编程断言

## 7. 验收清单(R6.4 逐项)

- [ ] `bluetoothctl scan on` 发现 `AT-Node-ESP-XXXX`,appearance = keyboard
- [ ] 默认状态 `pair` 被拒(安全);`AT+PAIR` 开窗后 60s 内配对成功
- [ ] `AT+KEY_STR=Hello` → VM evdev 收到 `H e l l o`(含 shift 修饰)
- [ ] `AT+TAP=0x39` → CapsLock;主机 LED output report 回到 S3(可选验证)
- [ ] 断连后 bonded 主机自动重连可打字;**非 bonded 主机连接被拒**
- [ ] 快连快断 20 轮:无崩溃、无内存泄漏(heap 水位不回降)
- [ ] `AT+BLE=clear` 清 bond 后,重连要求重新配对
- [ ] btmon 留证:SMP 配对、ATT 服务发现、HID notification 时序
