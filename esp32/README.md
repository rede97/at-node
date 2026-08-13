# esp32/ — ESP32-C3 AT Node (network-enabled variant)

> 基于 ESP32-C3 的 at-node 网络版实现。
> 与 CH582 版本共享同一套 AT 命令语义，但用 WiFi HTTP / MQTT 代替 USB CDC / BLE NUS。

## 目录规划

| 路径 | 用途 |
|------|------|
| `esp32/README.md` | 本文件，ESP32 版整体说明 |
| `esp32/PLAN.md` | ESP32 版实现计划（阶段、接口、验证） |
| `esp32/API.md` | HTTP API 参考文档（agent 集成用） |
| `.pi/skills/esp32-windows/` | Windows/ESP32-C3 开发踩坑要点（pi skill） |
| `esp32/esp32_at_node/` | ESP32-C3 AT Node 主 sketch |
| `tools/demo/esp32c3_kbd/` | 现有 C3 BLE 键盘测试台（保留，作为 dongle 陪练） |

## 与 CH582 版的关系

| 维度 | CH582 版 | ESP32-C3 版 |
|------|----------|-------------|
| 主控 | CH582F | ESP32-C3 |
| 传输 | USB CDC + BLE HID/NUS | WiFi HTTP + MQTT (TLS) |
| 键盘 | BLE HID Peripheral | BLE HID Peripheral（同 C3 测试台） |
| 接收器 | BLE HID Central | 无（可选后续） |
| AT 命令 | `AT+...` 文本协议 | `POST /at/<cmd>` JSON 协议 |
| 供电 | USB / 电池 | USB / 电池 |

**目标**：同一份 Agent 脚本（Python/JS），CH582 走串口，ESP32 走 HTTP，
命令语义完全一致。

## 当前状态

- ✅ HTTP 基础：`/at-node/status`, `/at-node/at`, `/at-node/cmd/keyboard/{tap,text,key}`
- ✅ BLE 键盘：NimBLE boot keyboard，设备名 `AT-Node-ESP`，CH582 dongle 已验证
- ✅ GPIO / ADC：HTTP + 串口 + 原生 AT 全通
- ✅ I2C：扫描/读写，SDA=GPIO8, SCL=GPIO9
- ✅ IR：RMT 38kHz 载波，NEC/SIRC/RAW，GPIO4
- ✅ 串口全功能：与 HTTP 等价的完整 AT 命令集
- ✅ 测试脚本：`tools/test/test_esp32_at_node.py` 全 PASS
- ✅ WiFi 凭据：NVS 持久化，HTTP/串口可配置
- ✅ AP 配网：GPIO10 触发（3 秒）或 `AT+AP=1`，Captive Portal 页面
- ✅ 设备名/hostname：默认含芯片 ID 后缀（如 `AT-Node-ESP-5688`），可配置
- ✅ HTTP 页面：`/` 重定向到 `/at-node/status`（HTML），`/at-node/cmd/status`（JSON），`/at-node/help`（API 文档），`/at-node/help.json`（机器可读 API 目录）
- ✅ MQTT：TLS (8883) + SHA256 指纹验证；plain TCP (1883) 兼容模式；指纹 NVS 可配置（无嵌入式 CA/PEM）
- ✅ MQTT 远程 broker：TLS + 指纹验证连接云服务器成功；MQTT 操作移至独立 FreeRTOS task（不阻塞 HTTP/串口）
- ✅ MQTT auto-connect：NVS `mqtt_auto` 开关，首次手动连接成功后自动启用，重启自愈；`AT+MQTT=auto,<0|1>` 可控制
- ✅ HTTP 开关：`AT+HTTP=status|enable,<0|1>|clear|0|1` 与 `/at-node/cmd/http/{status,config,clear}` 对齐 MQTT 风格，状态持久化到 NVS
- ✅ BLE 配对安全：默认不广播，需通过 `AT+PAIR=1` / `POST /at-node/cmd/ble/pair?enable=1` / MQTT `ble/pair?enable=1` 显式进入 60s 公共配对模式；配对后断连转为定向广播，仅已绑定主机可连
- ✅ NVS 擦除：`AT+NVS=clear` / `POST /at-node/cmd/nvs/clear` 恢复出厂设置并自动重启
- ✅ rathole 内网穿透客户端：**单隧道**（plain TCP transport，一条 SSH 即可跳板，降低公网暴露），`AT+TUNNEL=<1>,...` 串口配置 + Web 配置页 + REST 端点，NVS 持久化可开机自连；实测穿透 HTTP 控制面与 TCP echo 全通
- ✅ Web 控制面：`esp32/web/` 独立前端项目，`uv run python esp32/web/build.py` 打包内联 + gzip 成**单页应用**（`web_page.h`，~4.9KB），固件从 flash 一次性发送（`Content-Encoding: gzip`），页面内所有状态/配置均由 JSON `/at-node/cmd/*` 驱动；旧分页 URL（`/at-node/status|mqtt|tunnel|pair|help`）302 到 `/`。Config 页支持**配置导出/导入**（JSON 文件，纯浏览器 JS 解析 + 逐键调 `/at-node/cmd/config`，固件零 JSON 库开销；不支持的键自动忽略并在页面列出，密钥类不导出）

### 统一配置层

所有持久化配置收敛到单一注册表入口 `config_set/get/list`,AT / HTTP / MQTT 三通道等价：

- AT:`AT+SET=<key>=<val>` / `AT+GET=<key>` / `AT+KEYS`
- HTTP:`POST /at-node/cmd/config?key=..&val=..`,`GET` 同路径读取，`GET /at-node/cmd/config/list` 全量
- MQTT:`config/set` `config/get` `config/list` 方法
- 键空间：`device.name|hostname`、`wifi.ssid|pass`、`mqtt.broker|port|user|pass|ca|auto`、`http.enable`、`rathole.enable`、`tunnel.1.server|token|service|local|auto|retry|enable`；密码/token 类为只写
- 存量命令(`AT+WIFI=`、`AT+MQTT=`、`AT+HTTP=`、`AT+CONF=`、`/at-node/cmd/{wifi,mqtt,http}/config` 等）保留为注册表别名，老脚本不受影响

### 功能宏与固件变体（`features.h` + `build.ps1 -Variant`）

编译期开关：`FEATURE_BLE` / `FEATURE_MQTT` / `FEATURE_RATHOLE` / `FEATURE_I2C` /
`FEATURE_HTTP`（默认全 1）。关掉的功能其初始化、AT 分支、REST 路由、配置键全部不编译，
`AT+GET` 对应键返回未知键。变体：

| Variant | 宏 | 用途 |
|---|---|---|
| `full`（默认） | 全开 | 完整键盘节点 |
| `base` | `RATHOLE=0 HTTP=0` | 生产键盘节点（BLE+MQTT+I2C），无隧道、**无 LAN HTTP 控制面**（防局域网攻击，仅串口配置；按钮触发的 AP 配网页 8080 不受影响） |
| `rathole` | `BLE=0 MQTT=0 I2C=0` | 隧道专用测试板（free_heap ~180K vs 全开 ~20K） |

```bash
powershell -File esp32/esp32_at_node/build.ps1 -Port COMx -Variant rathole
```

**Ability 接口**：`AT+ABILITY` / `GET /at-node/cmd/ability` / MQTT `ability` 方法
返回 `{"ble","mqtt","rathole","i2c","http","breath_led"}`（同时内嵌在
`/at-node/cmd/status` 里）；Web UI 据此隐藏无对应功能的标签页并在 Status 页显示
功能徽章。未编译的功能路由直接 404。

**信号强度**：`/at-node/cmd/status` 带 `wifi_rssi`/`wifi_pct`（BLE 编译时另有
`ble_rssi`/`ble_pct`，取首个已连主机）；百分比 = `2×(rssi+100)` 截断 0-100
（-50dBm→100%，-100dBm→0%）。Status 页显示 dBm + 10 格彩条 + 百分比，
`AT+STATUS` 同样输出。

**呼吸灯（`FEATURE_BREATH_LED`，I2C 关闭时默认开）**：GPIO8 板载 LED 以 ~2s 周期
gamma 校正呼吸——呼吸=loop() 活着；定格/熄灭=死机。默认按 SuperMini C3 低电平点亮
（`BREATH_LED_ACTIVE_LOW 1`，高电平点亮的板子改 0）。

### rathole 隧道（`rathole_client.cpp`，架构/内存账目/坑录详见 [RATHOLE.md](RATHOLE.md)）

- 协议：rathole v1（bincode 定长消息），plain TCP transport；TLS/noise 未实现——**只穿透自带加密的协议**（SSH/HTTPS/MQTT-TLS），或把服务 bind 在 server 侧 `127.0.0.1`
- 架构：每隧道 1 个 manager task（控制通道 + 连接池轮询，3072B 栈）；服务器 TCP_POOL_SIZE=8，但每 standby socket 占 ~2.4KB heap，池缩为 **1 条/隧道**（服务器会按需重试 CreateDataChannel；实测池=2 时双隧道把 free_heap 压到 ~11KB，lwIP/HTTP 全饿死不响应）；访客到来才起转发 task（3072B 栈 + 1460B 缓冲）
- 并发纪律：t.cli/t.pool **仅 manager task 可触碰**；其他任务的 stop/改配置只置 `want_run`/`reconfig` 标志，manager 自己在 ~100ms 内收编重连（跨任务 `cli.stop()` 曾在握手中释放 RX buffer 导致 NULL 解引用 panic）；`last_error` 用定长 buffer，cfg 每次重连前快照
- 堆守护：`ESP.getFreeHeap() < 12KB` 时拒绝新建控制通道/池 socket/转发会话（`last_error="low heap, draining"`），等 TIME_WAIT(ESP-IDF 2×MSL≈120s）排空——实测重连风暴 25 轮可把 22K 打到 3.8K，全 IP 栈瘫痪 2-3 分钟；加守护后同场景 HTTP 全程可用、堆底 16K
- AT：`AT+TUNNEL=enable,<0|1>`（全局总开关，NVS）；`AT+TUNNEL=1,server|token|service|local|auto|retry|enable,<val>` / `connect|disconnect|clear|status`；`AT+TUNNEL=status` 汇总（含 free_heap）
- REST：`GET /at-node/cmd/tunnel/status`，`POST /at-node/cmd/tunnel/{config,enable,connect,disconnect,clear}`
- MQTT：`tunnel/{status,config,enable,connect,disconnect,clear}` 方法全通（见 sys/info API 目录）
- 配置项：全局 `rathole.enable` 一键总开关（关掉全停且不上电自连）；每隧道 `enable` 独立开关（持久化，关=立即停且禁止 connect）、`auto`（上电自连，需 enable=1）、`retry`（重连退避基数秒，1-60，指数×2 封顶 60s）。三级关系：`master && enable && auto → 上电自连`
- **RAM 共存**：MQTT TLS 握手需要 ~25KB+ 连续堆块。隧道 socket 会切碎堆，故启动时隧道等 MQTT 先连上（最多 30s）再建池；运行期若 MQTT 重连连续 5 次遇到 `SSL - Memory allocation failed`，设备自动重启整理堆（自愈）
- **适用场景**：长连接协议（SSH 等，实测 62s 12 往返稳定）。避免高频短连接爆发（每访客 2 条 socket，TIME_WAIT 驻留 ~60s）；WiFi 已关省电（`WiFi.setSleep(false)`），LAN 延迟 ~30ms 降到 ~2-30ms
- 测试陪练：`tools/test/rathole_server.test.toml`（本地 rathole server，两服务：c3http → 127.0.0.1:80，c3echo → TCP echo）
- 下一步：更多外设、Agent 工作流集成

## 安全策略

> **HTTP 仅应在可信的本地 NAT 网络内开启。**
> HTTP 控制面（`/at-node/*`）**无认证**，同网段任意设备都可调用写端点（注入按键、写 GPIO/I2C、清除绑定、改写凭据）。
> 因此它只适合部署在可信的本地 NAT 环境；接入不可信网络时，请用 `AT+HTTP=0` 关闭 HTTP
> （持久化到 NVS，重启保持），仅保留带 TLS 的 MQTT 控制面；需要时经串口 / AP portal / MQTT 重新开启。

## 快速开始

1. 拷贝 WiFi 配置：
   ```powershell
   Copy-Item wifi_config.h.example wifi_config.h
   # 编辑 wifi_config.h 填入 SSID 和密码
   ```

2. 编译/上传：
   ```powershell
   cd esp32/esp32_at_node
   .\build.ps1 -Port COM3
   ```

3. 与 CH582 dongle 验证 BLE 键盘：
   ```powershell
   cd tools
   .venv\Scripts\python test_dongle_c3.py --dongle-port COM4 --c3-ip 192.168.1.27
   ```

4. HTTP/AT 命令测试：
   ```powershell
   .venv\Scripts\python test_esp32_at_node.py --ip 192.168.1.27
   ```

5. 本地 MQTT broker 测试：
   ```powershell
   .venv\Scripts\python mqtt_broker.py   # 启动 broker
   # 另一个终端：
   .venv\Scripts\python test_esp32_at_node.py --ip 192.168.1.27
   ```
