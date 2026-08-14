# at-node 使用手册

> AT 命令完整参考。固件变体、每条命令的支持模式、参数与注意事项。
> 约定：命令与参数均为**十进制**(atoi 解析);命令行以 `\r\n` 结束。

## 1. 固件变体与命令可用性

| 变体 | 构建 | 角色 | 说明 |
|------|------|------|------|
| kbd | `make main-build` | 单模键盘(BLE Peripheral + USB) | 默认 |
| **kbd_multi** | `make main-build MODE=KBD_MULTI` | 多模键盘(3 槽主机) | 主力形态 |
| dongle | `make main-build DONGLE=1` | BLE 接收器(Central)→USB HID | 把 BLE 键盘桥接到主机 |
| dual | `make main-build MODE=DUAL` | kbd+dongle 调试(AT+ROLE 切换) | 仅开发 |

命令可用性标记:**K**=kbd/kbd_multi 都有,**M**=kbd_multi 特有,**D**=dongle 特有,**ALL**=全部变体。

通道:USB CDC 是唯一 AT 端口(UART1 仅调试输出)。

## 1.1 命令支持矩阵

### 符号与通用参数约定

**支持状态**:✅ 支持 · — 不支持 · △ DUAL 下按运行时角色

**通用参数**(表中不再重复展开):

| 参数 | 含义 |
|------|------|
| `<mods>` | 修饰键掩码(可叠加):`1`=左Ctrl `2`=左Shift `4`=左Alt `8`=左Win/Cmd |
| `<k>` | HID 键码(十进制):字母 `a-z`=4-29,数字 `1-0`=30-39,回车 `40`,空格 `44`,F1-F12=58-69 |
| `<pin>` | 引脚编号:PA0-PA15=`0`-`15`,PB0-PB23=`16`-`39` |
| `<BLEn>` | 槽位名:`BLE1` / `BLE2` / `BLE3` |
| `<addr>` | MAC 地址格式 `AA:BB:CC:DD:EE:FF`(大写十六进制,冒号分隔) |
| 转义 | KEY_STR 内:`\n`=回车 `\t`=Tab `\\`=反斜杠(命令行内不能嵌真换行) |
| 数字 | **全部十进制**(除 MAC 为十六进制) |

### COMMON(系统)

| 命令 | 格式 | KBD | KBD_MULTI | DONGLE | DUAL | 说明 |
|------|------|-----|-----------|--------|------|------|
| AT | `AT` | ✅ | ✅ | ✅ | ✅ | 握手,返回 OK |
| AT+VER | `AT+VER` | ✅ | ✅ | ✅ | ✅ | 版本+角色标签 `[kbd\|dongle]` |
| AT+HELP | `AT+HELP[=<CMD>]` | ✅ | ✅ | ✅ | ✅ | 无参=分组列表;带参=单条用法 |
| AT+STATUS | `AT+STATUS` | ✅ | ✅ | ✅ | ✅ | `role= dev= ble= batt=` 一行 |
| AT+ECHO | `AT+ECHO=<text>` | ✅ | ✅ | ✅ | ✅ | 回显,链路自检 |
| AT+RST | `AT+RST` | ✅ | ✅ | ✅ | ✅ | 软件复位 |
| AT+FACTORY | `AT+FACTORY` | ✅ | ✅ | ✅ | ✅ | 清全部绑定+槽位配置,软复位,**不可恢复** |
| AT+ISP | `AT+ISP` | ✅ | ✅ | ✅ | ✅ | 进 ISP 下载模式(擦应用区) |
| AT+WDG | `AT+WDG[=0\|1]` | ✅ | ✅ | ✅ | ✅ | 看门狗,默认关;武装后 0.56s 无喂狗复位 |
| AT+ROLE | `AT+ROLE[=KBD\|DONGLE]` | — | — | — | ✅ | 仅 dual:切运行时角色并软复位 |

### KBD(键盘输出)

| 命令 | 格式 | KBD | KBD_MULTI | DONGLE | DUAL | 说明 |
|------|------|-----|-----------|--------|------|------|
| AT+DEV | `AT+DEV[=USB\|BLE]` (单模) `AT+DEV[=USB\|BLE1\|BLE2\|BLE3]` (多模) | ✅ | ✅ | — | △ | 输出目标查询/切换;多模单活动链路,切即断旧连新 |
| AT+TAP | `AT+TAP=<ms>,<mods>,<k1>..<k6>` | ✅ | ✅ | ✅ | ✅ | 点按(原子按下+释放,**推荐注入方式**);例 `AT+TAP=80,0,4`=a |
| AT+KEY | `AT+KEY=<mods>,<k1>..<k6>` | ✅ | ✅ | ✅ | ✅ | 裸按下,仅限按住场景;必须补 `AT+KEY=0,0` 释放 |
| AT+MOD | `AT+MOD=<mask>` | ✅ | ✅ | ✅ | ✅ | 按住/释放修饰键,`0`=全放 |
| AT+KEY_STR | `AT+KEY_STR=<text>` | ✅ | ✅ | ✅ | ✅ | US 布局打字符串,完成报 `+KEY_DONE`;支持 `\n` `\t` `\\` |
| AT+KEY_SEQ | `AT+KEY_SEQ=<delay>,<mods>,<k1>..<k6>,...` | ✅ | ✅ | ✅ | ✅ | 批量 HID 序列,组间 delay(ms) |
| AT+PACE | `AT+PACE[=<ms 5-2000>]` | ✅ | ✅(每槽) | — | △ | KEY_STR 节奏,默认 30;法则 pace ≥ 2×连接间隔,持久 |
| AT+NAME | `AT+NAME[=<BLEn>,<label>]` | — | ✅ | — | △ | 槽位助记名(≤11 字符 A-Za-z0-9-_),持久 |
| AT+MAC | `AT+MAC[=<BLEn>[,<addr>]]` | — | ✅ | — | △ | 槽位本机 MAC,默认芯片MAC+槽号,持久 |
| AT+LED | `AT+LED=ON\|OFF\|BLINK\|FLASH\|TOGGLE[,<ms>[,<duty%>]]` | ✅ | ✅ | — | △ | 板载 LED(PA0) |
| AT+KEY_CFG | `AT+KEY_CFG=<pin 38\|39>,<mods>,<k>` | ✅ | ✅ | — | △ | 自定义物理按键映射,持久 |

### BLE-KBD(kbd 角色的配对管理)

| 命令 | 格式 | KBD | KBD_MULTI | DONGLE | DUAL | 说明 |
|------|------|-----|-----------|--------|------|------|
| AT+BT_PAIR | 单模:`AT+BT_PAIR` 多模:`AT+BT_PAIR[=<BLEn>]` | ✅ | ✅ | — | △ | 多模=清该槽预留+开 **60s 配对窗**(不开窗未知主机拒连) |
| AT+BT_DISC | `AT+BT_DISC[=<BLE1\|BLE2\|BLE3>]` | ✅ | ✅ | — | △ | 断开链路,绑定保留可回连 |
| AT+BT_UNBIND | `AT+BT_UNBIND=<BLE1\|BLE2\|BLE3>` | — | ✅ | — | △ | **忘记一台主机**:清预留+绑定 |

### BLE-DONGLE(接收器)

| 命令 | 格式 | KBD | KBD_MULTI | DONGLE | DUAL | 说明 |
|------|------|-----|-----------|--------|------|------|
| AT+BT_SCAN | `AT+BT_SCAN=<sec 1-30>[,<filter HID\|名字>]` | — | — | ✅ | △ | filter:`HID`=仅HID标记,或名字子串;按 RSSI 排序输出 |
| AT+BT_CONN | `AT+BT_CONN=mac,<addr>[,<sec>]` `AT+BT_CONN=name,<子串>[,<sec>]` `AT+BT_CONN=index,<n>` | — | — | ✅ | △ | mac/name=扫描匹配(默认 5s 超时);index=立即连扫描列表(先 AT+BT_SCAN);**同名设备多用 mac 形式** |
| AT+BT_DISC | `AT+BT_DISC` | — | — | ✅ | △ | 断链并 hold 自动重连一次 |
| AT+BT_AUTO | `AT+BT_AUTO[=0\|1]` | — | — | ✅ | △ | 自动重连开关(掉线直连已绑定地址) |
| AT+BT_LIST | `AT+BT_LIST` | — | — | ✅ | △ | 已绑定键盘列表 |
| AT+BT_PAIR | `AT+BT_PAIR` | — | — | ✅ | △ | 清本机绑定记录 |
| AT+BT_BATT | `AT+BT_BATT` | — | — | ✅ | △ | 读对端键盘电量 → `+BT_BATT:<pct>%` |
| AT+BT_STATE | `AT+BT_STATE` | — | — | ✅ | △ | 状态机诊断 |
| AT+BT_PASSKEY | `AT+BT_PASSKEY=<6digits>` | — | — | ✅ | △ | SMP 配对码,默认 123456 |

### GPIO / 硬件

| 命令 | 格式 | KBD | KBD_MULTI | DONGLE | DUAL | 说明 |
|------|------|-----|-----------|--------|------|------|
| AT+GPIO_W | `AT+GPIO_W=<pin>,<level 0\|1>[,<drive 5\|20>]` | ✅ | ✅ | ✅ | ✅ | 推挽输出,驱动默认 5mA,可选 20mA |
| AT+GPIO_R | `AT+GPIO_R=<pin>[,<mode 0\|1\|2>]` | ✅ | ✅ | ✅ | ✅ | mode:`0`=上拉(默认) `1`=浮空 `2`=下拉 |
| AT+ADC | `AT+ADC=<ch 0-13>[,<pga>]` | ✅ | ✅ | ✅ | ✅ | → `+ADC:<raw>,<mV>mV`,Vref 已校准 |
| AT+TEMP | `AT+TEMP` | ✅ | ✅ | ✅ | ✅ | 片内温度 → `+TEMP:<raw>,<C>C` |
| AT+SLEEP | `AT+SLEEP=<mode 0-2>[,<sec 1-3600>]` | △¹ | △¹ | △¹ | △¹ | mode:`0`=Idle `1`=Sleep `2`=Shutdown(RAM 保持,RTC 定时唤醒);¹**USB 构建直接拒绝**,仅 HWS_SLEEP 电池构建 |

---

---

## 2. 系统命令(ALL)

### AT
握手。返回 `OK`。

### AT+VER
固件版本 + 角色标签:`AT-Node v1.0 [kbd|dongle] BLE: CH58x_BLE_LIB_V2.12`。

### AT+HELP / AT+HELP=<CMD>
分组命令列表 / 单条命令用法。

### AT+STATUS
设备状态一行:`role=kbd dev=<目标> ble=<n>conn batt=<mV>mV`。

### AT+ECHO=<text>
回显文本，链路自检用。

### AT+RST
软件复位。

### AT+FACTORY
**出厂复位**:断开全部链路，擦除全部 BLE 绑定 + 槽位配置(预留/名字/节奏/MAC),软复位。不可恢复。

### AT+ISP
进入 ISP 下载模式(会擦除应用区，刷机用)。

### AT+WDG[=0|1]
看门狗开关(默认关)。武装后 0.56s 不喂狗即复位，用于死锁自愈。RAM 态，重启回关。

### AT+ROLE[=KBD|DONGLE]  (仅 dual 构建)
查询/切换运行时角色(写 DataFlash + 软复位)。

---

## 3. 键盘输出(K;M 支持多槽路由)

### AT+TAP=<ms>,<mods>,<k1>..<k6>
**点按**(原子按下+释放，推荐注入方式)。`ms`=按住时长,`mods`=修饰键掩码(1=LCtrl 2=LShift 4=LAlt 8=LGUI),`k`=HID 键码。
例:`AT+TAP=80,0,4` = 按一下 a;`AT+TAP=150,1,6` = Ctrl+C。

### AT+KEY=<mods>,<k1>,..,<k6>
裸 HID 报告(按下态)。**仅限需要按住的场景**(组合键按住阶段),用完必须 `AT+KEY=0,0` 释放，否则主机判定长按刷屏。

### AT+MOD=<mask>
设置修饰键(按住),`AT+MOD=0` 释放。

### AT+KEY_STR=<text>
按 US 布局打字符串(独立 TMOS 回放任务,`+KEY_DONE` 完成 URC)。
转义:`\n`=回车,`\t`=Tab,`\\`=反斜杠。
注意:中文输入法的输入框会把拼音组字(切英文模式)。

### AT+KEY_SEQ=<delay>,<mods>,<k1>..<k6>,...
批量 HID 序列回放(多组报告，组间 delay ms)。

### AT+DEV[=<target>]
输出目标查询/切换。
- kbd:`USB|BLE`
- kbd_multi:`USB|BLE1|BLE2|BLE3`(**单活动链路**:仅目标槽持有连接,切换即断旧连新，预留主机自动回连 0.1~3s)
查询输出(索引行首):`1,USB,ready[,active]` / `2,BLE1,<MAC>,connected,secure,notify,active,err=NN,int=22.50ms/lat0,name=Raspi` / `3,BLE2,<MAC>,-,free|reserved`

### AT+PACE[=<ms>]  (M,持久)
当前槽 KEY_STR 节奏(报告间隔 ms)。默认 30。**法则:pace ≥ 2×连接间隔**;手机链路(30ms)需 ≥60,dongle/Windows(21-22.5ms)30 即可,15 实测丢包。

### AT+NAME[=<BLEn>,<label>]  (M,持久)
槽位助记名(≤11 字符 A-Za-z0-9-_),AT+DEV 显示 `name=`。例:`AT+NAME=BLE1,Raspi`。

### AT+MAC[=<BLEn>[,<AA:BB:CC:DD:EE:FF>]]  (M,持久)
槽位本机 MAC。默认芯片 MAC+槽号(BLE1=芯片原 MAC)。自定义须静态随机型(MSB 高两位=11)。**名字与广播地址随活动槽 MAC 联动**:`AT-Node-XXXX-N`。

### AT+BT_PAIR[=<BLEn>]  (M)
**开 60s 配对窗**(市面键盘范式：不开窗未知主机拒连)。带槽参数时先清该槽预留。**配对即指向**:窗口在该槽的 MAC 上广播 `AT-Node-XXXX-N`。

### AT+BT_DISC[=<BLE1|BLE2|BLE3>]
断开链路。无参=全部断开(绑定保留，自动重连)。

### AT+BT_UNBIND=<BLE1|BLE2|BLE3>  (M)
**忘记一台主机**:清其预留 + 绑定记录。

### AT+LED=<ON|OFF|BLINK|FLASH|TOGGLE>[,<ms>[,<duty%>]]
板载 LED 控制(PA0)。

### AT+KEY_CFG=<pin 38|39>,<mods>,<keycode>
自定义物理按键映射(持久到 DataFlash)。

---

## 4. dongle 接收器(D)

### AT+BT_SCAN=<sec>[,<filter>]
扫描 BLE 设备。filter:`HID`=仅 HID 标记，或名字子串(大小写不敏感)。输出按 RSSI 排序:`+BT_SCAN:<idx>,<addr12hex>,<rssi>,<name> [HID]`。

### AT+BT_CONN=<形式>[,<sec>]
连接键盘，三种形式:
- `mac,AA:BB:CC:DD:EE:FF[,sec]` — 扫描匹配(默认 5s 超时)
- `name,<子串>[,sec]` — 扫描匹配(同上)
- `index,<n>` — 立即连扫描列表第 n 项(需先 AT+BT_SCAN)
**多设备同名时务必用 MAC**(AT-Node-ESP 等仿冒前缀会劫持按名连接)。

### AT+BT_DISC
断开当前链路(hold 住自动重连一次)。

### AT+BT_AUTO[=0|1]
自动重连开关。断开/掉线后自动直连已绑定地址。

### AT+BT_LIST
已绑定键盘列表。

### AT+BT_PAIR
清除本机绑定(重新配对用)。

### AT+BT_BATT
读对端键盘电量:`+BT_BATT:<pct>%`(连接后自动发现 0x180F/0x2A19)。

### AT+BT_STATE
诊断:dongle 状态机/建链结果。

### AT+BT_PASSKEY=<6digits>
SMP 配对码(默认 123456 自动)。

### AT+BT_STATE / +BT_NTF
收到 HID 报告的 URC:`+BT_NTF:h=<handle> l=<len> <bytes...>`。

---

## 5. 硬件(K,宏门控)

### AT+GPIO_W=<pin>,<level>[,<drive 5|20>]
写引脚(推挽输出,驱动 5mA 默认/20mA)。pin:PA0-15=0-15,PB16-39。

### AT+GPIO_R=<pin>[,<mode 0|1|2>]
读引脚。mode:0=上拉(默认),1=浮空,2=下拉。

### AT+ADC=<ch>[,<pga>]
ADC 读数:`+ADC:<raw>,<mV>mV`。ch 0-13;PGA 增益可选(校准过 Vref)。

### AT+TEMP
片内温度:`+TEMP:<raw>,<C>C`。

### AT+SLEEP=<mode 0-2>[,<sec>]
RTC 定时休眠(0=Idle 1=Sleep 2=Shutdown,RAM 保持)。**USB 构建下拒绝执行**——仅电池/HWS_SLEEP 构建(蓝牙场景,UART 控制)。

---

## 6. 关键行为模型(异同速查)

| 行为 | kbd | kbd_multi | dongle |
|------|-----|-----------|--------|
| 连接形态 | 1 主机 | 3 槽,**单活动** | 1 键盘 |
| 切换 | AT+DEV=USB\|BLE | AT+DEV=槽,即切即连 | — |
| 配对 | 开窗 60s | 开窗 60s(每槽) | 主动连 |
| 重连 | 预留自动回连(HDC 定向补发) | 同左 | AT+BT_AUTO 自动 |
| 本机地址 | 芯片 MAC | 每槽独立 MAC | — |
| 电池 | 自身 ADC(batt=mV) | 同左 | 读对端(AT+BT_BATT) |
| 节奏 | pace=30 全局 | pace 每槽持久 | — |

### 状态字段(AT+DEV 每槽)
`connected/reserved/free` → `secure`(加密) → `notify`(CCCD 已订阅) → `active`(当前目标) → `err=NN`(最近发送错误码) → `int=XXms/latY`(协商参数) → `drops`(丢包计数) → `name=`(助记)。

### 常见 err 码
`00` 正常;`0F`(15)=bleNoResources(对端未订阅 CCCD);`10`(16)=bleNotReady(槽空/未加密);`16`(0x16)=blePending(发送队列满)。

---

## 7. 典型工作流

**新主机配对(kbd_multi)**:
```
AT+BT_PAIR=BLE2          # 开窗(广播名 AT-Node-XXXX-2)
  → 主机搜 AT-Node-XXXX-2 配对
AT+NAME=BLE2,Laptop      # 起名
AT+PACE=60               # 手机类慢链路设节奏
```

**dongle 桥接**:
```
AT+BT_CONN=mac,E0:4E:7A:8C:13:76,10   # 按 MAC 连键盘
AT+BT_AUTO=1                           # 自动重连
AT+BT_BATT                             # 读键盘电量
```

**打字注入(agent)**:
```
AT+DEV=BLE2
AT+KEY_STR=echo hello >> /tmp/x.log\n  # \n 自动回车执行
# 等 +KEY_DONE 再发下一条
```
