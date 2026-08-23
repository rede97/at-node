# AT-Node rust-s3 (ESP32-S3, esp-hal + Embassy)

Zephyr 版的 Rust 重实现（后者已归档 commit `e759a2a`)。约束文档：
`MIGRATION.md`——选型、目录结构、工程纪律、阶段计划以它为准。

- **板子**:MuseLab nanoESP32-S3(ESP32-S3-WROOM-1-N8R8,8MB flash + 8MB octal PSRAM)
- **控制台**:UART0 115200,经 ESPLink 口(`/dev/ttyACMx`),原生 USB 口留给 OTG(R5)
- **状态灯**:WS2812 @ GPIO48(RMT ch0),预设 = boot 黄 / wifi 连接中蓝闪 / online 绿 / error 红
- **调色**:任意 RGB 色,三通道同一颜色语义(`#RRGGBB` | `r,g,b` | `off` | `auto`):
  `AT+LED=<spec>` / `AT+LED?`;HTTP `GET|POST /at-node/cmd/led?color=<spec>`;
  MQTT RPC `led`(`color=<spec>`,空 = 查询);Web UI「LED」页调色板(ability `led:"color"` 时显示)
- **LED 裁剪**:cargo feature `led-color`(默认开)。S3 的 LED 独占 GPIO48,与任何功能无冲突;
  `--no-default-features` 编译时驱动整体移除(无 RMT/无 led task),GPIO48 回到安全 GPIO 池,
  ability 报 `"led":"none"`,各通道设置命令返回 `led disabled`
- **USB HID 键盘**(cargo feature `kbd-usb`,默认开;R5):原生 USB 口(OTG,GPIO19/20)枚举为
  boot 协议键盘(8B report)。命令语义对齐 CH582:`AT+TAP=<key>[,<mods>][,<ms>]`(原子注入,
  首选)、`AT+KEY=<mods>,<k0..k5>`(裸 report,须 `AT+KEY=0,0` 释放)、`AT+KEY_STR=<text>`、
  `AT+KEY_SEQ=<ms>,<mods>,<k0..k5>,...`(≤8 条/命令,异步回放)、`AT+DEV=USB|BLE` / `AT+DEV?`;
  HTTP `POST /at-node/cmd/keyboard/<tap|text|key>`;MQTT `keyboard/tap|text|key`(broker
  方法名零改动)。路由层 `kb.rs` 持有全部时序,后端只发裸 report。

## 编译期功能矩阵与变体(`Cargo.toml [features]` + `build.sh`)

与 Arduino 变体(`features.h` + build.ps1/-sh)对齐的功能模型:**核心** =
`kbd-usb` / `kbd-ble`(R6)/ `led-color` / `hws`(GPIO·ADC·I2C);**通信** = `mqtt` /
`http` / `rathole`(WiFi 为底座不参与裁剪)。关掉的功能在三个通道统一报 `<x> disabled`,
ability/catalog 同步隐藏。

| Variant | cargo features | 用途 |
|---|---|---|
| `full`(默认) | 全开(kbd-ble 除外) | 完整节点 |
| `remoter` | full − rathole | 键盘 + MQTT + HTTP,无隧道 |
| `base` | full − rathole − http | 生产键盘,串口-only 配置 |
| `rathole` | led-color + http + rathole | 隧道测试专用(无 kbd/mqtt/hws) |

```bash
cd esp32/rust
./build.sh                 # full,仅编译
./build.sh remoter --flash # 编译 remoter 并烧录(默认 /dev/ttyACM0)
```
- **rathole 隧道**(cargo feature `rathole`,默认开;单隧道,与 Arduino 一致):协议 v1
  plain TCP/TCP-only(同 Arduino 裁剪),把**局域网主机**的 TCP 服务(如 LAN 另一台机器的
  SSH)反向穿透到 rathole server。`tunnel.1.local` 必须是**其他 LAN 主机**,不能是本机
  (smoltcp 无主机环回,127.0.0.1/本机 IP 均不可连;Arduino lwIP 无此限制)。
  三通道:`AT+TUNNEL=status|connect|disconnect|clear|enable[,<0|1>]` 与
  `AT+TUNNEL=server|token|service|local|auto|retry,<val>`;HTTP
  `GET|POST /at-node/cmd/tunnel/<status|config|enable|connect|disconnect|clear>`;
  MQTT `tunnel/*`。配置键 `rathole.enable` + `tunnel.1.*`(NVS 注册表)。
  架构:manager task 独占控制通道+1 条待机池,转发任务池 2 槽;45s 心跳判死、
  指数退避(retry 基线/30s 上限/100ms 切片)、12KB 堆守护;跨任务只翻转
  want_run/reconfig 原子标志(Arduino R3 纪律)。**MQTT 与隧道可共存**(实测堆余 ~63KB)。

## 构建 / 烧录

```bash
source ~/export-esp.sh          # espup toolchain (rustup channel "esp")
cd esp32/rust
cargo build --release
espflash flash --chip esp32s3 --port /dev/ttyACM0 \
    target/xtensa-esp32s3-none-elf/release/atnode-s3
cargo clippy --release -- -D warnings   # 提交门槛:零警告
```

串口纪律(MIGRATION §5.6):烧录前停掉占用串口的控制台;脚本复位用
RTS 脉冲(EN),不要假设"打开串口即复位"(pyserial 的 DTR/RTS 初态不稳定)。

## 阶段状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| R0 | 骨架:Embassy + esp-println/backtrace,WS2812,UART0 控制台 | ✅ 硬件验收 2026-08-20 |
| R1 | cfg(NVS 注册表)+ at 核心 + at_serial + AT/VER/HELP/SET/GET/KEYS/RST + LED | ✅ 硬件验收 2026-08-20 |
| R2 | wifi.rs(STA + 15s 看门狗)+ AT+WIFI/STATUS | ✅ 硬件验收 2026-08-20(冷启动 ~6s 重连、IP/RSSI 上报、ping 通;AP 断电自愈由同路径的 bogus-SSID 重试测试覆盖,未动实验室 AP) |
| R7 | hws.rs(GPIO/ADC/I2C)+ 对应 AT/HTTP 端点 | ✅ 2026-08-22:GPIO/ADC/I2C_SCAN(bit-bang)/I2C_R/I2C_W 全部实测通过(LCD1602 经 PCF8574 显示 Hello World);ADC 去掉 ch7/8 |
| R3 | mqttc.rs(v3.1.1 + TLS CA 强校验 + LWT + cmd→resp)+ AT+MQTT 系列 | ✅ 硬件验收 2026-08-20(见下) |
| R4 | httpd.rs 全路由 + 共享 SPA + AT+HTTP | ✅ 硬件验收 2026-08-20(见下) |
| R5–R6, R8 | 见 MIGRATION.md §7 | ⬜ |

## R4 验收记录(2026-08-20)

- **curl 端点清单全过**:SPA(gzip 6840B,单 Content-Type,ETag)、
  302 跳转、cmd/status、cmd/ability、help.json、at(raw AT)、
  gpio write/read、adc/read、i2c read/write(空总线错误路径)、
  mqtt status/config/connect/clear/publish/subscribe、
  config get/set/list、wifi/config、http status/config/clear、404。
- **浏览器 SPA 实测(Chromium)**:Status 页全字段(信号条/MQTT/HTTP/
  堆水位/特性行),MQTT 页表单回填(broker/port/auto/enable),Config、
  API 页正常;BLE/Tunnel 页按 ability 隐藏。首屏突发(11 并发)
  残留 ~1 个 ERR_CONNECTION_REFUSED(后台轮询,下拍重试自愈)。
- **HTTP 并发模型**:3 acceptor + 10 深 backlog + 3 连接 handler
  (picoserve `Server::serve` per connection)。教训:smoltcp 无内核级
  listen backlog,"accept→服务→再 accept"的单线程模型在 SPA 首屏
  突发下必现 RST;acceptor 与 handler 解耦后,慢速 SPA 下载期间
  10/10 并发 curl 全过。缓冲池用 FREE_IDX 索引回收(socket 借用
  不可跨任务,unsafe 单点,不变式:索引唯一即缓冲唯一)。
- **对齐修正(Arduino 权威,推翻 Zephyr 差异)**:MQTT 身份 =
  client_id `atnode-<hostname>`、topic `atnode/<hostname>/*`;
  cmd/resp 是 RPC(`<reqid> <method> <query>` → `{"id":..,...}`),
  不是 Zephyr 的裸 AT 行;info manifest = sys_info(device/hostname/
  ip/services 目录);注册表补 device.hostname(默认 `atnodeesp-XXXX`),
  device.name 默认改 `AT-Node-ESP-XXXX`(持久化键索引只能追加)。
- 修复链:① http.enable 默认关;② PubSub 订阅者扩容(wifi+mqtt+N
  http);③ SocketSet 容量 16(mqtt+acceptor+backlog);④ embassy task
  pool_size;⑤ 重复 wifi 凭据写入触发 driver 重配+活跃 TLS 会话卡死
  →同值跳过+变更先优雅 disconnect;⑥ mqtt 空闲分支不再因任意 cfg
  变更重新武装手动 disconnect。

## R3 验收记录(2026-08-20)

- 本地 atnode broker(amqtt,192.168.1.42:8883,TLS)+ API key 认证:
  连接、订阅 `atnode/<name>/cmd`、retained `state=online`、retained
  `info` JSON manifest 全部通过;`AT+VER`/`AT+GET` 经 cmd→resp 回环
  (与串口同一 `at::handle_line`,三通道等价)。
- `AT+MQTT=disconnect` → retained `state=offline` + DISCONNECT 优雅退出。
- **H4 重连无泄漏**:disconnect/connect 20 轮全过,每轮新建
  TCP/TLS/客户端三元组并 drop 旧连接;会话堆水位 157464 B 稳定
  (首轮后零漂移)。
- TLS:embedded-tls 0.19(pki 纯 Rust 验证器,RSA feature)+ CA DER
  内嵌(`certs/ca.der`,build.rs `have_ca` 门控;缺失时响亮降级)。
- 已知边界:amqtt broker 对 keepalive 超时的 LWT 投递未观察到
  (板等 115s 无 offline;优雅 disconnect 的 retained offline 正常)。
  固件 will 编码与 Zephyr 完全一致,判定为 amqtt 行为,另案跟踪。
- **调试中踩掉的三个真坑**(详情见"实现要点"):
  ① take_buffers 栈上初始化 35KB 数组顶爆 5.5KB executor 栈;
  ② embedded-tls pki 不支持 IP SAN(绕法:证书加 `DNS:<ip>` SAN);
  ③ rust-mqtt 只有 v5 而 amqtt 只讲 v3.1.1 → 协议栈换为自实现
  v3.1.1 mini client。

## R2/R7 验收记录(2026-08-20)

- 冒烟:`uv run python esp32/rust/tools/at_smoke.py` → **全绿**
  (R1 全套 + STATUS/WIFI 字段 + GPIO 黑名单与安全脚 + ADC 校准 mV +
  I2C 空总线错误路径)。`--slow` 追加 ~30s 的 I2C_SCAN 裸板检查。
- WiFi 看门狗:配置 `wifi.ssid` 后经 cfg pubsub 立即触发连接;
  失败后 16.5s 间隔重试(实测),LED 蓝闪。
- 内存水位:WiFi 驱动启动前 internal heap free **278544 B**
  (200KB 主堆 + 73KB reclaim)。
- 镜像:release 烧录 **~600 KB**(R7 后;WiFi blob 占大头)。

## 已知问题

- **AT+I2C_SCAN 已改 bit-bang**(2026-08-22):GPIO8/9 开漏+上拉直驱，
  START+ADDR+ACK+STOP，全程 ~0.3s（原 esp-hal 路径裸板 ~29s 且停摆
  executor);引脚一次性 claim/restore(I2C 输出路由 detach/restore),
  不进数据相位不弹设备寄存器。R/W 仍走 esp-hal 硬件 I2C。
- **I2C_W 写卡 EEPROM(已定案)**:"AT24C256" 模块是仿冒片(应答题
  0x50+0x58 双地址;任何写后读回全 FF,板复位不恢复,模块断电恢复;
  bit-bang 与硬件写同样卡)。非固件问题——换 PCF8574 后写路径
  实测通过(LCD1602 正常显示)。16 位寻址支持已加:reg>0xFF 自动
  双字节(AT24C256 等大容量 EEPROM 需要;单字节帧会把这类片子写
  到死锁)。临时诊断 AT+BBW/AT+BBR 已随结案删除。
- **ADC ch7/8 已移除**(AT+ADC=7/8 → "ERROR bad channel"):GPIO8/9 是
  I2C 总线脚，数字/模拟 pad 复用互斥；且这两通道从未读对过（恒
  4095,touch 脚的 esp-hal set_analog 映射疑似有坑）。Arduino 对齐
  记录于此。
- **R2 AP 断电自愈**:未动实验室 AP;link-loss→15s 重试路径由
  bogus-SSID 测试覆盖(实测 16.5s 间隔)。冷启动重连 ~6s 实测通过。
- GPIO 黑名单比 Zephyr 多禁了 48(本固件 48 是 WS2812/RMT)。
- AT+BBW 是写卡问题的临时诊断命令,R8 收尾时删除。

## R0/R1 验收记录(2026-08-20)

- 冒烟:`uv run python esp32/rust/tools/at_smoke.py` → **37 项全绿**
  (AT/VER/HELP、SET/GET/KEYS 全语义:默认值、bool/int 归一化与校验、
  write-only 密钥、未知键、bad args;LED 全分支;错误路径;
  AT+RST 复位后配置保持;NVS=clear 后回默认值)。
- 内存水位:boot 后 internal heap free **73744 B**(reclaimed 堆,尚未分配)。
- LED:GPIO48 肉眼确认点亮(boot 黄),AT+LED 自由色/关/auto 生效。

## AT 命令(R1/R2/R7 已实现)

```
AT / AT+STATUS / AT+VER / AT+HELP
AT+SET=<key>=<val> / AT+GET=<key> / AT+KEYS
AT+WIFI=ssid|pass,<val> / AT+WIFI=status
AT+GPIO_W=<pin>,<level> / AT+GPIO_R=<pin>
AT+ADC=<ch>                # ADC1 ch0..9 = GPIO1..10,校准 mV
AT+I2C_SCAN / AT+I2C_R=<addr>,<reg>,<len> / AT+I2C_W=<addr>,<reg>,<d0>,...
AT+LED=<r>,<g>,<b>|off|auto     # 数值 base-0(支持 0x..)
AT+NVS=clear / AT+RST
```

响应约定(对齐 Arduino):数据行在前,恰好一行 `OK` / `ERROR <reason>` 结尾。
`AT+KEYS` 输出单行 `+KEYS:[{"key":..,"value":..}|{"key":..,"secret":true}]`
(Arduino config_list_json 格式,HTTP config 端点 R4 复用同一 JSON)。

## 配置键空间(13 键)

`device.name`(默认 `AT-Node-S3-XXXX`,XXXX=efuse MAC 末 2 字节)、
`wifi.ssid`、`wifi.pass`(write-only)、`mqtt.broker`、`mqtt.port`
(1–65535,默认 8883)、`mqtt.user`、`mqtt.pass`(write-only)、
`mqtt.auto`、`mqtt.enable`、`http.auto`、`http.enable`、`ble.auto`、`ble.enable`
(bool 归一化 "1"/"0",接受 true/false)。

与 Arduino 键空间的差异(有意):无 `device.hostname`(mDNS 是非目标)、
无 `mqtt.ca`(Rust 版内嵌 CA DER)、无 rathole/tunnel.*(非目标)。

## 实现要点(排障参考)

- **MQTT 协议栈(与 MIGRATION §3 的偏差,已记录)**:选型表的
  rust-mqtt 被替换为 `mqttc.rs` 内置的 v3.1.1 mini client(~250 行)。
  原因:rust-mqtt 0.3 的 v3 全部 stub 返回 UnsupportedProtocolVersion,
  0.5 的 v3 模块为空;而生产 broker(amqtt,含云端部署)协议级别=4
  只讲 3.1.1。mini client 只实现 Zephyr 契约面:CONNECT(will/clean
  session/user+pass)、SUBSCRIBE 单 topic QoS0、PUBLISH QoS0、
  PINGREQ、DISCONNECT。
- **embedded-tls 用 `pki` 验证器,不用 `webpki`**:rustls-webpki 依赖
  ring,ring 不为 `xtensa-esp32s3-none-elf` 构建(按大端目标编译,
  链接即失败)。pki 是纯 Rust(der + p256/ecdsa/rsa),需 `rsa`
  feature + `enable_rsa_signatures()`(开发 CA 是 RSA-2048)。
- **eio 0.6↔0.7 双向桥**:embassy-net/rust 侧用 embedded-io-async
  0.6,embedded-tls 0.19 用 0.7;`mqttc.rs` 的 Eio6/Eio7 newtype 直接
  委托,错误 kind 尽力映射(任何错误都终结会话)。
- **executor 栈与堆共享 dram_seg**:`heap_allocator!(size: N)` 的静态
  数组越大,executor 栈越小。200KB 堆把栈压到 5.5KB,TLS 握手
  (SHA256/RSA 大数)顶爆 stack guard(现象:boot 后
  "write to the stack guard value" panic)。现 128KB 堆 ≈ 78KB 栈。
  同理:`StaticCell::init([0; N])` 会在调用者栈上物化整个数组——
  大静态缓冲一律 `uninit()` + `ptr::write_bytes` 原地清零。
- **amqtt broker 只讲 v3.1.1**:它的 CONNECT 解码没有 v5 properties
  字段,v5 CONNECT 的属性长度字节会被当成 client_id 长度 → 解码
  挂起等数据(表象:broker ACK 但不回 CONNACK)。
- **持久化**:sequential-storage 8 map 模式,键 = 注册表索引(u8),值 ≤63B;
  区域 = flash 末尾 64KiB(`0x7F0000..0x800000`,app 远够不着,espflash
  烧录不擦该区域)。RAM 全量缓存,读不落盘。
- **esp-storage 必须开 `bytewise-read`**:不开时 read() 强制 length%4==0,
  sequential-storage 的非对齐读全部 NotAligned(store 表现为写入失败)。
- **trait 桥**:esp-storage 实现 embedded-storage 0.3(同步),
  sequential-storage 8 要 embedded-storage-async 0.4;`cfg.rs` 的
  `EspFlash` newtype 直接委托(ROM flash 操作本来就是阻塞的)。
- **UART 写必须循环**:`write_async` 在 TX FIFO(128B)满时返回短写,
  不循环会把长响应(KEYS json ~700B)截断在 FIFO 边界。
- **flash 写与多核**:`FlashStorage::multicore_auto_park()`(S3 双核,
  默认策略在另一核运行时直接报错)。
- **clippy**:`.clippy.toml` 把 large_stack_frames 阈值调到 8KiB——
  Embassy 任务 future 住在 executor 静态任务池里,不占调用栈,
  默认 1KiB 阈值对 async fn 全是误报。
- **LED**:RMT 80MHz、GRB 线序、预设亮度 0x20(Zephyr 语义);
  所有 RMT 传输只在 led_task 里(回调/中断里绝不做外设事务,§5.2)。

## 目录

```
src/main.rs      启动:时钟/堆/esp-rtos/cfg/LED/WiFi/HWS/MQTT/HTTP/AT 串口
src/cfg.rs       NVS 配置注册表(14 键,write-only 密钥,RAM 缓存,变更 pubsub)
src/at.rs        AT 解析/分发(通道无关,AtSink trait)
src/at_serial.rs UART0 控制台(回显/退格/Ctrl-C/CRLF 吞咽,300B 行缓冲)
src/led.rs       WS2812 状态灯(预设 + 调色:共享 parse/current,AT/HTTP/MQTT 同语义)
src/wifi.rs      WiFi STA + 15s 重连看门狗(embassy-net DHCP,同值免重配)
src/mqttc.rs     MQTT v3.1.1 mini client + TLS + RPC cmd/resp + pub/sub API
src/rathole.rs   rathole v1 隧道客户端(单隧道,plain TCP,转发 LAN 主机;不代理本机)
src/hws.rs       GPIO/ADC/I2C(引脚黑名单,ADC 校准 mV,I2C 100kHz @8/9)
src/api.rs       ability/services 目录/sys_info(Arduino API_CATALOG 子集)
src/httpd.rs     HTTP 控制面(picoserve,acceptor+backlog+handler 模型)
tools/at_smoke.py    硬件冒烟
tools/lcd_hello.py   LCD1602(PCF8574 backpack)Hello World 演示
```
