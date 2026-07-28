# tools/ 目录说明

| 目录 | 性质 | 内容 |
|------|------|------|
| `broker/` | **应用组成部分** | `atnode_broker.py` — 远程 broker（嵌入式 MQTT broker + HTTP 代理 + client CLI，可部署到服务器），部署/客户端快速上手见 `tools/broker/GET_START.md`;`mqtt_broker.py` — 本地开发用轻量 MQTT broker;`certs/` — CA/服务器证书 |
| `test/` | 测试脚本 | AT 回归、dongle 闭环/加固、ESP32 功能测试、`send_key.py`、`at_cli.py`;蓝牙主机侧工具:`bt_host.py`(扫描/配对/监听/断言)、`bt_agent.py`(BlueZ Just-Works 配对 agent);验收:`test_dev_switch_accept.py`(多模切换)、`test_multi.py`(多槽矩阵) |
| `demo/` | demo / 侦察 sketch | `c3_demo_nimble`(NimBLE 最小键盘基准）、`c3_demo_tvk`(Bluedroid 对照）、`esp32c3_kbd`（键盘 bench)、`esp32c3_probe`、`rk_recon` |
| `utils/` | 工程工具 | `batch_utf8.py`(GB2312→UTF-8 编码迁移/检查） |
| `ci/` | 构建/烧录 | 工具链说明、全变体构建、wlink/ISP 烧录、一键闭环测试 |

所有 Python 脚本统一 `uv run python tools/<dir>/<name>.py` 运行。
