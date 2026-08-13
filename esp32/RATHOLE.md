# rathole 隧道客户端(ESP32-C3)— 架构与实战记录

`esp32_at_node/rathole_client.cpp` 实现 rathole 协议 v1 客户端,把设备上的
TCP 服务(SSH 等)反向穿透到公网 rathole server。**单隧道**(id 恒为 1,
一条 SSH 即可跳板,降低公网暴露与 RAM 占用)。

## 1. 协议裁剪

对齐 `rathole/src/protocol.rs` + `client.rs`,bincode 小端定长:

```
Hello::ControlChannelHello = u32(0) + u8(version=1) + sha256(service)[32]   (37B)
Hello::DataChannelHello    = u32(1) + u8(version=1) + session_key[32]       (37B)
Auth                       = sha256(token || server_nonce)[32]              (32B)
Ack                        = u32: 0=Ok 1=ServiceNotExist 2=AuthFailed       (4B)
ControlChannelCmd          = u32: 0=CreateDataChannel 1=HeartBeat           (4B)
DataChannelCmd             = u32: 0=StartForwardTcp 1=StartForwardUdp       (4B)
```

裁剪:**plain TCP transport only**(无 TLS/noise —— 只穿透自带加密的协议,
或把服务 bind 在 server 侧 127.0.0.1)、TCP only(无 UDP)、无热加载、无代理。

## 2. 任务模型与并发纪律

```
manager task (常驻, 3072B 栈)          forward task (每访客一个, 3072B 栈)
├─ 控制通道 t.cli ───── 唯一属主        ├─ remote = 池中被激活的 socket
├─ 待机池 t.pool[1] ─── 唯一属主        ├─ local  = cfg.local 的新连接 (setNoDelay)
├─ CreateDataChannel → 补池            └─ 双向泵 1460B, 会话结束即销毁
└─ StartForwardTcp   → 移交 forward task
```

**铁律:`t.cli` / `t.pool` 只允许 manager task 触碰。** 其他任务(AT/HTTP/MQTT
上下文)改配置或启停,只置两个标志位:

| 标志 | 语义 | 生效延迟 |
|---|---|---|
| `want_run` | 期望运行状态(start/stop/master switch) | ≤100ms(切片退避轮询) |
| `reconfig` | 配置已变,拆线用新配置重连(多字段合并为一次) | ≤100ms |

manager 每轮重连前**快照** `t.cfg`(`TunnelCfg cfg = t.cfg`),快照用于握手与
`start_forward`;`last_error` 是 `char[64]` 定长 buffer(`set_err()` 写入)。
这两处曾是 String 跨任务读写 —— 对端写 String 会 free 旧 buffer,读者踩悬空指针。

开关层级:`master(rathole.enable) && enable && auto → 上电自连`;
`master`/`enable` 关掉立即停且禁止 connect。

## 3. 内存账目(ESP32-C3, ~320KB SRAM)

| 项 | 成本 | 备注 |
|---|---|---|
| 每条隧道 | ~5KB | 控制 socket 2.4K + 池 socket 2.4K + 栈/碎片 |
| 每个转发会话 | ~7KB | socket 2.4K + 任务栈 3K + 泵缓冲 1.5K |
| MQTT TLS 握手 | ~25KB **连续块**,瞬时 | 失败 5 次自愈重启(见 R5) |
| lwIP TIME_WAIT | ~1-2KB/sock,**驻留 ~120s**(2×MSL) | 重连风暴的隐形杀手,见 R4 |

稳态基线:WiFi+BLE+MQTT+HTTP+单隧道 ≈ free_heap 20-23KB。

**堆守护**:`ESP.getFreeHeap() < 12000` 时 manager 拒绝新建控制通道/池 socket,
`start_forward` 拒绝新访客(server 会重试),`last_error="low heap, draining"`,
等 TIME_WAIT 排空再恢复。

## 4. 坑录(全部实机复现 + addr2line/测量取证)

### R1 双隧道直接把堆打死 → "卡死"
双隧道全开 free_heap 仅 **10.9KB**;lwIP 分不出 pbuf,HTTP 页 15s 超时 0 字节,
ICMP 不应答,串口 AT 却正常(不吃网络堆)——表象"设备死了"。
**决策**:砍单隧道(`RATHOLE_MAX_TUNNELS=1`),池 2→1 条,任务栈 4096→3072。

### R2 HTML 页整页拷堆 → 低堆时页面必挂
`send_html(const String&)` 把 ~14KB 页面从 flash 拷进堆 String 再发;heap 13.6K
时实测 8.6s 只收到半截(curl code 18)。修复:静态页改 `send_P` 流式发送;
随后更进一步:整个 Web UI 改为 gzip 单页应用(`esp32/web/` → `web_page.h`,
15.2KB→4.5KB,一次响应零堆拷贝,页面内全走 JSON API)。

### R3 跨任务 `cli.stop()` → 空指针 panic(最恶性)
复现:POST 改隧道配置 → 立即 `Guru Meditation: Load access fault, MTVAL=0x14`。
addr2line 调用链:
```
manager_task → read_exact (rathole_client.cpp:96)
→ NetworkClient::available() (NetworkClient.cpp:558)
→ NetworkClientRxBuffer::failed() — _rxBuffer == NULL
```
`rathole_stop()` 在 manager 握手阻塞途中 `t.cli.stop()`,RX buffer 在对方脚下
被释放。崩溃发生在第一字段写 NVS 之后 → 重启后配置残缺(新 server + 旧
service),接着无限 "service not exist"。修复:见 §2 并发纪律。

### R4 重连风暴 → TIME_WAIT 堆积 → 全 IP 栈瘫痪 2-3 分钟(周期性卡顿主因)
心跳超时(45s)/WAN 抖动 → 反复重连,每轮烧几 KB socket;ESP-IDF TIME_WAIT
驻留 2×MSL≈120s。压力测试(25 轮改配置+启停,25s 打完):22K → **3.8K**,
期间 HTTP/ICMP/出站全灭,~175s 后自行恢复 —— 与用户感受的"隔一阵子卡死、
过几分钟自己好"完全吻合。修复:§3 堆守护。复测同场景:HTTP 全程 200,
堆底 16K,6.5s 打完。

### R5 MQTT TLS 握手需要 ~25KB 连续块
socket 碎片化后握手报 `SSL - Memory allocation failed`;启动时隧道等 MQTT 先连
(最多 30s);运行期连续 5 次失败 → `schedule_restart()` 自愈重启。属于已知
设计,堆守护(R4)同时缓解了触发频率。

### R6 转发本地侧未关 Nagle → SSH 击键 ~40ms 抖动
`forward_task` 的 `local` socket 补 `setNoDelay(true)`(remote 侧池 socket 建池
时已开)。SSH 回显是大量小写,Nagle 会攒包。

## 5. 本地验证方法

```bash
# 本地 rathole server(c3http → 设备 HTTP,c3echo → TCP echo)
rathole --server tools/test/rathole_server.test.toml   # bind 0.0.0.0:2333

# 设备改指本地(隧道运行中改配置 = R3 的触发路径)
curl -X POST http://<dev>/at-node/cmd/tunnel/config \
  -d "id=1&server=<pc-ip>:2333&service=c3http&token=tok_http_secret&local=127.0.0.1:80"

# 访客端到端
curl http://127.0.0.1:5202/at-node/cmd/status

# 压力(R4 场景):循环交替 repoint + connect/disconnect + enable 0/1,
# 同时持续 curl /at-node/cmd/tunnel/status 观察 free_heap 底与 HTTP 存活
```

辅助工具:`tools/test/serial_tty.py`(串口 REPL,留后台看 [rathole] 日志)、
`tools/test/ping_watch.py`(ICMP 时延记录;注意设备在高负载时 ICMP 最先丢,
不代表 TCP 死)。

## 6. 已知边界

- BLE+WiFi 单射频共存:BLE 主机连着键盘时,射频分时给 WiFi 带来周期性抖动,
  属硬件约束,软件侧已 `WiFi.setSleep(false)` 到底。
- 服务器心跳 30s/超时 40s,客户端 45s 判死;控制通道断开**不影响**已建立的
  转发会话,只影响新访客。
- 公网 server 端口会收到无关 rathole 客户端的握手试探(日志
  "No such a service <sha256>" 刷屏),在服务名检查阶段即拒绝,无害。
