# Broker 快速部署 & 本地 Client 配置 Runbook

> Agent 可执行的操作手册。按顺序执行即可从零拉起远程 broker + 本地 client 环境。

## 前置条件

| 项目 | 要求 |
|------|------|
| 本地 | Windows, Python 3.11+, `paho-mqtt` (`pip install paho-mqtt`) |
| 服务器 | Linux + systemd, `uv`, SSH 别名 `Server` 可达 |
| 网络 | 云安全组入站放行 TCP **8883**（MQTT-TLS）；**禁止**放行 8080 |

## 1. 服务器部署（首次 / 重建）

```bash
# 同步代码
ssh Server "cd ~/at-node && git pull --ff-only"

# 一键部署（生成 unit + enable linger + start）
ssh Server "bash -lc 'cd ~/at-node/tools/broker && uv run python atnode_broker.py deploy install'"
```

**注意：**
- `deploy install` 幂等——重复执行只更新 unit 并 restart
- HTTP 代理**默认关闭**（明文 + 直接硬件操作，极其危险）
- 如需 HTTP，仅限 `--http` 显式开启 + nginx/SSH 隧道访问

### 1.1 证书策略（三选一）

| 场景 | 命令 |
|------|------|
| 使用已有 `certs/` | `deploy install`（默认） |
| 重新生成 CA + 服务器证书 | `deploy install --gen-certs --ip <PUBLIC_IP>` |
| 指定外部证书目录 | `deploy install --certs /path/to/certs` |

> ⚠️ **重新生成证书后**，所有已连接设备的 CA 指纹会失效，需更新（见 §4）。

### 1.2 验证服务

```bash
ssh Server "bash -lc 'cd ~/at-node/tools/broker && uv run python atnode_broker.py deploy status'"
```

期望输出：`active (running)` + `enabled` + 端口 1883/8883 监听 + SHA256 指纹。

## 2. 本地 Client 配置

### 2.1 创建 `tools/broker/client.toml`（已 gitignore）

```toml
[client]
server = "122.51.226.5"
port = 8883
ca = "certs/ca.crt"       # 相对路径，基于脚本目录
key = "<API_KEY>"          # manager key add 发放
```

### 2.2 同步 CA 证书到本地

```powershell
scp Server:~/at-node/tools/broker/certs/ca.crt tools\broker\certs\ca.crt
```

> 证书重新生成后必须重新拉取，否则 TLS 验证失败。

### 2.3 验证连接

```powershell
cd tools\broker
python atnode_broker.py client list
```

期望：`[bridge] connected to broker` + 设备列表。

**优先级**：CLI 参数 > `$ATNODE_KEY` 环境变量 > `client.toml` > 默认值(127.0.0.1:1883)。

## 3. 常用 Client 命令

```bash
python atnode_broker.py client list                          # 在线设备
python atnode_broker.py client info atnodeesp-5688           # 设备详情
python atnode_broker.py client call atnodeesp-5688 sys/info  # 远程 RPC
python atnode_broker.py client call atnodeesp-5688 keyboard/text s=Hello
python atnode_broker.py client wol atnodeesp-5688 AA:BB:CC:DD:EE:FF
python atnode_broker.py client ping atnodeesp-5688 192.168.1.1
```

所有命令自动读取 `client.toml`，无需重复指定 `--server/--port/--key`。

## 4. ESP32 设备指纹更新

证书重建后 ESP32 的 `mqtt/status` 会显示 `connected: false`。修复：

```powershell
# 1. 获取服务器当前指纹
ssh Server "openssl x509 -in ~/at-node/tools/broker/certs/server.crt -noout -fingerprint -sha256"
# 输出: sha256 Fingerprint=A7:0E:E1:74:...

# 2. 推送到 ESP32（HTTP 本地接口）
Invoke-RestMethod -Uri "http://192.168.1.27/at-node/cmd/mqtt/ca?fp=A7:0E:E1:74:..." -Method POST

# 3. 触发重连
Invoke-RestMethod -Uri "http://192.168.1.27/at-node/cmd/mqtt/connect" -Method POST

# 4. 确认
Invoke-RestMethod -Uri "http://192.168.1.27/at-node/cmd/mqtt/status"
# 期望: connected=true, ca_fp 与新指纹一致
```

## 5. 故障排查

| 症状 | 原因 | 修复 |
|------|------|------|
| `ConnectionRefusedError` (8883) | 服务未启动 / 正在重启 | `ssh Server "systemctl --user status atnode-broker"` |
| `CERTIFICATE_VERIFY_FAILED` | 本地 CA 与服务器不匹配 | 重新 `scp` CA（§2.2） |
| `IP address mismatch` | 通过隧道连接 / 证书 SAN 是 IP | 代码已修复（`tls_insecure_set`） |
| `ModuleNotFoundError: amqtt` | 本地无 broker 依赖 | 已修复（lazy import），确保代码最新 |
| ESP32 `connected: false` | CA 指纹过期 | 更新指纹（§4） |
| 服务反复重启 | 短时间内多次 `deploy restart` | 等待 10s 稳定，或 `stop` + `start` |
| `client list` → `no devices` | ESP32 未通电 / MQTT 断开 | 检查 ESP32 HTTP `/mqtt/status` |

## 6. 运维速查

```bash
# 日常运维（服务器上）
uv run python atnode_broker.py deploy status|start|stop|restart|logs

# 远程一键同步+重启（本地）
ssh Server "cd ~/at-node && git pull --ff-only && bash -lc 'cd tools/broker && uv run python atnode_broker.py deploy restart'"

# API Key 管理
uv run python atnode_broker.py manager key add --name <name>
uv run python atnode_broker.py manager key list

# SSH 隧道访问 HTTP（如需调试）
ssh -f -N -L 8080:127.0.0.1:8080 Server
# 然后本地 curl http://localhost:8080/api/help
```

## 关键安全规则

1. **HTTP 8080 禁止暴露公网**——明文 + 可直接操作硬件
2. **云安全组仅开放 8883**（MQTT-TLS）
3. **`client.toml` 含 API Key**——已 gitignore，绝不提交
4. **证书私钥** (`certs/`)——已 gitignore，绝不提交
