# CH582F 硬件信息（wchble/mr2）

> CH582 平台的硬件规格、引脚分配与硬件层约束。
> 问题与原因记录见 [FIELD-NOTES.md](FIELD-NOTES.md)（实战坑录 F1–F19）；
> 低功耗细节见 [POWER.md](POWER.md)；架构见 [DESIGN.md](DESIGN.md)。

## 芯片规格

| 参数 | 值 |
|------|-----|
| MCU | CH582F（RISC-V rv32imac，60 MHz） |
| Flash | 448 KB |
| RAM | 32 KB（`.highcode` ~8KB 为 WCH RAM 常驻代码，不可占用；BLE 堆 5KB 起） |
| BLE | 4.2/5.0（预编译 `LIB/libCH58xBLE.a`，TMOS 调度） |
| 外设 | GPIO × 38、I²C × 1、SPI × 2、ADC 14ch × 10bit、PWM × 14、USB 2.0 FS |
| 封装 | QFN48 / QFN32 |
| BLE SNV | Data Flash `0x77E00`（最后 512B），1 个绑定设备，新配对覆盖 |

## 默认引脚分配

| 引脚 | 功能 | 备注 |
|------|------|------|
| PA0 | LED1 | 推挽输出，低电平亮 |
| PB22 | KEY1 | 上拉输入，低电平触发 |
| PB4 | KEY2 | 上拉输入，低电平触发 |
| PA9 (TXD1) | UART1 TX | 调试输出（115200 波特率） |
| PA8 (RXD1) | UART1 RX | 调试输入 |
| PB10/PB11 | USB D+/D- | USB 2.0 FS（不可作 GPIO） |
| PB13 | I²C SCL | 上拉，100 kHz |
| PB12 | I²C SDA | 上拉 |
| ADC ch0-13 | 外部模拟输入 | 单端，0–VDD |

AT 命令引脚编号：PA0-PA15 = `0`-`15`，PB0-PB23 = `16`-`39`（详见 [USER-MANUAL.md](USER-MANUAL.md)）。

## USB 端点分配（CDC + HID 复合，IAD）

| EP | 接口 | 类型 | 大小 | 用途 |
|----|------|------|------|------|
| EP0 | — | Control | 64B | 枚举 |
| EP1 | CDC Data | BULK IN/OUT | 64B | AT 命令管道 |
| EP2 | HID Keyboard | Interrupt IN/OUT | 8B | 按键 + LED |
| EP3 | CDC Comm | Interrupt IN | 8B | 串口状态通知 |

VID `1A86` / PID `2107`（CDC ACM）。`usb_dev.c` 是 WCH EVT 官方代码的拷贝，不要重写。

## 硬件层约束

- **休眠与 USB 互斥**：休眠后 USB 外设时钟关闭、枚举丢失（主机报代码 43）。
  `HWS_SLEEP=TRUE` 时编译期禁用 USB。详见 [POWER.md](POWER.md)。
- **GPIO_Pin_All 初始化不干扰 USB D+/D-**（PB10/11）——已由 BleInputStick 证实。
- **BLE 角色在 `BLE_LibInit` 时定死**：DUAL 构建运行期切角色必须软复位。
- **工具链**：只用 MounRiver `riscv-none-embed-gcc`；xPack/上游 GCC 会把
  WCH 快速中断属性编成普通函数（ret 代替 mret），固件首个中断即崩。
  证据见 `tools/ci/TOOLCHAIN.md`。

## 规划芯片：CH592

| 参数 | 值 |
|------|-----|
| 定位 | 更低功耗、更低成本，BLE 5.4 |
| 兼容性 | 引脚兼容 CH582，外设 API 全兼容 |
| 意义 | 若支持休眠态保持 USB，可解除休眠↔USB 互斥（REQUIREMENTS T6.5） |
