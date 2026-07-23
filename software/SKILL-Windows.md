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

## RK-S75RGB 专项（详见 software/PLAN.md §3.0)

- boot 特征 0x2A22 是空壳（可订阅，永不发数据）。
- Report Map 331B:5 个 Report ID(ID1 NKRO 16B / ID2 boot-8B /
  ID3 厂商 / ID4 消费控制 / ID5 系统），通知带 ID 前缀。
- 现状：配对成功 + 电量推送正常，按键流零 — 复杂键盘支持归 F1.22(P4)。
