# AT-Node Remote Broker 手册

> 单文件远程网关：`atnode_broker.py`。让 agent/用户从任何地方安全控制
> ESP32 AT-Node 设备（BLE 键盘、GPIO、WOL、ping 等）。
>
> **TL;DR**:
> ```bash
> uv run python tools/broker/atnode_broker.py deploy install  # 一键部署 systemd 服务
> uv run python tools/broker/atnode_broker.py manager key add --name my-agent  # 发 key
> uv run python tools/broker/atnode_broker.py client list --key <KEY>      # 用 key 访问
> uv run python tools/broker/atnode_broker.py deploy status   # 查看服务状态
> ```
>
> **四种角色**：`serve`(broker 服务端）/ `client`(MQTT 客户端，设备控制）/
> `manager`(key 管理 + 证书工具）/ `deploy`(systemd 服务管理）。

---

## 1. 架构

```
用户/Agent                          远程服务器                     局域网
─────────                          ──────────                    ──────
curl ──HTTPS──▶ HTTP proxy :8080 ─┐
   Bearer <api-key>              │  atnode_broker.py（单文件）
                                  │    ├── amqtt broker :1883/:8883(TLS)
ESP32 ──MQTT/TLS outbound────────▶│    └── paho 桥（设备注册表+RPC关联）
(NAT 穿透，设备主动外连)      atnode/<id>/{state,info,cmd,resp}
```

- ESP32 **主动外连** broker —— 无需公网 IP / 端口映射，天然穿 NAT。
- 所有设备能力统一为 **method 调用**（与设备本地 HTTP API 同名同参）。

## 2. 认证模型（API Key）

```
            ┌─────────────────────────────────────┐
  client ──▶│  ApiKeyAuthPlugin                   │
  (MQTT)    │   username = API key,须 active     │
            │   localhost 连接免 key              │
  ESP32  ──▶│  KeyStore: ~/.atnode_broker_keys.sqlite
  (MQTT)    │     key | name | status | created   │
            │  ManageAclPlugin                    │
  manager ─▶│   _manage/# topic 限 localhost      │
  (MQTT)    │  AccessLog: ~/.atnode_broker_logs/  │
            │   <key-name>.log / auth_fail.log    │
            └─────────────────────────────────────┘
```

- **远程 MQTT 连接**：用户名 = API key（须存在于 SQLite 且状态 active)。
- **本地连接（127.0.0.1）免 key**——manager 和本机 client 开箱即用；
  远程管理走 SSH 端口转发：`ssh -L 1883:127.0.0.1:1883 server`。
- **HTTP 代理**:`Authorization: Bearer <api-key>`；本地请求免认证。
- ESP32 也是普通客户端：`mqtt_user = <key>`（密码留空即可）。

## 3. Key 管理（manager key，走 MQTT，无需重启服务）

```bash
B=tools/broker/atnode_broker.py
uv run python $B manager key add --name esp32-home     # 生成 key（只打印一次）
uv run python $B manager key list                      # 全部 key + 状态
uv run python $B manager key revoke agent-alice        # 标记废弃（立即断认证）
uv run python $B manager key enable agent-alice        # 恢复
uv run python $B manager key remove agent-alice        # 彻底删除
# 接受 key 或名字定位；--server/--port 可指向 SSH 转发的端口
```

## 3b. 证书管理（manager certs，本地操作，无需 MQTT）

```bash
B=tools/broker/atnode_broker.py
uv run python $B manager certs gen --ip 1.2.3.4       # 一键生成 CA + 服务器证书
uv run python $B manager certs fingerprint             # SHA256 指纹（ESP32 配置用）
uv run python $B manager certs info                    # 证书详情（subject/有效期/SAN）
uv run python $B manager certs verify                  # 验证证书链完整性
# --certs DIR 指定证书目录（默认 tools/broker/certs/）
# --days N 指定有效期（默认 3650 天）
```

**访问日志**（纯文件，不入库）:`~/.atnode_broker_logs/`
- `<key-name>.log` — 该 key 的连接/HTTP 调用记录（时间、client_id、IP、操作）
- `auth_fail.log` — 被拒的尝试（含尝试的 key 前缀，方便识别泄漏）
- `local.log` — 本地免认证访问

## 3. 配置

本地文件：

| 文件 | 内容 |
|------|------|
| `~/.atnode_broker_keys.sqlite` | API key 数据库（manager 维护） |
| `~/.atnode_broker_logs/` | 访问日志（每 key 一个文件） |

serve 参数：

| 参数 | 默认 | 说明 |
|------|------|------|
| `--http [PORT]` | 不启动 | **HTTP 代理开关**；`--http`=8080,`--http 9000` 自定义；不带则不启动 |
| `--mqtt-port` | 1883 | MQTT 明文端口（LAN/localhost） |
| `--mqtt-tls-port` | 8883 | MQTT TLS 端口（远程，需证书） |
| `--certs` | `tools/broker/certs` | 含 `server.crt`/`server.key` 的目录；无证书则只开明文 |

## 4. HTTP API 参考

> **⚠️ 安全警告**：HTTP 代理为**明文传输（无 TLS）**，且可直接操作硬件（GPIO、键盘注入、WOL 等），
> 暴露公网极其危险。**默认不启动**。远程访问必须通过以下方式之一：
> - **nginx 反向代理 + HTTPS**（推荐生产部署）
> - **SSH 端口转发**：`ssh -L 8080:127.0.0.1:8080 Server`，然后访问 `localhost:8080`
>
> 绝对不要将 8080 端口直接暴露到公网安全组。

Base: `http://<server>:8080`。除 `/api/help` 外全部需要
header `Authorization: Bearer <api-key>`（localhost 免）。全部返回 JSON。

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
           "services":{"keyboard/tap":{"d":"press+release one key",
             "p":{"mods":"modifier mask","k":"HID keycode","ms":"hold ms"}},
             "...":"17 services with full param descriptions"}},
  "usage": "client call <device> <method> k=v ..."
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
KEY=...; S=http://server:8080; D=atnodeesp-5688
AUTH="Authorization: Bearer $KEY"

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
# 本地（localhost 免 key，开箱即用）
uv run python tools/broker/atnode_broker.py client list
uv run python tools/broker/atnode_broker.py client info atnodeesp-5688
uv run python tools/broker/atnode_broker.py client call atnodeesp-5688 keyboard/text s=Hello ms=60
uv run python tools/broker/atnode_broker.py client wol  atnodeesp-5688 AA:BB:CC:DD:EE:FF
uv run python tools/broker/atnode_broker.py client ping atnodeesp-5688 192.168.1.1 4

# 远程 / TLS（需要 API key）
uv run python tools/broker/atnode_broker.py client list \
  --server broker.example.com --port 8883 --ca ca.crt --key <API_KEY>
```

### 配置文件 `client.toml`（免重复传参）

将连接信息保存到 `tools/broker/client.toml`（已 gitignore），之后所有 client 命令自动读取：

```toml
[client]
server = "122.51.226.5"
port = 8883
fp = "A7:0E:E1:74:..."    # 服务器证书 SHA256 指纹（manager certs fingerprint）；推荐
# ca = "certs/ca.crt"      # 备选：CA 验证；设置了 fp 时被忽略
key = "your-api-key"
```

```bash
cp tools/broker/client.toml.example tools/broker/client.toml   # 首次: 复制模板并填入真实值
uv run python tools/broker/atnode_broker.py client list   # 无需任何额外参数
```

优先级：CLI 参数 > `$ATNODE_KEY` 环境变量 > `client.toml` > 默认值。

| 参数 | 默认 | 说明 |
|------|------|------|
| `--server` | client.toml / 127.0.0.1 | broker 地址 |
| `--port` | client.toml / 1883 | broker 端口（8883 自动启用 TLS） |
| `--fp` | client.toml / — | 服务器证书 SHA256 指纹（pin，推荐；设置后忽略 `--ca`） |
| `--ca` | client.toml / — | CA 证书（备选；无 `fp`/`ca` 时 8883 不校验证书，不安全） |
| `--key` | client.toml / `$ATNODE_KEY` | API key（manager 发放） |

## 7b. 任何 MQTT 客户端都能直接操作设备

命令通道是纯 MQTT 约定，不绑定本脚本：

```bash
# 发命令（格式：<reqid> <method> <query>）
mosquitto_pub -h server -u <API_KEY> \
  -t atnode/atnodeesp-5688/cmd -m "r1 net/ping host=192.168.1.1 count=4"
# 收响应（按 reqid 关联）
mosquitto_sub -h server -u <API_KEY> -t atnode/atnodeesp-5688/resp
# 设备发现（retained）
mosquitto_sub -h server -u <API_KEY> -t 'atnode/+/state'
mosquitto_sub -h server -u <API_KEY> -t 'atnode/+/info'
```

## 8. ESP32 侧配置

| 方式 | 命令 |
|------|------|
| HTTP | `POST /at-node/cmd/mqtt/config?broker=<ip>&port=<p>&user=<api-key>` 然后 `POST /at-node/cmd/mqtt/connect` |
| 串口 AT | `AT+MQTT=broker,<ip>` / `AT+CONF=mqtt_user=<api-key>` / `AT+MQTT=port,<p>` / `AT+MQTT=connect` |
| 清除配置 | `AT+MQTT=clear` — 清空全部 MQTT 设置（NVS+运行时）并断开，同时停止自动重连 |

- 参数持久化在 NVS，重启后 `AT+MQTT=connect` 重连即可。
- **TLS(8883)**：设备仅验证服务器证书 SHA256 指纹（无嵌入式 CA/PEM）
  （`AT+MQTT=ca,<fingerprint>`；查指纹：`manager certs fingerprint`）。
- 设备上线后 broker 立即可见（retained `state=online` + `info` 清单）；
  掉线由 LWT 置 `offline`。
- **自动重连**：首次手动 `AT+MQTT=connect` 成功后自动启用 `mqtt_auto`，
  重启后每 10s 重试直至连上；`AT+MQTT=auto,0` 可关闭，`AT+MQTT=clear` 重置。
  重连成功会重新发布 state/info，broker 重启后注册表自愈。

## 9. 远程部署清单

> **安全原则**：证书在服务器上生成，私钥永远不离开服务器。
> 项目 `.gitignore` 已排除 `tools/broker/certs/` 和 `esp32/esp32_at_node/certs/`。

### 9.1 服务器初始化

```bash
# 1. 克隆仓库（项目不大，直接全量）
git clone https://github.com/rede97/at-node.git ~/at-node
cd ~/at-node

# 2. 生成证书（CA + 服务器证书，SAN 填服务器公网 IP）
uv run python tools/broker/atnode_broker.py manager certs gen --ip <SERVER_IP>
chmod 600 tools/broker/certs/*.key

# 3. 用 uv 创建 Python 环境并安装全部依赖（含 broker）
uv sync --all-packages   # 国内服务器已配置阿里云镜像
```

### 9.2 启动服务

```bash
B="uv run python tools/broker/atnode_broker.py"

# 手动测试（前台运行，Ctrl+C 停止）
$B serve --certs tools/broker/certs --http

# systemd 用户服务（推荐，开机自启 + 崩溃自重启）
$B deploy install                    # 使用已有 certs/，仅 MQTT（HTTP 默认关闭）
$B deploy install --gen-certs --ip <SERVER_IP>  # 首次部署同时生成证书
$B deploy install --http             # 同时启用 HTTP 代理（仅限本地/隧道访问）
$B deploy install --http --http-port 9090       # 自定义 HTTP 端口

# 日常运维
$B deploy status                     # 服务状态 + 证书指纹
$B deploy restart                    # 重启
$B deploy logs                       # 实时日志 (journalctl -f)
$B deploy stop | start | enable | disable
$B deploy uninstall                  # 完全移除服务
```

> `deploy install` 幂等：重复执行只更新 unit 文件并 restart。
> 自动执行 `loginctl enable-linger` 确保服务器重启后服务自启；若提示权限不足，需手动执行 `sudo loginctl enable-linger $USER`。

### 9.3 防火墙 / 安全组

- **云安全组**（腾讯云/阿里云控制台）：入站仅需放行 TCP **8883**（MQTT-TLS）。
- **HTTP 8080 禁止对外暴露**——明文传输 + 直接硬件操作，必须通过 nginx HTTPS 反代或 SSH 隧道访问。
- 1883 仅绑 localhost/LAN，不对外暴露。
- **本机测试注意**：Windows 防火墙会拦 LAN 入站 1883，需添加入站规则或临时关闭。

### 9.4 生成 API Key

```bash
uv run python tools/broker/atnode_broker.py manager key add --name esp32-home
# 输出 key（只展示一次，妥善保存）
```

### 9.5 ESP32 配置

```bash
# 获取服务器证书 SHA256 指纹（在服务器上执行）
openssl x509 -in tools/broker/certs/server.crt -noout -fingerprint -sha256

# ESP32 侧（HTTP 或串口 AT）
AT+MQTT=broker,<SERVER_IP>
AT+MQTT=port,8883
AT+CONF=mqtt_user=<API_KEY>
AT+MQTT=ca,<SHA256_FINGERPRINT>
AT+MQTT=connect
```

设备上线后 `client list` 即可看到（retained state=online）。

## 10. 排障

| 症状 | 原因/处理 |
|------|----------|
| ESP32 connect 后 HTTP/串口卡死 | 固件旧版 mqtt_connect() 阻塞主循环；升级到最新版（MQTT 已移至独立 task） |
| ESP32 connect 卡住无响应 | 端口被其他 broker 占用（杀残留 `mqtt_broker.py` 进程）；或防火墙拦 LAN 入站 |
| `mqtt_port` 配置不生效 | 曾有的 NVS 类型 bug 已修；确认固件为最新 |
| `client list` 空 | 设备未连上（`AT+MQTT=status` 查）；设备带自动重连，broker 重启后 ~10s 内会自动恢复注册 |
| RPC timeout | 设备离线 / MQTT 断开；`GET /api/devices/<id>` 看 `online` |
| keyboard 返回 BLE not connected | 设备 BLE 未连主机，先在主机侧连接（或用 `/at-node/pair` 页面管理） |
| TLS 连接失败 | 指纹/CA 不匹配：`AT+MQTT=ca,status` 查当前验证方式 |
