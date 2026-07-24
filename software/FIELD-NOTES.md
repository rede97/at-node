# FIELD-NOTES.md — 实战坑录（按日期归档)

> 调试中踩过的坑、根因、修法、排障手法。新坑按日期追加，不要删旧账。
> 相关:SKILL-Linux.md(环境/工具坑)、TEST-TODO.md(待测项)。

## 2026-07-24 多模键盘(KBD_MULTI)攻关

### F1. SNV 布局溢出导致启动死锁(最隐蔽)

- **现象**:kbd_multi 固件"刷得进、不起飞",USB 完全不枚举,无 dmesg attach 事件
- **根因**:`BLE_SNV_ADDR` 固定值(0x7E00)只容 1 个 256B 绑定块;`BLE_SNV_NUM=3` 时
  `ble_stack_init()` 的运行时守卫(addr + block×num > 0x8000)命中 → USB 初始化**之前** while(1)
- **排障手法**:wlink halt + `wlink regs` 多次采样,PC 在 `TMOS_SystemProcess` 内变化
  → 固件活着、主循环在跑 → 问题在"启动早期分支",不是整体崩溃
- **修法**:SNV 地址改为按 BLOCK×NUM 自动锚定 Data Flash 顶部;加**编译期 #error** 前置拦截
- **教训**:数据 Flash 布局变更要同时算清"守卫公式";编译期检查优于运行时死锁

### F2. 单连接假设三处(多连接改造核心)

1. `ble_hid_emu_conn_handle` / `gapConnHandle` 单全局 → 槽位表(连接事件 opcode 驱动,与 GAP 状态机解耦)
2. **CONNECTED 即停广播**(WCH 例程惯例)→ 未满槽时必须**显式重新开广播**;
   只判断"不关广播"不够——GAPRole 库连接时默认会停
3. 加密标志/报告发送按句柄路由;CCCD 是 GATT 服务器按连接存的,直接可用

### F3. DEV=ALL 广播互踩 → 单活动链路模型(架构决策)

- **现象**:同一 tick 向两条链路发通知,**两个主机都收不到**
- **根因**:WCH 栈内背靠背 GATT 通知跨连接互踩(未深究,绕过)
- **决策**:删除 DEV=ALL,改**单活动链路模型**(市面产品共性):只有目标槽允许持有链路,
  其他已绑定主机连接即 TerminateLink;未绑定主机永远放行(配对友好)
- **收益**:省电(空闲链路零空口)、消灭整类多链路复杂度(参数调优/队列风暴/广播互踩)

### F4. 槽位持久化(slotmap)的时序坑

- **现象**:dongle 的预留槽位被莫名清除,策略放行了应拒绝的主机
- **根因**:断开处理里"未绑定主机清除预留"的逻辑,读取 `hidDevConnSecure` 时
  **profile 层已经先一步把它清了**(回调链顺序)→ 每个主机看起来都"未绑定"
- **修法**:删除自动清除,预留只能显式 `AT+BT_UNBIND` 清理
- **教训**:跨层读取"状态快照"要注意回调链谁先谁后;状态要用自己层的缓存

### F5. 手机打字丢键(最长的战线)

- **现象**:`KEY_STR=hello` 手机收到 `hhhhhe`/`hewo`/`ph`;慢速单键(600ms)100% 正常
- **根因链**:
  1. KEY_STR 以 15ms 节拍连发 → 手机链路排不动 → 释放报告丢失 → 手机判定长按自动重复
  2. 失败码不是猜的 blePending,而是 bleNotReady=0x10/bleNoResources=0x0F(WCH 码表与直觉不同,查 LIB 头文件!)
  3. 手机连接间隔可能先 180ms 后 30ms(参数更新 12.8s 后才协商到位)——**要读真实协商值,别猜**
- **修法**:per-slot `AT+PACE`(随预留持久化);经验法则 **pace ≥ 2×连接间隔**;
  AT+DEV 显示每槽 `int=XXms/latY` + `err=NN` + `drops`,链路问题可自诊断
- **排障手法**:快慢对照测试定位"吞吐量 vs 功能";错误码透传到 AT 输出再分析

### F6. AT 参数是十进制(atoi)

- 测试脚本把 `0x14` 写进命令串 → atoi 得 20 而非 14;`0x0A` → 0(哑键)
- **教训**:测试脚本与固件打印(十六进制)之间的进制转换是独立 bug 源,
  一度伪装成"广播丢包"。工具脚本里显式注释十进制约定

### F7. RPA(手机地址轮换)

- Android 每次重连可能换 RPA 地址 → 按地址认槽会失败
- **修法**:`GAPBOND_AUTO_SYNC_RL`(解析列表自动同步),已绑定主机的连接事件
  上报**身份地址**(稳定),槽位绑定表才有意义

### F8. 安卓缓存设备名

- 改广播名(如加槽位后缀)后,已配对手机**永远显示旧名**
- 必须"忽略设备 → 重新搜索配对"才能看到新名

### F14. 广播 AD 结构偏移错误(名字变乱码)

- **现象**:广播有 MAC 无名字,nRF Connect 显示名称为空/乱码,dongle 扫描名 `?`
- **根因**:重写名字段时 memcpy 从 `advertData[8]` 开始,冲掉了 AD type 字节(0x09);
  正确应从偏移 9(length@7, type@8, chars@9)
- **排障手法**:dongle 板扫描看到 `+BT_ADV:...,len=15`(应为 21)→ 数据被截/错位;
  按 MAC 不过滤地扫,确认"在广播但数据坏"
- **教训**:手写 AD 结构偏移极易错;改完立刻用 nrfConnect/dongle 双端验证广播名

## 2026-07-24 Linux 蓝牙主机测试环境(VMware)

### F9. BlueZ uhid 抖动(不可根治,环境级)

- **现象**:evdev 能收到键,过一会儿静默归零;内核日志显示 AT-Node 输入设备
  **每 1-2 分钟销毁重建**(input16→17→18→19...)
- **取证**:btmon 证明通知已到达适配器(`Handle 0x0032 Data 00001400...`)→ 固件无罪
- **结论**:VM + BlueZ 的 HID 输入通道不稳定,重配对无效;**换确定性主机**(dongle 板/手机),
  Linux 腿标记为环境问题

### F10. 权限连环(Linux BT 调试)

| 需求 | 解法 |
|------|------|
| bluetoothctl agent 注册被拒 | 自写 dbus-next agent:`tools/bt_agent.py` |
| evdev 读取 | `sudo usermod -aG input mxq`(+newgrp/重登录) |
| btmon 抓包 | `sudo setcap 'cap_net_admin,cap_net_raw+eip' $(readlink -f $(which btmon))` |
| bluetooth 组 | `sudo usermod -aG bluetooth mxq` |

- **注意**:btmon 权限不足时静默零输出,会给出"板子没发"的**假证据**,先验证工具链再下结论

## 2026-07-22 看门狗/CDC/睡眠(补录)

### F11. CDC 死等 EP1 ACK → 复位循环

- 主机掉线时 `USB_CDC_Write` 无限等端点 ACK → 看门狗复位
- 修法:2KB 环形缓冲 + 独立 TMOS 任务异步排空

### F12. AT+SLEEP 与 USB 的关系(设计决策)

- BLE 栈深睡(HWS_SLEEP):与 USB **编译期互斥**(#error)
- 运行期 AT+SLEEP:**USB 构建直接拒绝**——拆装 USB 协议栈的恢复面太大且无场景

### F13. 编辑事故的教训(两次)

- 批量编辑"部分成功静默丢弃",二进制里 grep 字符串造成"代码在"的假象
- **规矩**:改完必须 grep `.c` 源文件 + 编译验证;优先用精确 edit 工具而非脚本批量替换
