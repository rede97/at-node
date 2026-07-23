# SKILL-Windows.md — ESP32-C3 开发踩坑要点（Windows 环境）

> 面向本仓库的 ESP32-C3 参考主机开发（tools/esp32c3_probe、tools/rk_recon）。
> 全是实测踩过的坑，按主题分组，新会话先读这份再动手。

## 工具链与下载

- **arduino-cli**: 单文件版解压即用（`C:\tools\arduino-cli\arduino-cli.exe`)，
  不必等 winget（慢且可能挂）。
- **IDE 与 CLI 共享数据目录**:Arduino IDE 2.x 装的 core/库在
  `%LOCALAPPDATA%\Arduino15`,arduino-cli 直接可用，无需重复下载。
- **GitHub 直连不可用**（本网络 ~1.5MB/5min，超时必现）。两条活路：
  1. core 索引换乐鑫官方中国镜像（jihulab):
     `https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/master/package/package_esp32_index.template.json`
     （改 `%LOCALAPPDATA%\Arduino15\arduino-cli.yaml` 的 additional_urls)
  2. 但**工具包 zip 仍指向 github.com** — 从乐鑫官方 CDN 手动下载放入暂存目录，
     arduino-cli 校验一致即复用：
     `https://dl.espressif.com/github_assets/<org>/<repo>/releases/download/<tag>/<file>`
     → `%LOCALAPPDATA%\Arduino15\staging\packages\`(CDN 实测 ~100MB/30s)
- 当前版本：esp32:esp32 **3.3.10**(core search 可见；3.3.5 也能用）。

## 编译与上传

- **fqbn 必须带 `CDCOnBoot=cdc`**:`esp32:esp32:esp32c3:CDCOnBoot=cdc`。
  缺了串口完全无输出（C3 原生 USB 的 Serial 走 USB-Serial-JTAG/CDC)。
- **一个 sketch 目录只能有一个 .ino** — 多 .ino 全部参与编译，
  setup/loop 重复定义。参考代码放 `.ino.txt` 或独立目录。
- 上传端口：C3 = VID **0x303A** 的 COM 口（AT-Node 是 0x1A86，别混）。
- 端口被占：Arduino IDE 串口监视器、其他 pyserial 会话都会锁 COM;
  "port busy/not exist" 先查占用，再查 C3 是否掉线重枚举。

## 串口监控（pyserial 驱动）

- **复位脉冲**:`dtr=False; rts=True; sleep(0.5); rts=False; dtr=True`
  让 C3 干净重启（输出从头开始）。
- **别在上传完成前复位** — 会抓到老固件或半截日志（踩过两次）。
- Windows 下 `/tmp` 不存在，日志落地用相对/绝对 Windows 路径。

## BLE 行为（Bluedroid Arduino 封装）

- **`getCharacteristics()` 按 UUID 字符串去重**（std::map)— 同一 UUID 的
  多个特征（如 RK 的 8 个 0x2A4D）只枚举到**任意一个**。要订全部实例？
  这个 API 做不到，换句柄级主机（AT-Node dongle）或 NimBLE。
- **setup 里 `scan->start(5)` 的 5 秒窗口必然错过配对** — loop 里未连接就
  `start(0)` 永久扫描（rk_recon 已示范）。
- sketch 每次启动清 NVS → 对端键盘还存着旧绑定，**每轮测试都要让键盘
  重新进配对模式**（清绑定），否则它广播变脸（无名字/无 HID UUID/定向）
  被名字过滤漏掉。
- 键盘配对模式超时极短（数十秒），节奏：先永久扫描，再让键盘进配对。

## Windows 主机侧 BLE（bleak/WinRT 教训）

- Windows 一旦把 BLE 设备认领为 HID（键盘）,**第三方 GATT 全被挡**:
  设备停止广播，bleak 按地址连接报 "not found"。要 bleak 能连：
  先在 Windows 蓝牙设置里**删除该设备**（解除 HID 占用），让它回广播。
- 换固件新增 GATT 服务后，主机端要"忘记设备"重连刷新服务缓存，
  否则看不到新服务（NUS 事件的教训）。

## C3 可编程键盘 bench（tools/esp32c3_kbd）

> 2026-07-22 实测记录。把 C3 当成 BLE HID 键盘，经 CH582 dongle
> 转发为 USB HID，供自动化输入/回归测试用。

### 库选择：不要高层封装，直接 NimBLE

- **ESP32-BLE-Keyboard**（T-vK）与当前 esp32 core **3.3.10 不兼容**，
  编译报 `std::string` → `String` 转换错误。不可用。
- **ESP32BLECombo** 能编译，但它创建的 **Boot Keyboard Input Report (0x2A22)
  只有 NOTIFY 属性，没有 READ**。CH582 dongle 用 `GATT_ReadUsingCharUUID`
  按值 UUID 发现特征时会读到 `ATT_ERR_READ_NOT_PERMITTED (0x02)`，
  导致 boot 特征找不到，dongle 退回到 report mode 订阅 0x2A4D。
- **结论**：直接调用 **NimBLE-Arduino** API，自己创建 HID service、
  boot input/output、protocol mode，精确控制特征属性。

### Boot 键盘必须满足的条件（CH582 dongle 侧）

1. **Boot Keyboard Input Report (0x2A22)** 属性必须包含 `READ | NOTIFY`。
   - READ 让 dongle 的 Read-Using-Char-UUID 发现成功。
   - NOTIFY 用于后续按键通知。
2. **Protocol Mode (0x2A4E)** 默认值设为 `0x00`（boot protocol）。
   - 若保持库默认 `0x01`（report protocol），dongle 会按 report mode 处理。
3. **Report Map** 使用标准 boot keyboard map（无 Report ID 前缀，8 字节报告）。
4. 服务/特征句柄范围必须在 dongle 可发现的 HID service (0x1812) 内。

### 设备名与扫描

- NimBLE 默认把设备名放在 advertisement 里即可，dongle 扫描能解析到
  `C3-Kbd [HID]`。
- 如果名字解析失败，测试脚本用 **BLE MAC 地址**兜底匹配。
  C3 的串口日志/NimBLE `getAddress()` 给出的是公共地址；空中广播地址
  可能与公共地址差末几位（随机静态地址），以扫描结果中的地址为准。

### HTTP 控制面：不要阻塞 handler

- `/text` 端点如果逐个字符 `delay()` 发键，会阻塞 WebServer 几百毫秒，
  客户端可能 TCP 超时并重试，结果出现**重复打字/乱序/漏键**。
- **正确做法**：handler 只做入队，立即返回 `OK queued`；
  `loop()` 里的 `type_poll()` 用 `millis()` 节拍异步逐字发送。

### 打字速度

- 默认 **40ms 按下 + 30ms 间隔** 在大多数主机上可用；若目标系统漏键，
  用 `--ms` 和 `--gap` 加大间隔。
- 实测较慢但稳的组合：`--ms 60 --gap 100`。
- BLE + dongle 转发链路本身有 10-100ms 抖动，**不能用于延迟指标测试**。

### 连接状态注意

- C3 与 dongle 配对绑定后，dongle 会主动回连；若 C3 重启或刷机，
  旧绑定可能失效，需要 dongle 端 `AT+BT_PAIR` 清绑定后重新扫描连接。
- `AT+BT_DISC` 会断开当前链路并抑制自动回连一次，适合测试脚本做 clean slate。

## RK-S75RGB 专项（详见 software/PLAN.md §3.0)

- boot 特征 0x2A22 是空壳（可订阅，永不发数据）。
- Report Map 331B:5 个 Report ID(ID1 NKRO 16B / ID2 boot-8B /
  ID3 厂商 / ID4 消费控制 / ID5 系统），通知带 ID 前缀。
- 现状：配对成功 + 电量推送正常，按键流零 — 复杂键盘支持归 F1.22(P4)。
