# R6:BLE HID 键盘(Rust S3)实施计划

> 状态:✅ 已实施(2026-08-24 R6.0–R6.4 全阶段验收通过) · 维护:esp32/rust/
> 关联:`MIGRATION.md` §7 R6 行、`esp32/arduino/README.md`(Arduino BLE 安全模型)
>
> 实施与计划的偏差(全部为实测驱动的最终决定):
> 1. **trouble-host 0.4 → 0.6.0**:0.4 锁 bt-hci 0.6,esp-radio 0.18 锁 bt-hci 0.8,
>    trait 不互通;0.6.0 是唯一兼容对(esp-radio 官方 bas_peripheral 样例同配)。
> 2. **新增 `wifi` cargo feature**:WiFi 从"恒开底座"降级为 feature
>    (mqtt/http/rathole/ssdp 均隐含依赖),`ble` 变体(kbd-ble + led-color,
>    WiFi OFF)服务纯 BLE 场景。**WiFi+BLE 已验证可共存**(2026-08-24,
>    full+kbd-ble 固件:WiFi up + HTTP ble/* 端点 + BLE 打字 + MQTT/TLS
>    并行),条件:① 内部堆 77.7K→129.7K(静态堆数组 4K→56K,对齐 esp-hal
>    embassy_coex 样例的 128K;executor 栈余 ~120K,TLS 握手实测无碍);
>    ② `max_connections ≥ 2`(esp-radio os_adapter 以之充当 ble_max_act,
>    =1 时控制器 activity 额度连 advertiser 都不够 → HCI 0x07 Memory
>    Capacity Exceeded;取 3 = 1 conn + 1 adv + 余量)。初判"内存不共存"
>    (`BLE assert emi.c 164`)实为 ①+② 叠加。
> 3. **bond 存储**:`ble.bond` 单键(append 注册表尾,F_WO),CH582 单 bond 语义
>    (新配对覆盖);40B 二进制 hex 80 字符 → VAL_MAX 64→96。
> 4. **静态随机地址 efuse 派生**:S3 控制器 public BD_ADDR 不可靠,不设地址
>    广播不上空气;`FC|MAC`(静态随机位)= `FC:DF:A1:E3:A0:14`。
> 5. **kb.rs 默认 target 按编译 feature**:kbd-ble-only 构建默认 BLE,否则
>    reboot 后键被不存在的 USB 后端静默吞掉。
> 6. **Report Reference id=0**:report map 无 Report ID 项,描述符写 id=1 会让
>    HoG 剥掉通知首字节丢键。

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
| BLE 栈 | esp-radio 0.18 `ble` feature + **trouble-host 0.6.0** | 与 esp-hal 1.1 官方对齐;embassy 原生 |
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

## 7. 验收清单(R6.4 逐项,2026-08-24 全过)

- [x] `bluetoothctl scan on` 发现 `AT-Node-ESP-A014`,appearance = keyboard
      (0x03c1,icon input-keyboard;地址 FC:DF:A1:E3:A0:14 静态随机)
- [x] 默认状态连接被 S3 主动断开(`ble: reject non-bonded`,BlueZ 报
      le-connection-abort-by-local);`AT+PAIR` 开窗 60s 内 Just Works 配对成功
- [x] `AT+KEY_STR=Hello` → VM evdev 收到 `H e l l o`(LEFTSHIFT+H press/release,
      E/L/L/O;BlueZ HoG 生成 /dev/input/event6)
- [x] `AT+TAP=0x39` → CapsLock(主机 input271::capslock 翻转);LED output
      report 回到 S3(串口 `ble: write h=0x15 [..]`,双向验证)
- [x] 断连后 bonded 主机自动重连(LTK 加密恢复)可打字;非 bonded 拒连
- [x] 快连快断 29 轮(10+19):无崩溃,internal heap 恒定 42044 B 零泄漏
- [x] `AT+BLE=clear` 后重连被拒,需重新 `AT+PAIR` 配对
- [x] 留证:btmon 抓到 HCI `LE Start Encryption` + `Encryption Change
      AES-CCM`(bonded 重连);SMP/ATT/HID 时序以串口日志留证
      (`ble: paired Encrypted` / `ble: write h=0x12 [1, 0]` CCCD 订阅 /
      notify 无错)。**限制**:本测试环境 btmon 抓不到 ACL 数据层
      (nRF52840 hci_usb 固件),仅 mgmt/HCI 命令事件可见。

## 8. 实测坑录(R6 实施过程)

| WiFi+BLE 内部堆不足 | `BLE assert emi.c 164` panic(31.4KB free vs ~30.7KB 需求) | ~~WiFi 降级~~ 初判误诊;终局:堆 77.7K→129.7K(对齐官方 coex 样例 128K),共存验证通过。`wifi` 仍保留为 cargo feature(纯 BLE 变体用) |
| **ble_max_act 被误砍** | 共存排查时把 `max_connections` 设为 1 省内存,advertise 全部 `HCI 0x07 Memory Capacity Exceeded` | esp-radio os_adapter 以 `max_connections` 充当 `ble_max_act`(连接+广播+扫描共享额度),=1 连 advertiser 都不够;设 3(1 conn + 1 adv + 余量)。ESP-IDF 官方公式:MAX_ACT ≥ conn + adv + scan + psync |
| HCI 0x07 排查弯路 | 依次排除堆(129K 仍败)、初始化顺序(BLE 先行仍败)、coex feature(无关)——真凶是 max_act | 教训:**HCI 0x07 是控制器 activity/资源额度,不是系统堆**;变量隔离时一次只改一个(本次 max_connections 改动与 WiFi-on 同步引入,造成归因混淆) |
| esp-radio `coex` 依赖 wifi 模块 | coex 挂 kbd-ble 时 ble 变体(无 wifi)编译报 `crate::wifi` 不存在 | coex 只能与 `esp-radio/wifi` 同开;最终未启用(实测无 coex 仲裁也稳定,官方样例配置留作后备) |
| 控制器无可靠 public 地址 | 广播不上空气,扫描零结果 | efuse MAC 派生静态随机地址(raw[5]\|=0xC0) |
| embassy-sync 0.8 vs 0.7 | gatt_server 宏类型跨版本不匹配 | 项目降 embassy-sync 0.7.2 |
| trouble 默认不可 bond | 主机配对即 PairingFailed 断连 | 连接后按窗口 `set_bondable` |
| bond 仅 RAM | reflash 后 LTK Authentication Failure 死循环 | `ble.bond` NVS 持久化(R6.3 核心) |
| Report Reference id=1 | HoG 剥首字节,evdev 零事件 | id=0(report map 无 Report ID) |
| reboot 后 target 复位 USB | ble-only 构建吞键无报错 | kb.rs 默认 target 按 feature |
| Stack 非 Sync | 全局引用方案编译失败 | stack 作任务参数;AT 清 bond 走 Signal |
| BlueZ list-attributes 残缺 | 只显示 0x1801 | BlueZ 缓存/显示问题;gatttool plain-ATT 验证表完整,HoG 实测正常 |
| BlueZ 侧陈旧 bond | S3 清 bond 后主机仍持旧 LTK → Authentication Failure 循环 | 测试纪律:设备端 `AT+BLE=clear`/reflash 后,主机侧必须 `bluetoothctl remove` 对齐 |
| BlueZ agent 拦截 Just Works | 脚本化配对停在 "Accept pairing (yes/no)" 超时 | 管道里补 `yes`(NoInputNoOutput agent 在 bluetoothctl 下仍提示) |
| VAL_MAX 64→96 连锁 | rathole/wifi 的 `String<64>` 消费方编译错 | 消费方统一改 `String<{cfg::VAL_MAX}>` |
| btmon 抓不到 ACL 层 | 只见 mgmt/HCI 命令事件与扫描报告 | 留证改用串口 SMP/ATT 日志 + HCI 加密事件(nRF52840 hci_usb 固件限制) |
| 测试脚本轮次虚报 | 快连快断脚本 20 轮实际只成 10(trusted auto-connect 竞争) | 每轮以 `Connection successful` 计数,失败重跑补足 |
| WiFi+BLE 内部堆不足 | `BLE assert emi.c 164` panic(31.4KB free vs ~30.7KB 需求) | `wifi` 降级为 cargo feature;`ble` 变体 WiFi OFF |
| 控制器无可靠 public 地址 | 广播不上空气,扫描零结果 | efuse MAC 派生静态随机地址(raw[5]\|=0xC0) |
| embassy-sync 0.8 vs 0.7 | gatt_server 宏类型跨版本不匹配 | 项目降 embassy-sync 0.7.2 |
| trouble 默认不可 bond | 主机配对即 PairingFailed 断连 | 连接后按窗口 `set_bondable` |
| bond 仅 RAM | reflash 后 LTK Authentication Failure 死循环 | `ble.bond` NVS 持久化(R6.3 核心) |
| Report Reference id=1 | HoG 剥首字节,evdev 零事件 | id=0(report map 无 Report ID) |
| reboot 后 target 复位 USB | ble-only 构建吞键无报错 | kb.rs 默认 target 按 feature |
| Stack 非 Sync | 全局引用方案编译失败 | stack 作任务参数;AT 清 bond 走 Signal |
| BlueZ list-attributes 残缺 | 只显示 0x1801 | BlueZ 缓存/显示问题;gatttool plain-ATT 验证表完整,HoG 实测正常 |
