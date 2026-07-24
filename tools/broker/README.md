# AT-Node Remote Broker 手册

> 单文件远程网关：`atnode_broker.py`。让 agent/用户从任何地方通过
> `curl` 安全控制 ESP32 AT-Node 设备（BLE 键盘、GPIO、WOL、ping 等）。
>
> **TL;DR**:
> ```bash
> uv run python tools/broker/atnode_broker.py serve --http   # MQTT broker + HTTP 代理
> uv run python tools/broker/atnode_broker.py client list    # 看设备
> curl -H "Authorization: Bearer $TOKEN" \
>   -X POST "http://SERVER:8080/api/devices/<id>/cmd/keyboard/text?s=Hello"
> ```
>
> **注意**:`serve` 默认**只启动 MQTT broker**；HTTP 代理需显式 `--http [PORT]`。

---

## 1. 架构

```
用户/Agent                          远程服务器                     局域网
─────────                          ──────────                    ──────
curl ──HTTPS──▶ HTTP proxy :8080 ─┐
      Bearer token                │  atnode_broker.py（单文件）
                                  │    ├── amqtt broker :1883/:8883(TLS)
ESP32 ──MQTT/TLS outbound────────▶│    └── paho 桥（设备注册表+RPC关联）
(NAT 穿透，设备主动外连)      atnode/<id>/{state,info,cmd,resp}
```

- ESP32 **主动外连** broker —— 无需公网 IP / 端口映射，天然穿 NAT。
- 所有设备能力统一为 **method 调用**（与设备本地 HTTP API 同名同参）。

## 2. 快速开始（本地 3 步）

```bash
# 1. 启动（首次运行生成 ~/.atnode_broker.json：token + MQTT 账号密码，并打印）
#    --http 才会启动 HTTP 代理（默认 8080）；不带则纯 MQTT broker
uv run python tools/broker/atnode_broker.py serve --http

# 2. 把 ESP32 指到 broker（设备当前用哪个账号密码见配置文件打印）
curl -X POST "http://192.168.1.27/at-node/cmd/mqtt/config?broker=<broker-ip>&port=1883&user=<u>&pass=<p>"
curl -X POST "http://192.168.1.27/at-node/cmd/mqtt/connect"

# 3. 通过代理控制设备
TOKEN=$(jq -r .token ~/.atnode_broker.json)
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8080/api/devices
```

## 3. 配置

配置文件 `~/.atnode_broker.json`（首次 serve 自动生成，权限 600):

```json
{"token": "<http-bearer>", "mqtt_user": "atnode", "mqtt_password": "<mqtt-pass>"}
```

serve 参数：

| 参数 | 默认 | 说明 |
|------|------|------|
| `--http [PORT]` | 不启动 | **HTTP 代理开关**；`--http`=8080,`--http 9000` 自定义；不带则不启动 |
| `--mqtt-port` | 1883 | MQTT 明文端口（LAN/localhost） |
| `--mqtt-tls-port` | 8883 | MQTT TLS 端口（远程，需证书） |
| `--certs` | `tools/broker/certs` | 含 `server.crt`/`server.key` 的目录；无证书则只开明文 |
| `--token` | 配置文件值 | 覆盖 HTTP bearer token |

## 4. HTTP API 参考

Base: `http://<server>:8080`。除 `/api/help` 外全部需要
header `Authorization: Bearer <token>`。全部返回 JSON。

| Method | Path | 说明 | 响应 |
|--------|------|------|------|
| GET | `/api/help` | 内置简版文档（无需认证） | text |
| GET | `/api/devices` | 设备列表+在线状态 | `{"devices":[{"id","online","info","last_seen"}]}` |
| GET | `/api/devices/<id>` | 设备详情+服务目录+调用示例 | 见下 |
| POST | `/api/devices/<id>/cmd/<method>` | 调用设备 method | 设备响应原样返回 |

**POST 参数**：query string、form body 或 JSON body 均可（合并）。
**超时**：设备 10s 未响应返回 `{"ok":false,"error":"timeout..."}`。
**错误码**:401 未认证 / 404 未知设备或路径 / 200 业务结果看 `ok` 字段。

`/api/devices/<id>` 响应示例：

```json
{
  "id": "atnodeesp-5688",
  "online": true,
  "info": {"device":"AT-Node-ESP-5688","ip":"192.168.1.27",
           "ble_addr":"88:56:a6:7b:c8:42","ble_connected":true,
           "services":["keyboard/tap","..."]},
  "services": ["keyboard/tap","keyboard/text","...","net/wol","net/ping","sys/info"],
  "usage": {"call":"POST /api/devices/atnodeesp-5688/cmd/<method>?k=v&...",
            "examples":["..."]}
}
```

## 5. 设备 method 目录（cmd 可调用）

> 在线目录以 `GET /api/devices/<id>` 的 `services` 字段为准。

| method | 参数 | 说明 | 响应要点 |
|--------|------|------|---------|
| `keyboard/tap` | `mods,k,ms` | 单键按下+释放。k 支持 `0x` 十六进制 | `{"ok":true}` |
| `keyboard/text` | `s,ms,gap` | 打 ASCII 文本（URL 编码） | `{"ok":true,"queued":true}` |
| `keyboard/key` | `mods,k0..k5` | 原始 HID 报告状态（0=释放） | `{"ok":true}` |
| `gpio/write` | `pin,level` | 数字输出 | |
| `gpio/read` | `pin` | 数字读 | `{"level":0|1}` |
| `adc/read` | `ch` | ADC | `{"mv":1234}` |
| `ble/status` | — | BLE 名称/地址/连接/绑定列表 | `{"ble":{...}}` |
| `net/wol` | `mac` | 在设备所在 LAN 发 WOL 魔包 | `{"ok":true}` |
| `net/ping` | `host,count` | 从设备 LAN ICMP ping | `{"ip","recv","avg_ms"}` |
| `sys/info` | — | 设备清单（同 info 字段） | `{"info":{...}}` |

modifiers 位掩码：bit0 LCtrl, bit1 LShift, bit2 LAlt, bit3 LGui, bit4-7 右侧同序。
键码：USB HID usage ID（'a'=0x04, Enter=0x28, F13=0x68, CapsLock=0x39）。

**前置条件**：所有 `keyboard/*` 需要设备 BLE 已连接到主机
（`ble/status` 看 `connected`)；否则返回 409 语义的 `{"ok":false,"error":"BLE not connected"}`。

## 6. Agent 常用食谱

```bash
TOKEN=...; S=http://server:8080; D=atnodeesp-5688
AUTH="Authorization: Bearer $TOKEN"

# 设备在线？BLE 键盘连着吗？
curl -s -H "$AUTH" $S/api/devices/$D | jq '{online, ble:.info.ble_connected}'

# 远程打字（文本需 URL 编码）
curl -s -H "$AUTH" -X POST "$S/api/devices/$D/cmd/keyboard/text?s=Hello%20World&ms=60&gap=100"

# 远程按 Enter
curl -s -H "$AUTH" -X POST "$S/api/devices/$D/cmd/keyboard/tap?mods=0&k=0x28&ms=50"

# Ctrl+Alt+Del（mods=LCtrl|LAlt=0x05, k=0x4C Delete）
curl -s -H "$AUTH" -X POST "$S/api/devices/$D/cmd/keyboard/tap?mods=5&k=0x4C&ms=100"

# 唤醒家里 PC
curl -s -H "$AUTH" -X POST "$S/api/devices/$D/cmd/net/wol?mac=AA:BB:CC:DD:EE:FF"

# 检查家里网络/主机存活
curl -s -H "$AUTH" -X POST "$S/api/devices/$D/cmd/net/ping?host=192.168.1.1&count=4"
```

## 7. Client CLI（纯 MQTT 直连，无需 HTTP 代理）

client 与设备是**同一协议的不同角色**：都是 broker 的 MQTT 客户端。
因此 client 不依赖 HTTP 代理，可指向**任意可达 broker**（本地/远程/TLS)。

```bash
# 本地（凭据自动读 ~/.atnode_broker.json）
uv run python tools/broker/atnode_broker.py client list
uv run python tools/broker/atnode_broker.py client info atnodeesp-5688
uv run python tools/broker/atnode_broker.py client call atnodeesp-5688 keyboard/text s=Hello ms=60
uv run python tools/broker/atnode_broker.py client wol  atnodeesp-5688 AA:BB:CC:DD:EE:FF
uv run python tools/broker/atnode_broker.py client ping atnodeesp-5688 192.168.1.1 4

# 远程 / TLS
uv run python tools/broker/atnode_broker.py client list \
  --server broker.example.com --port 8883 --ca ca.crt --user atnode --pass xxx
```

| 参数 | 默认 | 说明 |
|------|------|------|
| `--server` | 127.0.0.1 | broker 地址 |
| `--port` | 1883 | broker 端口（8883 自动启用 TLS） |
| `--ca` | — | CA 证书（存在则严格校验；8883 无 CA 则跳过校验） |
| `--user/--pass` | 配置文件 | MQTT 凭据 |

## 7b. 任何 MQTT 客户端都能直接操作设备

命令通道是纯 MQTT 约定，不绑定本脚本：

```bash
# 发命令（格式：<reqid> <method> <query>）
mosquitto_pub -h server -u atnode -P xxx \
  -t atnode/atnodeesp-5688/cmd -m "r1 net/ping host=192.168.1.1 count=4"
# 收响应（按 reqid 关联）
mosquitto_sub -h server -u atnode -P xxx -t atnode/atnodeesp-5688/resp
# 设备发现（retained）
mosquitto_sub -h server -u atnode -P xxx -t 'atnode/+/state'
mosquitto_sub -h server -u atnode -P xxx -t 'atnode/+/info'
```

## 8. ESP32 侧配置

| 方式 | 命令 |
|------|------|
| HTTP | `POST /at-node/cmd/mqtt/config?broker=<ip>&port=<p>&user=<u>&pass=<pw>` 然后 `POST /at-node/cmd/mqtt/connect` |
| 串口 AT | `AT+MQTT=broker,<ip>` / `AT+CONF=mqtt_user=<u>` / `AT+CONF=mqtt_pass=<pw>` / `AT+MQTT=port,<p>` / `AT+MQTT=connect,x` |

- 参数持久化在 NVS，重启后 `AT+MQTT=connect` 重连即可。
- **TLS(8883)**：设备验证方式二选一——CA 证书或服务器证书 SHA256 指纹
  （`AT+MQTT=ca,<fingerprint>`；查指纹：`openssl x509 -in server.crt -noout -fingerprint -sha256`）。
- 设备上线后 broker 立即可见（retained `state=online` + `info` 清单）；
  掉线由 LWT 置 `offline`。
- **自动重连**：配置了 broker 且 WiFi 在线时，设备每 10s 重试直至连上；
  重连成功会重新发布 state/info，broker 重启后注册表自愈。

## 9. 远程部署清单

1. `scp tools/broker/atnode_broker.py tools/broker/certs user@server:`（或 git clone)。
2. 服务器：`pip install amqtt paho-mqtt`（或 uv 环境）。
3. **只暴露 8883(MQTT-TLS)**；1883 绑 LAN/localhost 即可。
4. 需要 HTTP 代理时：`serve --http`，8080 建议套 nginx/Caddy 上 HTTPS；bearer token 即认证。
   纯转发场景也可以只跑 MQTT broker（不带 `--http`），由你自己的服务直连 MQTT。
5. ESP32 改指 `broker=<server-ip>` `port=8883` + CA 指纹（见 §8)。
6. 防火墙：服务器放行 8883/443；**本机测试遇过 Windows 防火墙拦 LAN 入站**。

## 10. 排障

| 症状 | 原因/处理 |
|------|----------|
| ESP32 connect 卡住无响应 | 端口被其他 broker 占用（杀残留 `mqtt_broker.py` 进程）；或防火墙拦 LAN 入站 |
| `mqtt_port` 配置不生效 | 曾有的 NVS 类型 bug 已修；确认固件为最新 |
| `client list` 空 | 设备未连上（`AT+MQTT=status` 查）；设备带自动重连，broker 重启后 ~10s 内会自动恢复注册 |
| RPC timeout | 设备离线 / MQTT 断开；`GET /api/devices/<id>` 看 `online` |
| keyboard 返回 BLE not connected | 设备 BLE 未连主机，先在主机侧连接（或用 `/at-node/pair` 页面管理） |
| TLS 连接失败 | 指纹/CA 不匹配：`AT+MQTT=ca,status` 查当前验证方式 |
